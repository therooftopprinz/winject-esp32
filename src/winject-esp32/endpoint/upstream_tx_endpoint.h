#ifndef WINJECT_UPSTREAM_TX_ENDPOINT_H_
#define WINJECT_UPSTREAM_TX_ENDPOINT_H_

#include "config.h"
#include "packet.h"

#include <atomic>
#include <stdint.h>

#include "bfc-esp32/semaphore.hpp"
#include "esp_eth.h"
#include "esp_netif.h"

class wifi_tx;

// L2 UDP hijack on EMAC input → wifi_tx queue (see docs/flow_refactor.md).

struct upstream_tx_bind_s
{
    uint16_t udp_port;
    bool socket_open;
    bool null_sink;
    uint32_t drop_no_pkt_pool;
    uint32_t drop_queue_full;
    uint16_t pool_free_min;
    // EMAC input path (split mgr_wifi→radio_udp loss):
    // eth_rx_cb = driver delivered frame to eth_input_cb.
    // eth_inject_l2 = UDP dst matched inject port (before wifi_tx).
    // Gap (mgr air TX − eth_inject_l2) ≈ silent EMAC/DMA loss before callback.
    // drop_queue_full ≈ WiFi DMA backpressure after L2 hijack.
    uint32_t eth_rx_cb;
    uint32_t eth_inject_l2;
    uint32_t eth_inject_len_drop;
    uint32_t eth_inject_null_sink;
};

class upstream_tx_endpoint
{
public:
    static upstream_tx_endpoint& instance();
    upstream_tx_endpoint(const upstream_tx_endpoint&) = delete;
    upstream_tx_endpoint& operator=(const upstream_tx_endpoint&) = delete;

    bool init(wifi_tx& tx);

    void attach_eth_input(esp_eth_handle_t eth, esp_netif_t* netif);

    bool set_endpoint(uint16_t udp_port);
    bool clear();
    bool get_status(upstream_tx_bind_s* out);
    void set_null_sink(bool enabled);
    bool null_sink() const;
    void reset_pool_free_min();

private:
    upstream_tx_endpoint() = default;

    static esp_err_t eth_input_cb(esp_eth_handle_t eth, uint8_t* buffer,
                                  uint32_t length, void* priv, void* info);

    void clear_locked();
    void note_pool_free(size_t free_n);
    bool accept_packet(packet&& pkt);
    bool try_hijack_udp(uint8_t* buffer, uint32_t length);

    bool used = false;
    std::atomic<uint16_t> inject_port_{0};
    esp_eth_handle_t eth_ = nullptr;
    esp_netif_t* netif_ = nullptr;
    std::atomic<uint32_t> drop_no_pkt_pool{0};
    std::atomic<uint32_t> drop_queue_full{0};
    std::atomic<uint32_t> eth_rx_cb{0};
    std::atomic<uint32_t> eth_inject_l2{0};
    std::atomic<uint32_t> eth_inject_len_drop{0};
    std::atomic<uint32_t> eth_inject_null_sink{0};
    std::atomic<uint16_t> pool_free_min{UINT16_MAX};
    std::atomic<bool> null_sink_{false};
    wifi_tx* tx = nullptr;
    bfc::semaphore lock;
};

#endif  // WINJECT_UPSTREAM_TX_ENDPOINT_H_
