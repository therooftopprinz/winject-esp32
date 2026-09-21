#ifndef WINJECT_LC_TX_ENDPOINT_H_
#define WINJECT_LC_TX_ENDPOINT_H_

#include "config.h"
#include "packet.h"

#include <atomic>
#include <stdint.h>

#include "bfc-esp32/semaphore.hpp"
#include "esp_eth.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class wifi_tx;

struct lc_tx_bind_s
{
    uint16_t udp_port;
    bool socket_open;
    bool null_sink;
    uint32_t drop_no_pkt_pool;
    uint32_t drop_queue_full;
    uint16_t pool_free_min;
};

class lc_tx_endpoint
{
public:
    static lc_tx_endpoint& instance();
    lc_tx_endpoint(const lc_tx_endpoint&) = delete;
    lc_tx_endpoint& operator=(const lc_tx_endpoint&) = delete;

    bool init(wifi_tx& tx);
    // Flush staging → wifi_tx. Inject frames are intercepted in the EMAC
    // input path (bypass lwIP) when sut port is set.
    bool start(BaseType_t core = APP_TASK_CORE,
               UBaseType_t prio = LC_TX_DRAIN_TASK_PRIO,
               uint32_t stack_bytes = LC_TX_DRAIN_TASK_STACK);

    // Install after esp_netif_attach so we wrap eth→netif delivery.
    void attach_eth_input(esp_eth_handle_t eth, esp_netif_t* netif);

    bool set_endpoint(uint16_t udp_port);
    bool clear();
    bool get_status(lc_tx_bind_s* out);
    void set_null_sink(bool enabled);
    bool null_sink() const;
    void reset_pool_free_min();

    // Runtime ETH→WiFi timeshare (defaults from config.h; tunable via console).
    uint8_t flush_batch() const;
    uint8_t emac_gap_ticks() const;
    uint8_t staging_pressure_margin() const;
    bool set_flush_batch(uint8_t n);
    bool set_emac_gap_ticks(uint8_t n);
    bool set_staging_pressure_margin(uint8_t n);

    // Console tx_grant: manager asks how many inject datagrams may follow.
    void reset_inject_grant();
    void issue_inject_grant(uint16_t n);
    uint16_t inject_grant_remaining() const;
    uint32_t inject_grant_denied() const;

private:
    bool take_inject_grant();
    lc_tx_endpoint() = default;

    struct staging_slot
    {
        uint16_t len = 0;
        uint8_t data[WIFI_RADIO_INJECT_MAX]{};
    };

    static void flush_task(void* arg);
    static esp_err_t eth_input_cb(esp_eth_handle_t eth, uint8_t* buffer,
                                  uint32_t length, void* priv, void* info);

    void clear_locked();
    void note_pool_free(size_t free_n);
    int flush_staging_to_wifi(int max_move);
    void handle_datagram(const uint8_t* data, uint16_t len);
    bool try_hijack_udp(uint8_t* buffer, uint32_t length);

    bool used = false;
    std::atomic<uint16_t> inject_port_{0};
    esp_eth_handle_t eth_ = nullptr;
    esp_netif_t* netif_ = nullptr;
    std::atomic<uint32_t> drop_no_pkt_pool{0};
    std::atomic<uint32_t> drop_queue_full{0};
    std::atomic<uint32_t> drop_inject_grant{0};
    std::atomic<bool> inject_grant_enforce_{false};
    std::atomic<uint16_t> inject_grant_remaining_{0};
    std::atomic<uint16_t> pool_free_min{UINT16_MAX};
    std::atomic<bool> null_sink_{false};
    wifi_tx* tx = nullptr;
    bfc::semaphore lock;
    bool started = false;

    staging_slot staging_[LC_TX_STAGING_DEPTH]{};
    std::atomic<uint8_t> staging_head_{0};
    std::atomic<uint8_t> staging_tail_{0};
    std::atomic<uint8_t> staging_count_{0};
    std::atomic<uint8_t> flush_batch_{LC_TX_FLUSH_BATCH};
    std::atomic<uint8_t> emac_gap_ticks_{LC_TX_EMAC_GAP_TICKS};
    std::atomic<uint8_t> staging_pressure_margin_{4};
    TaskHandle_t flush_task_handle_{nullptr};
};
#endif  // WINJECT_LC_TX_ENDPOINT_H_
