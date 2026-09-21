#include "lc_tx_endpoint.h"

#include "config.h"
#include "packet.h"
#include "wifi.h"
#include "wifi_tx.h"

#include <stdlib.h>
#include <string.h>
#include <utility>

#include "esp_log.h"

static const char* TAG = "lc_tx_ep";

lc_tx_endpoint& lc_tx_endpoint::instance()
{
    static lc_tx_endpoint inst;
    return inst;
}

void lc_tx_endpoint::note_pool_free(size_t free_n)
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

void lc_tx_endpoint::handle_datagram(const uint8_t* data, uint16_t len)
{
    if (len < WIFI_RADIO_INJECT_MIN || len > WIFI_RADIO_INJECT_MAX)
    {
        return;
    }
    if (null_sink_.load(std::memory_order_relaxed))
    {
        return;
    }

    if (!take_inject_grant())
    {
        drop_inject_grant.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (staging_count_.load(std::memory_order_acquire) >=
        static_cast<uint8_t>(LC_TX_STAGING_DEPTH))
    {
        drop_no_pkt_pool.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    wifi::instance().note_udp_tx_pkt();
    const uint8_t head = staging_head_.load(std::memory_order_relaxed);
    staging_slot& slot = staging_[head];
    memcpy(slot.data, data, len);
    slot.len = len;
    staging_head_.store(
        static_cast<uint8_t>((head + 1) % LC_TX_STAGING_DEPTH),
        std::memory_order_relaxed);
    staging_count_.fetch_add(1, std::memory_order_release);
    if (tx != nullptr)
    {
        tx->report_inject_staging(
            staging_count_.load(std::memory_order_relaxed));
    }
}

bool lc_tx_endpoint::try_hijack_udp(uint8_t* buffer, uint32_t length)
{
    const uint16_t port = inject_port_.load(std::memory_order_acquire);
    if (port == 0 || buffer == nullptr || length < 42)
    {
        return false;
    }
    // Ethernet II + IPv4 + UDP (no VLAN).
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
    if (ip[9] != 17)  // UDP
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
    handle_datagram(udp + 8, static_cast<uint16_t>(udp_len - 8));
    return true;
}

esp_err_t lc_tx_endpoint::eth_input_cb(esp_eth_handle_t eth, uint8_t* buffer,
                                       uint32_t length, void* priv, void* info)
{
    (void)eth;
    (void)info;
    auto* self = static_cast<lc_tx_endpoint*>(priv);
    if (self == nullptr || buffer == nullptr)
    {
        free(buffer);
        return ESP_ERR_INVALID_ARG;
    }
    if (self->try_hijack_udp(buffer, length))
    {
        free(buffer);
        if (self->flush_task_handle_ != nullptr)
        {
            xTaskNotifyGive(self->flush_task_handle_);
        }
        return ESP_OK;
    }
    if (self->netif_ == nullptr)
    {
        free(buffer);
        return ESP_ERR_INVALID_STATE;
    }
    return esp_netif_receive(self->netif_, buffer, length, nullptr);
}

void lc_tx_endpoint::attach_eth_input(esp_eth_handle_t eth, esp_netif_t* netif)
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

int lc_tx_endpoint::flush_staging_to_wifi(int max_move)
{
    int moved = 0;
    const bool null_sink = null_sink_.load(std::memory_order_relaxed);
    while (moved < max_move &&
           staging_count_.load(std::memory_order_acquire) > 0)
    {
        if (!null_sink && tx != nullptr && !tx->may_feed_from_lc_tx())
        {
            break;
        }
        const uint8_t tail = staging_tail_.load(std::memory_order_relaxed);
        staging_slot& slot = staging_[tail];
        if (null_sink)
        {
            staging_tail_.store(
                static_cast<uint8_t>((tail + 1) % LC_TX_STAGING_DEPTH),
                std::memory_order_relaxed);
            staging_count_.fetch_sub(1, std::memory_order_release);
            ++moved;
            continue;
        }

        note_pool_free(packet_allocator::tx().available());
        packet p = packet_allocator::tx().allocate();
        if (!p.is_valid())
        {
            break;
        }
        if (slot.len == 0 || slot.len > p.capacity())
        {
            drop_no_pkt_pool.fetch_add(1, std::memory_order_relaxed);
            staging_tail_.store(
                static_cast<uint8_t>((tail + 1) % LC_TX_STAGING_DEPTH),
                std::memory_order_relaxed);
            staging_count_.fetch_sub(1, std::memory_order_release);
            continue;
        }
        p.set_packet_offset(0);
        memcpy(p.data(), slot.data, slot.len);
        p.set_packet_size(slot.len);
        if (tx == nullptr || !tx->enqueue(std::move(p)))
        {
            drop_queue_full.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        staging_tail_.store(
            static_cast<uint8_t>((tail + 1) % LC_TX_STAGING_DEPTH),
            std::memory_order_relaxed);
        staging_count_.fetch_sub(1, std::memory_order_release);
        ++moved;
    }
    if (tx != nullptr)
    {
        tx->report_inject_staging(
            staging_count_.load(std::memory_order_relaxed));
    }
    return moved;
}

void lc_tx_endpoint::flush_task(void* arg)
{
    auto* self = static_cast<lc_tx_endpoint*>(arg);
    ESP_LOGI(TAG, "flush L2-hijack staging=%u batch=%u gap_ticks=%u",
             static_cast<unsigned>(LC_TX_STAGING_DEPTH),
             static_cast<unsigned>(self->flush_batch()),
             static_cast<unsigned>(self->emac_gap_ticks()));
    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, 0);
        const uint8_t staged =
            self->staging_count_.load(std::memory_order_relaxed);
        const uint8_t pressure_margin = self->staging_pressure_margin();
        const uint8_t pressure_slots = static_cast<uint8_t>(
            LC_TX_STAGING_DEPTH > pressure_margin
                ? LC_TX_STAGING_DEPTH - pressure_margin
                : 1u);
        const bool wifi_busy =
            self->tx != nullptr && !self->tx->may_feed_from_lc_tx();
        if (self->tx != nullptr)
        {
            // Backpressure while feeding WiFi; cleared during the post-batch EMAC
            // gap so the host can refill staging while WiFi DMA is idle.
            self->tx->set_lc_tx_yield(
                wifi_busy ||
                staged >= static_cast<uint8_t>(LC_TX_STAGING_DEPTH / 2u));
        }
        const int moved =
            self->flush_staging_to_wifi(static_cast<int>(self->flush_batch()));
        if (moved == 0)
        {
            if (staged > 0 || wifi_busy)
            {
                // wifi_tx / pool / EMAC timeshare — yield, retry soon.
                taskYIELD();
            }
            else
            {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
            }
            continue;
        }
        // Wait until wifi_tx has mostly drained this batch, then pause so
        // EMAC can refill staging with WiFi DMA idle (upstream_tx timeshare).
        // If EMAC is still filling staging during the wait, skip the gap and
        // flush again — otherwise L2 hijack drops with staging full.
        const uint8_t staged_after =
            self->staging_count_.load(std::memory_order_relaxed);
        const bool staging_pressure = staged_after >= pressure_slots;
        const uint8_t gap_ticks = self->emac_gap_ticks();
        if (self->tx != nullptr && !self->null_sink_.load(std::memory_order_relaxed))
        {
            if (!staging_pressure)
            {
                const uint32_t infl_cap = self->tx->max_in_flight_cap();
                for (int i = 0;
                     i < 400 &&
                     (self->tx->queue_size() > 2 ||
                      self->tx->tx_in_flight_count() > infl_cap / 2u);
                     ++i)
                {
                    if (self->staging_count_.load(std::memory_order_relaxed) >=
                        pressure_slots)
                    {
                        break;
                    }
                    taskYIELD();
                }
                self->tx->set_lc_tx_yield(false);
                if (gap_ticks > 0)
                {
                    vTaskDelay(gap_ticks);
                }
            }
        }
        else if (!staging_pressure && gap_ticks > 0)
        {
            vTaskDelay(gap_ticks);
        }
    }
}

void lc_tx_endpoint::clear_locked()
{
    used = false;
    inject_port_.store(0, std::memory_order_release);
    drop_no_pkt_pool.store(0, std::memory_order_relaxed);
    drop_queue_full.store(0, std::memory_order_relaxed);
    pool_free_min.store(UINT16_MAX, std::memory_order_relaxed);
    staging_head_.store(0, std::memory_order_relaxed);
    staging_tail_.store(0, std::memory_order_relaxed);
    staging_count_.store(0, std::memory_order_relaxed);
}

bool lc_tx_endpoint::init(wifi_tx& tx_ref)
{
    if (!lock.init())
    {
        return false;
    }
    tx = &tx_ref;
    return true;
}

bool lc_tx_endpoint::start(BaseType_t core, UBaseType_t prio,
                           uint32_t stack_bytes)
{
    if (started)
    {
        return true;
    }
    if (xTaskCreatePinnedToCore(flush_task, "lc_tx_flush", stack_bytes, this,
                                prio, &flush_task_handle_, core) != pdPASS)
    {
        ESP_LOGE(TAG, "flush task create failed");
        return false;
    }
    started = true;
    ESP_LOGI(TAG, "L2 inject flush core=%d prio=%u staging=%u",
             static_cast<int>(core), static_cast<unsigned>(prio),
             static_cast<unsigned>(LC_TX_STAGING_DEPTH));
    return true;
}

void lc_tx_endpoint::set_null_sink(bool enabled)
{
    null_sink_.store(enabled, std::memory_order_relaxed);
    ESP_LOGI(TAG, "inject sink=%s", enabled ? "null" : "wifi");
}

bool lc_tx_endpoint::null_sink() const
{
    return null_sink_.load(std::memory_order_relaxed);
}

void lc_tx_endpoint::reset_pool_free_min()
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

bool lc_tx_endpoint::set_endpoint(uint16_t port)
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

    if (used && inject_port_.load(std::memory_order_relaxed) == port)
    {
        return true;
    }

    inject_port_.store(port, std::memory_order_release);
    used = true;
    drop_no_pkt_pool.store(0, std::memory_order_relaxed);
    drop_queue_full.store(0, std::memory_order_relaxed);
    staging_head_.store(0, std::memory_order_relaxed);
    staging_tail_.store(0, std::memory_order_relaxed);
    staging_count_.store(0, std::memory_order_relaxed);
    reset_pool_free_min();

    ESP_LOGI(TAG, "sut UDP %u (L2 eth hijack, staging %u)", port,
             static_cast<unsigned>(LC_TX_STAGING_DEPTH));
    return true;
}

bool lc_tx_endpoint::clear()
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
    reset_inject_grant();
    return true;
}

uint8_t lc_tx_endpoint::flush_batch() const
{
    return flush_batch_.load(std::memory_order_relaxed);
}

uint8_t lc_tx_endpoint::emac_gap_ticks() const
{
    return emac_gap_ticks_.load(std::memory_order_relaxed);
}

uint8_t lc_tx_endpoint::staging_pressure_margin() const
{
    return staging_pressure_margin_.load(std::memory_order_relaxed);
}

bool lc_tx_endpoint::set_flush_batch(uint8_t n)
{
    if (n < 1 || n > 64)
    {
        return false;
    }
    flush_batch_.store(n, std::memory_order_relaxed);
    return true;
}

bool lc_tx_endpoint::set_emac_gap_ticks(uint8_t n)
{
    if (n > 50)
    {
        return false;
    }
    emac_gap_ticks_.store(n, std::memory_order_relaxed);
    return true;
}

bool lc_tx_endpoint::set_staging_pressure_margin(uint8_t n)
{
    if (n < 1 || n >= LC_TX_STAGING_DEPTH)
    {
        return false;
    }
    staging_pressure_margin_.store(n, std::memory_order_relaxed);
    return true;
}

void lc_tx_endpoint::reset_inject_grant()
{
    inject_grant_enforce_.store(false, std::memory_order_relaxed);
    inject_grant_remaining_.store(0, std::memory_order_relaxed);
}

void lc_tx_endpoint::issue_inject_grant(uint16_t n)
{
    if (n == 0)
    {
        return;
    }
    inject_grant_enforce_.store(true, std::memory_order_relaxed);
    const uint16_t cur =
        inject_grant_remaining_.load(std::memory_order_relaxed);
    uint32_t sum = static_cast<uint32_t>(cur) + static_cast<uint32_t>(n);
    constexpr uint32_t k_grant_cap = 255u;
    if (sum > k_grant_cap)
    {
        sum = k_grant_cap;
    }
    inject_grant_remaining_.store(static_cast<uint16_t>(sum),
                                   std::memory_order_relaxed);
}

uint16_t lc_tx_endpoint::inject_grant_remaining() const
{
    return inject_grant_remaining_.load(std::memory_order_relaxed);
}

uint32_t lc_tx_endpoint::inject_grant_denied() const
{
    return drop_inject_grant.load(std::memory_order_relaxed);
}

bool lc_tx_endpoint::take_inject_grant()
{
    if (!inject_grant_enforce_.load(std::memory_order_relaxed))
    {
        return true;
    }
    uint16_t left = inject_grant_remaining_.load(std::memory_order_relaxed);
    if (left == 0)
    {
        return false;
    }
    inject_grant_remaining_.store(left - 1, std::memory_order_relaxed);
    return true;
}

bool lc_tx_endpoint::get_status(lc_tx_bind_s* out)
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
    return true;
}
