#include "upstream_tx_endpoint.h"

#include "config.h"
#include "packet.h"
#include "wifi.h"
#include "wifi_tx.h"

#include <stdlib.h>
#include <string.h>
#include <utility>

#include "esp_log.h"

static const char* TAG = "upstream_tx";

static bool match_udp_dst(const uint8_t* buffer, uint32_t length, uint16_t port,
                          const uint8_t** payload_out, uint16_t* payload_len_out)
{
    if (port == 0 || buffer == nullptr || length < 42 || payload_out == nullptr ||
        payload_len_out == nullptr)
    {
        return false;
    }
    if (buffer[12] != 0x08 || buffer[13] != 0x00)
    {
        return false;
    }
    const uint8_t* ip = buffer + 14;
    const uint8_t ver_ihl = ip[0];
    if ((ver_ihl >> 4) != 4)
    {
        return false;
    }
    const uint32_t ihl = static_cast<uint32_t>(ver_ihl & 0x0Fu) * 4u;
    if (ihl < 20 || length < 14u + ihl + 8u)
    {
        return false;
    }
    if (ip[9] != 17)
    {
        return false;
    }
    const uint8_t* udp = ip + ihl;
    const uint16_t dst =
        static_cast<uint16_t>((udp[2] << 8) | udp[3]);
    if (dst != port)
    {
        return false;
    }
    const uint16_t udp_len =
        static_cast<uint16_t>((udp[4] << 8) | udp[5]);
    if (udp_len < 8 || 14u + ihl + udp_len > length)
    {
        return false;
    }
    *payload_len_out = static_cast<uint16_t>(udp_len - 8u);
    *payload_out = udp + 8;
    return true;
}

upstream_tx_endpoint& upstream_tx_endpoint::instance()
{
    static upstream_tx_endpoint inst;
    return inst;
}

void upstream_tx_endpoint::note_pool_free(size_t free_n)
{
    const uint16_t v =
        free_n > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(free_n);
    uint16_t prev = pool_free_min.load(std::memory_order_relaxed);
    while (v < prev &&
           !pool_free_min.compare_exchange_weak(prev, v,
                                                std::memory_order_relaxed))
    {
    }
}

bool upstream_tx_endpoint::accept_packet(packet&& pkt)
{
    if (!pkt.is_valid())
    {
        return false;
    }
    const size_t len = pkt.size();
    if (len < WIFI_RADIO_INJECT_MIN || len > WIFI_RADIO_INJECT_MAX)
    {
        eth_inject_len_drop.fetch_add(1, std::memory_order_relaxed);
        pkt.reset();
        return false;
    }
    if (null_sink_.load(std::memory_order_relaxed))
    {
        eth_inject_null_sink.fetch_add(1, std::memory_order_relaxed);
        pkt.reset();
        return true;
    }
    if (tx == nullptr)
    {
        drop_queue_full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (tx->queue_full())
    {
        drop_queue_full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!tx->enqueue(std::move(pkt)))
    {
        drop_queue_full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    wifi::instance().note_udp_tx_pkt();
    note_pool_free(packet_allocator::tx().available());
    return true;
}

bool upstream_tx_endpoint::try_hijack_udp(uint8_t* buffer, uint32_t length)
{
    const uint16_t port = inject_port_.load(std::memory_order_acquire);
    const uint8_t* payload = nullptr;
    uint16_t payload_len = 0;
    if (!match_udp_dst(buffer, length, port, &payload, &payload_len))
    {
        return false;
    }
    eth_inject_l2.fetch_add(1, std::memory_order_relaxed);
    packet pkt = packet::adopt_eth_frame(buffer, payload,
                                         static_cast<size_t>(payload_len));
    if (!pkt.is_valid())
    {
        drop_no_pkt_pool.fetch_add(1, std::memory_order_relaxed);
        free(buffer);
        return false;
    }
    if (!accept_packet(std::move(pkt)))
    {
        return false;
    }
    return true;
}

esp_err_t upstream_tx_endpoint::eth_input_cb(esp_eth_handle_t eth,
                                             uint8_t* buffer, uint32_t length,
                                             void* priv, void* info)
{
    (void)eth;
    (void)info;
    auto* self = static_cast<upstream_tx_endpoint*>(priv);
    if (self == nullptr || buffer == nullptr)
    {
        free(buffer);
        return ESP_ERR_INVALID_ARG;
    }
    self->eth_rx_cb.fetch_add(1, std::memory_order_relaxed);
    if (self->try_hijack_udp(buffer, length))
    {
        return ESP_OK;
    }
    if (self->netif_ == nullptr)
    {
        free(buffer);
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_receive(self->netif_, buffer, length, nullptr);
}

void upstream_tx_endpoint::attach_eth_input(esp_eth_handle_t eth,
                                            esp_netif_t* netif)
{
    if (eth == nullptr || netif == nullptr)
    {
        return;
    }
    eth_ = eth;
    netif_ = netif;
    const esp_err_t err =
        esp_eth_update_input_path_info(eth, eth_input_cb, this);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "eth input hijack failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "eth L2 inject hijack installed");
}

void upstream_tx_endpoint::clear_locked()
{
    used = false;
    inject_port_.store(0, std::memory_order_release);
    drop_no_pkt_pool.store(0, std::memory_order_relaxed);
    drop_queue_full.store(0, std::memory_order_relaxed);
    eth_rx_cb.store(0, std::memory_order_relaxed);
    eth_inject_l2.store(0, std::memory_order_relaxed);
    eth_inject_len_drop.store(0, std::memory_order_relaxed);
    eth_inject_null_sink.store(0, std::memory_order_relaxed);
    pool_free_min.store(UINT16_MAX, std::memory_order_relaxed);
}

bool upstream_tx_endpoint::init(wifi_tx& tx_ref)
{
    if (!lock.init())
    {
        return false;
    }
    tx = &tx_ref;
    return true;
}

void upstream_tx_endpoint::set_null_sink(bool enabled)
{
    null_sink_.store(enabled, std::memory_order_relaxed);
    ESP_LOGI(TAG, "inject sink=%s", enabled ? "null" : "wifi");
}

bool upstream_tx_endpoint::null_sink() const
{
    return null_sink_.load(std::memory_order_relaxed);
}

void upstream_tx_endpoint::reset_pool_free_min()
{
    const size_t free_n = packet_allocator::tx().available();
    const uint16_t v =
        free_n > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(free_n);
    pool_free_min.store(v, std::memory_order_relaxed);
    if (tx != nullptr)
    {
        tx->reset_queue_hwm();
    }
}

bool upstream_tx_endpoint::set_endpoint(uint16_t port)
{
    if (port == 0 || !lock.ready())
    {
        return false;
    }

    bfc::semaphore::lock guard(lock);
    if (!guard)
    {
        return false;
    }

    const bool same_port =
        used && inject_port_.load(std::memory_order_relaxed) == port;
    if (!same_port)
    {
        inject_port_.store(port, std::memory_order_release);
        used = true;
    }
    drop_no_pkt_pool.store(0, std::memory_order_relaxed);
    drop_queue_full.store(0, std::memory_order_relaxed);
    eth_rx_cb.store(0, std::memory_order_relaxed);
    eth_inject_l2.store(0, std::memory_order_relaxed);
    eth_inject_len_drop.store(0, std::memory_order_relaxed);
    eth_inject_null_sink.store(0, std::memory_order_relaxed);
    reset_pool_free_min();
    ESP_LOGI(TAG, "sut UDP %u (L2 hijack → wifi_tx queue %u)", port,
             static_cast<unsigned>(WIFI_RADIO_TX_QUEUE));
    return true;
}

bool upstream_tx_endpoint::clear()
{
    if (!lock.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(lock);
    if (!guard)
    {
        return false;
    }
    if (used)
    {
        clear_locked();
    }
    return true;
}

bool upstream_tx_endpoint::get_status(upstream_tx_bind_s* out)
{
    if (out == nullptr || !lock.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(lock);
    if (!guard)
    {
        return false;
    }
    if (!used)
    {
        return false;
    }
    out->udp_port = inject_port_.load(std::memory_order_relaxed);
    out->socket_open = eth_ != nullptr && out->udp_port != 0;
    out->null_sink = null_sink_.load(std::memory_order_relaxed);
    out->drop_no_pkt_pool = drop_no_pkt_pool.load(std::memory_order_relaxed);
    out->drop_queue_full = drop_queue_full.load(std::memory_order_relaxed);
    out->pool_free_min = pool_free_min.load(std::memory_order_relaxed);
    out->eth_rx_cb = eth_rx_cb.load(std::memory_order_relaxed);
    out->eth_inject_l2 = eth_inject_l2.load(std::memory_order_relaxed);
    out->eth_inject_len_drop = eth_inject_len_drop.load(std::memory_order_relaxed);
    out->eth_inject_null_sink = eth_inject_null_sink.load(std::memory_order_relaxed);
    return true;
}
