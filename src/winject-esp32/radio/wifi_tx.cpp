#include "wifi_tx.h"

#include "config.h"
#include "wifi.h"

#include <atomic>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bfc-esp32/semaphore.hpp"

static const char* TAG = "wifi_tx";

extern "C" int ieee80211_raw_frame_sanity_check(int32_t, int32_t, uint32_t,
                                                uint32_t)
{
    return 0;
}

extern "C"
{
    int hal_mac_tx_set_cca(int enable);
    void esp_rom_phy_disable_cca(void) __attribute__((weak));
    void phy_disable_cca(void) __attribute__((weak));
    void phy_enable_cca(void) __attribute__((weak));
}

wifi_tx::wifi_tx(wifi& radio) : radio_(radio) {}

int8_t wifi_tx::power_dbm() const
{
    return tx_power_dbm_;
}

bool wifi_tx::apply_power()
{
    esp_err_t err = radio_.apply_country();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_country failed: %s", esp_err_to_name(err));
        return false;
    }

    const int8_t quarter_dbm = static_cast<int8_t>(tx_power_dbm_ * 4);
    err = esp_wifi_set_max_tx_power(quarter_dbm);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_max_tx_power %d dBm failed: %s", tx_power_dbm_,
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

bool wifi_tx::apply_cca()
{
    const int err = hal_mac_tx_set_cca(cca_enabled_ ? 1 : 0);
    if (err != 0)
    {
        ESP_LOGE(TAG, "hal_mac_tx_set_cca failed: %d", err);
        return false;
    }

    if (cca_enabled_)
    {
        if (phy_cca_off_ && phy_enable_cca)
        {
            phy_enable_cca();
            phy_cca_off_ = false;
        }
        return true;
    }

    if (esp_rom_phy_disable_cca)
    {
        esp_rom_phy_disable_cca();
        phy_cca_off_ = true;
    }
    else if (phy_disable_cca)
    {
        phy_disable_cca();
        phy_cca_off_ = true;
    }
    return true;
}

bool wifi_tx::init()
{
    return pool_.init() && queue_.init();
}

void wifi_tx::run()
{
    uint32_t consecutive_ok = 0;
    for (;;)
    {
        slot_s* slot = queue_.take(portMAX_DELAY);
        if (slot == nullptr)
        {
            continue;
        }

        bool ok = false;
        int fail_tries = 0;
        for (;;)
        {
            {
                bfc::semaphore::lock lock(radio_.lock(), pdMS_TO_TICKS(50));
                if (!lock)
                {
                    vTaskDelay(1);
                    continue;
                }
                uint16_t seq = 0;
                const bool have_seq = seq_of(slot->data, slot->len, &seq);
                if (have_seq)
                {
                    note_submit(seq);
                }
                const esp_err_t err =
                    esp_wifi_80211_tx(WIFI_IF_STA, slot->data,
                                      static_cast<int>(slot->len), false);
                if (err == ESP_OK)
                {
                    ok = true;
                    break;
                }
                if (have_seq)
                {
                    cancel_submit(seq);
                }
                // MAC TX ring full: keep the slot and wait for a descriptor.
                if (err != ESP_ERR_NO_MEM &&
                    ++fail_tries >= WIFI_RADIO_INJECT_RETRIES)
                {
                    break;
                }
            }
            // Do not hold the radio lock across a delay: that blocks
            // set_modulation/channel and keeps CPU0 out of idle.
            vTaskDelay(1);
        }

        if (ok)
        {
            radio_.pulse_tx_led();
            consecutive_ok++;
            // Continuous MCS7 inject never blocks on an empty queue, so the
            // CPU0 idle task never runs and the task WDT panics. Yield, and
            // tick-delay occasionally so idle can reset the watchdog.
            if ((consecutive_ok & 31u) == 0)
            {
                vTaskDelay(1);
            }
            else
            {
                taskYIELD();
            }
        }
        else
        {
            consecutive_ok = 0;
            udp_tx_failed_.fetch_add(1, std::memory_order_relaxed);
        }

        pool_.release(slot);
    }
}

void wifi_tx::task(void* arg)
{
    static_cast<wifi_tx*>(arg)->run();
}

bool wifi_tx::start_task()
{
    if (xTaskCreatePinnedToCore(task, "wifi_tx", 6144, this,
                                WIFI_RADIO_TASK_PRIO, nullptr,
                                WIFI_RADIO_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "wifi_tx task failed");
        return false;
    }
    return true;
}

void wifi_tx::fill_status(wifi_status_s* status)
{
    if (status == nullptr)
    {
        return;
    }
    status->cca_enabled = cca_enabled_;
    status->tx_power_dbm = tx_power_dbm_;
    status->udp_tx_pkt = udp_tx_pkt_.load(std::memory_order_relaxed);
    status->udp_tx_failed = udp_tx_failed_.load(std::memory_order_relaxed);
    status->tx_queue = static_cast<uint16_t>(queue_.size());

    const uint32_t n = latency_count_.load(std::memory_order_relaxed);
    if (n == 0)
    {
        status->tx_latency_valid = false;
        status->tx_latency_us = 0;
        return;
    }
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        sum += latency_us_[i].load(std::memory_order_relaxed);
    }
    status->tx_latency_valid = true;
    status->tx_latency_us = static_cast<uint32_t>(sum / n);
}

void wifi_tx::note_udp_tx_pkt()
{
    udp_tx_pkt_.fetch_add(1, std::memory_order_relaxed);
}

wifi_tx::slot_s* wifi_tx::take(TickType_t wait)
{
    if (!radio_.ready() || !pool_.ready())
    {
        return nullptr;
    }
    return pool_.take(wait);
}

void wifi_tx::release(slot_s* slot)
{
    if (slot == nullptr)
    {
        return;
    }
    pool_.release(slot);
}

bool wifi_tx::post(slot_s* slot, TickType_t wait)
{
    if (!radio_.ready() || slot == nullptr || !queue_.ready() ||
        slot->len < WIFI_RADIO_INJECT_MIN || slot->len > WIFI_RADIO_INJECT_MAX)
    {
        release(slot);
        return false;
    }
    if (!queue_.post(slot, wait))
    {
        release(slot);
        return false;
    }
    return true;
}

bool wifi_tx::inject(const uint8_t* frame, size_t len)
{
    if (!radio_.ready() || frame == nullptr || len < WIFI_RADIO_INJECT_MIN ||
        len > WIFI_RADIO_INJECT_MAX)
    {
        return false;
    }

    // Non-blocking: a full TX pool must not hang Ethernet/console tasks.
    slot_s* slot = take(0);
    if (slot == nullptr)
    {
        return false;
    }
    slot->len = static_cast<uint16_t>(len);
    memcpy(slot->data, frame, len);
    return post(slot, 0);
}

bool wifi_tx::set_cca_enabled(bool enabled)
{
    if (!radio_.ready())
    {
        return false;
    }
    if (cca_enabled_ == enabled)
    {
        return true;
    }
    bfc::semaphore::lock lock(radio_.lock(), pdMS_TO_TICKS(1000));
    if (!lock)
    {
        return false;
    }
    const bool previous = cca_enabled_;
    cca_enabled_ = enabled;
    const bool ok = apply_cca();
    if (!ok)
    {
        cca_enabled_ = previous;
    }
    return ok;
}

bool wifi_tx::set_tx_power(int8_t dbm)
{
    if (!radio_.ready())
    {
        return false;
    }
    if (dbm < WIFI_TX_POWER_DBM_MIN || dbm > WIFI_TX_POWER_DBM_MAX)
    {
        return false;
    }
    if (tx_power_dbm_ == dbm)
    {
        return true;
    }
    bfc::semaphore::lock lock(radio_.lock(), pdMS_TO_TICKS(1000));
    if (!lock)
    {
        return false;
    }
    const int8_t previous = tx_power_dbm_;
    tx_power_dbm_ = dbm;
    const bool ok = apply_power();
    if (!ok)
    {
        tx_power_dbm_ = previous;
        apply_power();
    }
    return ok;
}

bool wifi_tx::seq_of(const uint8_t* frame, size_t len, uint16_t* seq)
{
    if (frame == nullptr || seq == nullptr || len < WIFI_RADIO_INJECT_MIN)
    {
        return false;
    }
    const uint16_t ctl = static_cast<uint16_t>(frame[22] | (frame[23] << 8));
    *seq = static_cast<uint16_t>((ctl >> 4) & 0x0FFF);
    return true;
}

void wifi_tx::record_latency(uint32_t us)
{
    const uint32_t slot =
        latency_next_.fetch_add(1, std::memory_order_relaxed) % kLatencySamples;
    latency_us_[slot].store(us, std::memory_order_relaxed);
    uint32_t n = latency_count_.load(std::memory_order_relaxed);
    while (n < kLatencySamples &&
           !latency_count_.compare_exchange_weak(n, n + 1,
                                                 std::memory_order_relaxed))
    {
    }
}

void wifi_tx::note_submit(uint16_t seq)
{
    const size_t idx = static_cast<size_t>(seq) & (kPendingCap - 1);
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    uint8_t hol = 0;
    if (pending_used_[idx].load(std::memory_order_acquire) != 0)
    {
        // Same seq refresh, or a missed callback occupying this slot.
        // Do not change in_flight: replace the outstanding entry.
        hol = 0;
    }
    else if (in_flight_.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        hol = 1;
    }
    pending_t0_us_[idx].store(now, std::memory_order_relaxed);
    pending_seq_[idx].store(seq, std::memory_order_relaxed);
    pending_hol_[idx].store(hol, std::memory_order_relaxed);
    pending_used_[idx].store(1, std::memory_order_release);
}

void wifi_tx::cancel_submit(uint16_t seq)
{
    const size_t idx = static_cast<size_t>(seq) & (kPendingCap - 1);
    if (pending_used_[idx].load(std::memory_order_acquire) == 0)
    {
        return;
    }
    if (pending_seq_[idx].load(std::memory_order_relaxed) != seq)
    {
        return;
    }
    pending_used_[idx].store(0, std::memory_order_relaxed);
    uint32_t n = in_flight_.load(std::memory_order_relaxed);
    while (n > 0 &&
           !in_flight_.compare_exchange_weak(n, n - 1,
                                             std::memory_order_relaxed))
    {
    }
}

void wifi_tx::note_done(uint16_t seq)
{
    const size_t idx = static_cast<size_t>(seq) & (kPendingCap - 1);
    if (pending_used_[idx].load(std::memory_order_acquire) == 0)
    {
        return;
    }
    if (pending_seq_[idx].load(std::memory_order_relaxed) != seq)
    {
        return;
    }
    pending_used_[idx].store(0, std::memory_order_relaxed);
    const bool hol = pending_hol_[idx].load(std::memory_order_relaxed) != 0;
    const uint64_t t0 = pending_t0_us_[idx].load(std::memory_order_relaxed);
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    uint32_t n = in_flight_.load(std::memory_order_relaxed);
    while (n > 0 &&
           !in_flight_.compare_exchange_weak(n, n - 1,
                                             std::memory_order_relaxed))
    {
    }
    const uint64_t prev = last_done_us_.exchange(now, std::memory_order_relaxed);

    uint64_t dt = 0;
    if (hol)
    {
        // Alone in the driver at submit: accept → done ≈ airtime + CCA.
        if (now >= t0)
        {
            dt = now - t0;
        }
    }
    else if (prev != 0 && now >= prev)
    {
        // Saturated path: time between completions ≈ one MPDU service.
        dt = now - prev;
    }
    if (dt == 0 || dt > 30000000ull)
    {
        return;
    }
    record_latency(static_cast<uint32_t>(dt));
}

void wifi_tx::on_tx_done(const esp_80211_tx_info_t* info)
{
    if (info == nullptr || info->data == nullptr)
    {
        return;
    }
    uint16_t seq = 0;
    if (!seq_of(info->data, WIFI_RADIO_INJECT_MIN, &seq))
    {
        return;
    }
    wifi::instance().tx_.note_done(seq);
}

bool wifi_tx::apply_tx_done_cb()
{
    const esp_err_t err = esp_wifi_register_80211_tx_cb(on_tx_done);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "register_80211_tx_cb failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
