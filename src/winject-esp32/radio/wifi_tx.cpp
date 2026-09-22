#include "config.h"
#include "packet.h"
#include "udp_logger.h"
#include "wifi.h"
#include "wifi_tx.h"

#include <atomic>
#include <string.h>
#include <utility>

#include "esp_log.h"
#include "esp_rom_sys.h"
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

wifi_tx::wifi_tx(wifi& radio) : radio(radio) {}

int8_t wifi_tx::power_dbm() const
{
    return tx_power_dbm;
}

bool wifi_tx::set_domain(uint16_t domain)
{
    domain_.store(domain, std::memory_order_release);
    return true;
}

uint16_t wifi_tx::domain() const
{
    return domain_.load(std::memory_order_acquire);
}

bool wifi_tx::apply_power()
{
    esp_err_t err = radio.apply_country();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_country failed: %s", esp_err_to_name(err));
        return false;
    }

    int8_t dbm = tx_power_dbm;
    const wifi_phy_rate_t rate = radio.phy_rate();
    if (rate == WIFI_PHY_RATE_48M || rate == WIFI_PHY_RATE_54M)
    {
        if (dbm > WIFI_TX_POWER_64QAM_LEGACY_MAX_DBM)
        {
            dbm = WIFI_TX_POWER_64QAM_LEGACY_MAX_DBM;
        }
    }

    const int8_t quarter_dbm = static_cast<int8_t>(dbm * 4);
    err = esp_wifi_set_max_tx_power(quarter_dbm);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_max_tx_power %d dBm failed: %s", dbm,
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

bool wifi_tx::apply_cca()
{
    const int err = hal_mac_tx_set_cca(cca_enabled ? 1 : 0);
    if (err != 0)
    {
        ESP_LOGE(TAG, "hal_mac_tx_set_cca failed: %d", err);
        return false;
    }

    if (cca_enabled)
    {
        if (phy_cca_off && phy_enable_cca)
        {
            phy_enable_cca();
            phy_cca_off = false;
        }
        return true;
    }

    if (esp_rom_phy_disable_cca)
    {
        esp_rom_phy_disable_cca();
        phy_cca_off = true;
    }
    else if (phy_disable_cca)
    {
        phy_disable_cca();
        phy_cca_off = true;
    }
    return true;
}

bool wifi_tx::init()
{
    return q.init();
}

void wifi_tx::notify_inject_work()
{
    if (task_handle_ != nullptr)
    {
        xTaskNotifyGive(task_handle_);
    }
}

uint32_t wifi_tx::tx_in_flight_count() const
{
    return in_flight.load(std::memory_order_relaxed);
}

uint32_t wifi_tx::max_in_flight_cap() const
{
    return max_in_flight_cap_.load(std::memory_order_relaxed);
}

bool wifi_tx::set_max_in_flight(uint32_t n)
{
    if (n < 1 || n > 32)
    {
        return false;
    }
    max_in_flight_cap_.store(n, std::memory_order_relaxed);
    return true;
}

uint8_t wifi_tx::tx_burst_size() const
{
    return burst_size_.load(std::memory_order_relaxed);
}

uint32_t wifi_tx::tx_burst_gap_us() const
{
    return burst_gap_us_.load(std::memory_order_relaxed);
}

bool wifi_tx::set_tx_burst(uint8_t size, uint32_t gap_us)
{
    if (size < 1 || size > k_queue_cap || gap_us > 1000000u)
    {
        return false;
    }
    burst_size_.store(size, std::memory_order_relaxed);
    burst_gap_us_.store(gap_us, std::memory_order_relaxed);
    return true;
}

static void delay_us(uint32_t us)
{
    if (us == 0)
    {
        return;
    }
    if (us >= 1000u)
    {
        vTaskDelay(pdMS_TO_TICKS((us + 999u) / 1000u));
        return;
    }
    esp_rom_delay_us(us);
}

bool wifi_tx::queue_full() const
{
    if (!q.ready())
    {
        return true;
    }
    return queue_size() >= k_queue_cap;
}

bool wifi_tx::on_inject_task() const
{
    return task_handle_ != nullptr &&
           xTaskGetCurrentTaskHandle() == task_handle_;
}

bool wifi_tx::enqueue(packet&& pkt)
{
    if (!q.ready() || !pkt.is_valid())
    {
        tx_enqueue_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!q.try_push(std::optional<packet>(std::move(pkt))))
    {
        tx_enqueue_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    tx_enqueue_ok.fetch_add(1, std::memory_order_relaxed);
    note_queue_sample(queue_size());
    if (!on_inject_task())
    {
        notify_inject_work();
    }
    return true;
}

void wifi_tx::note_queue_sample(uint8_t size)
{
    uint8_t prev = tx_q_hwm.load(std::memory_order_relaxed);
    while (size > prev &&
           !tx_q_hwm.compare_exchange_weak(prev, size,
                                           std::memory_order_relaxed))
    {
    }
}

void wifi_tx::reset_queue_hwm()
{
    tx_q_hwm.store(queue_size(), std::memory_order_relaxed);
}

packet wifi_tx::pop(TickType_t wait)
{
    packet out;
    std::optional<packet> slot;
    if (!q.pop(&slot, wait) || !slot.has_value())
    {
        return out;
    }
    out = std::move(*slot);
    return out;
}

uint8_t wifi_tx::queue_size() const
{
    return q.size();
}

bool wifi_tx::inject_retry(const uint8_t* frame, size_t len)
{
    bool ok = false;
    int fail_tries = 0;
    int retries = 0;
    esp_err_t last_err = ESP_OK;
    const uint64_t t0 = static_cast<uint64_t>(esp_timer_get_time());
    for (;;)
    {
        uint16_t seq = 0;
        const bool have_seq = seq_of(frame, len, &seq);
        if (have_seq)
        {
            note_submit(seq);
        }
        // Do not hold radio.lock across 80211_tx: that call posts into the
        // Wi-Fi task (same core as TX-done / promiscuous). A lock inversion
        // there stalls completions, so the driver ring never drains.
        // Bound outstanding TX so we do not storm NO_MEM and starve EMAC DMA.
        for (int spin = 0;
             in_flight.load(std::memory_order_relaxed) >=
                 max_in_flight_cap_.load(std::memory_order_relaxed);
             ++spin)
        {
            if (spin < WIFI_RADIO_INJECT_NOMEM_YIELD)
            {
                taskYIELD();
            }
            else
            {
                vTaskDelay(1);
                spin = 0;
            }
        }
        const esp_err_t err = esp_wifi_80211_tx(
            WIFI_IF_STA, frame, static_cast<int>(len), false);
        if (err == ESP_OK)
        {
            ok = true;
            break;
        }
        last_err = err;
        if (have_seq)
        {
            cancel_submit(seq);
        }
        retries++;
        tx_retry_count.fetch_add(1, std::memory_order_relaxed);
        if (err == ESP_ERR_NO_MEM)
        {
            tx_retry_nomem.fetch_add(1, std::memory_order_relaxed);
            udp_logger::instance().log(
                log_level_e::warn,
                "tx_retry err=NO_MEM retries=%d in_flight=%u q=%u", retries,
                static_cast<unsigned>(in_flight.load(std::memory_order_relaxed)),
                static_cast<unsigned>(queue_size()));
            // Prefer yield over 1 ms sleep: TX-done on this core often frees
            // the driver ring within tens of µs. A full tick per NO_MEM was
            // capping inject well below OFDM_24M goodput.
            if (retries <= WIFI_RADIO_INJECT_NOMEM_YIELD)
            {
                taskYIELD();
            }
            else
            {
                vTaskDelay(1);
            }
            continue;
        }
        tx_retry_other.fetch_add(1, std::memory_order_relaxed);
        udp_logger::instance().log(
            log_level_e::warn,
            "tx_retry err=%s retries=%d in_flight=%u q=%u", esp_err_to_name(err),
            retries,
            static_cast<unsigned>(in_flight.load(std::memory_order_relaxed)),
            static_cast<unsigned>(queue_size()));
        if (++fail_tries >= WIFI_RADIO_INJECT_RETRIES)
        {
            break;
        }
        if (fail_tries <= 2)
        {
            taskYIELD();
        }
        else
        {
            vTaskDelay(1);
        }
    }

    const uint64_t t1 = static_cast<uint64_t>(esp_timer_get_time());
    const uint64_t dt = t1 > t0 ? t1 - t0 : 0;
    if (dt > 0 && dt <= 30000000ull)
    {
        const uint32_t slot = inject_wait_next.fetch_add(
                                  1, std::memory_order_relaxed) %
                              kInjectWaitSamples;
        inject_wait_us[slot].store(static_cast<uint32_t>(dt),
                                    std::memory_order_relaxed);
        uint32_t n = inject_wait_count.load(std::memory_order_relaxed);
        while (n < kInjectWaitSamples &&
               !inject_wait_count.compare_exchange_weak(
                   n, n + 1, std::memory_order_relaxed))
        {
        }
    }

    if (ok)
    {
        inject_ok.fetch_add(1, std::memory_order_relaxed);
        if (retries > 0)
        {
            udp_logger::instance().log(
                log_level_e::info,
                "tx_ok after_retries=%d wait_us=%u in_flight=%u", retries,
                static_cast<unsigned>(dt),
                static_cast<unsigned>(
                    in_flight.load(std::memory_order_relaxed)));
        }
        else
        {
            udp_logger::instance().log(
                log_level_e::debug, "tx_ok wait_us=%u in_flight=%u",
                static_cast<unsigned>(dt),
                static_cast<unsigned>(
                    in_flight.load(std::memory_order_relaxed)));
        }
    }
    else
    {
        inject_fail.fetch_add(1, std::memory_order_relaxed);
        udp_logger::instance().log(
            log_level_e::error,
            "tx_fail err=%s retries=%d wait_us=%u in_flight=%u q=%u",
            esp_err_to_name(last_err), retries, static_cast<unsigned>(dt),
            static_cast<unsigned>(in_flight.load(std::memory_order_relaxed)),
            static_cast<unsigned>(queue_size()));
    }
    return ok;
}

void wifi_tx::set_dry_run(bool enabled)
{
    dry_run_.store(enabled, std::memory_order_relaxed);
}

bool wifi_tx::dry_run() const
{
    return dry_run_.load(std::memory_order_relaxed);
}

void wifi_tx::run()
{
    for (;;)
    {
        packet out = pop(portMAX_DELAY);
        if (!out.is_valid() || out.data() == nullptr)
        {
            continue;
        }
        if (out.size() < WIFI_RADIO_INJECT_MIN ||
            out.size() > WIFI_RADIO_INJECT_MAX)
        {
            continue;
        }

        if (dry_run_.load(std::memory_order_relaxed))
        {
            inject_ok.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            const bool ok = inject_retry(out.data(), out.size());
            if (ok)
            {
                radio.pulse_tx_led();
            }
            else
            {
                drop_tx_nomem.fetch_add(1, std::memory_order_relaxed);
            }
        }

        burst_sent_++;
        const uint8_t burst_cap =
            burst_size_.load(std::memory_order_relaxed);
        const bool queue_drained = queue_size() == 0;
        if (burst_sent_ >= burst_cap || queue_drained)
        {
            delay_us(burst_gap_us_.load(std::memory_order_relaxed));
            burst_sent_ = 0;
        }
        else
        {
            taskYIELD();
        }
    }
}

void wifi_tx::task(void* arg)
{
    static_cast<wifi_tx*>(arg)->run();
}

bool wifi_tx::start(BaseType_t core, UBaseType_t prio, uint32_t stack_bytes)
{
    if (!q.ready())
    {
        ESP_LOGE(TAG, "wifi_tx start without queue");
        return false;
    }
    if (xTaskCreatePinnedToCore(task, "wifi_tx", stack_bytes, this, prio,
                                &task_handle_, core) != pdPASS)
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
    status->cca_enabled = cca_enabled;
    status->tx_power_dbm = tx_power_dbm;
    status->udp_tx_pkt = udp_tx_pkt.load(std::memory_order_relaxed);
    status->drop_tx_nomem = drop_tx_nomem.load(std::memory_order_relaxed);
    status->tx_retry_count = tx_retry_count.load(std::memory_order_relaxed);
    status->tx_retry_nomem = tx_retry_nomem.load(std::memory_order_relaxed);
    status->tx_retry_other = tx_retry_other.load(std::memory_order_relaxed);
    status->inject_ok = inject_ok.load(std::memory_order_relaxed);
    status->inject_fail = inject_fail.load(std::memory_order_relaxed);
    status->tx_enqueue_ok = tx_enqueue_ok.load(std::memory_order_relaxed);
    status->tx_enqueue_fail = tx_enqueue_fail.load(std::memory_order_relaxed);
    status->tx_in_flight =
        static_cast<uint16_t>(in_flight.load(std::memory_order_relaxed));
    status->tx_queue = static_cast<uint16_t>(queue_size());
    status->tx_queue_hwm =
        static_cast<uint16_t>(tx_q_hwm.load(std::memory_order_relaxed));
    status->tx_pool_free =
        static_cast<uint16_t>(packet_allocator::tx().available());
    status->inject_dry_run = dry_run_.load(std::memory_order_relaxed);
    status->domain = domain_.load(std::memory_order_relaxed);

    const uint32_t n = latency_count.load(std::memory_order_relaxed);
    if (n == 0)
    {
        status->tx_latency_valid = false;
        status->tx_latency_us = 0;
    }
    else
    {
        uint64_t sum = 0;
        for (uint32_t i = 0; i < n; i++)
        {
            sum += latency_us[i].load(std::memory_order_relaxed);
        }
        status->tx_latency_valid = true;
        status->tx_latency_us = static_cast<uint32_t>(sum / n);
    }

    const uint32_t wn = inject_wait_count.load(std::memory_order_relaxed);
    if (wn == 0)
    {
        status->inject_wait_valid = false;
        status->inject_wait_us = 0;
        return;
    }
    uint64_t wsum = 0;
    for (uint32_t i = 0; i < wn; i++)
    {
        wsum += inject_wait_us[i].load(std::memory_order_relaxed);
    }
    status->inject_wait_valid = true;
    status->inject_wait_us = static_cast<uint32_t>(wsum / wn);
}

void wifi_tx::note_udp_tx_pkt()
{
    udp_tx_pkt.fetch_add(1, std::memory_order_relaxed);
}

void wifi_tx::reset_channel_stats()
{
    // Metrics only — never zero in_flight or pending slots while 802.11 TX may
    // still complete (e.g. bw_test reset between A→B and B→A legs).
    udp_tx_pkt.store(0, std::memory_order_relaxed);
    drop_tx_nomem.store(0, std::memory_order_relaxed);
    tx_retry_count.store(0, std::memory_order_relaxed);
    tx_retry_nomem.store(0, std::memory_order_relaxed);
    tx_retry_other.store(0, std::memory_order_relaxed);
    inject_ok.store(0, std::memory_order_relaxed);
    inject_fail.store(0, std::memory_order_relaxed);
    tx_enqueue_ok.store(0, std::memory_order_relaxed);
    tx_enqueue_fail.store(0, std::memory_order_relaxed);
    tx_q_hwm.store(0, std::memory_order_relaxed);
    latency_count.store(0, std::memory_order_relaxed);
    inject_wait_count.store(0, std::memory_order_relaxed);
    inject_wait_next.store(0, std::memory_order_relaxed);
}

bool wifi_tx::set_cca_enabled(bool enabled)
{
    if (!radio.ready())
    {
        return false;
    }
    if (cca_enabled == enabled)
    {
        return true;
    }
    bfc::semaphore::lock lock(radio.lock, pdMS_TO_TICKS(1000));
    if (!lock)
    {
        return false;
    }
    const bool previous = cca_enabled;
    cca_enabled = enabled;
    const bool ok = apply_cca();
    if (!ok)
    {
        cca_enabled = previous;
    }
    return ok;
}

bool wifi_tx::set_tx_power(int8_t dbm)
{
    if (!radio.ready())
    {
        return false;
    }
    if (dbm < WIFI_TX_POWER_DBM_MIN || dbm > WIFI_TX_POWER_DBM_MAX)
    {
        return false;
    }
    if (tx_power_dbm == dbm)
    {
        return true;
    }
    bfc::semaphore::lock lock(radio.lock, pdMS_TO_TICKS(1000));
    if (!lock)
    {
        return false;
    }
    const int8_t previous = tx_power_dbm;
    tx_power_dbm = dbm;
    const bool ok = apply_power();
    if (!ok)
    {
        tx_power_dbm = previous;
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
        latency_next.fetch_add(1, std::memory_order_relaxed) % kLatencySamples;
    latency_us[slot].store(us, std::memory_order_relaxed);
    uint32_t n = latency_count.load(std::memory_order_relaxed);
    while (n < kLatencySamples &&
           !latency_count.compare_exchange_weak(n, n + 1,
                                                 std::memory_order_relaxed))
    {
    }
}

void wifi_tx::note_submit(uint16_t seq)
{
    const size_t idx = static_cast<size_t>(seq) & (kPendingCap - 1);
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    uint8_t hol = 0;
    if (pending_used[idx].load(std::memory_order_acquire) != 0)
    {
        hol = 0;
    }
    else if (in_flight.fetch_add(1, std::memory_order_relaxed) == 0)
    {
        hol = 1;
    }
    pending_t0_us[idx].store(now, std::memory_order_relaxed);
    pending_seq[idx].store(seq, std::memory_order_relaxed);
    pending_hol[idx].store(hol, std::memory_order_relaxed);
    pending_used[idx].store(1, std::memory_order_release);
}

void wifi_tx::cancel_submit(uint16_t seq)
{
    const size_t idx = static_cast<size_t>(seq) & (kPendingCap - 1);
    if (pending_used[idx].load(std::memory_order_acquire) == 0)
    {
        return;
    }
    if (pending_seq[idx].load(std::memory_order_relaxed) != seq)
    {
        return;
    }
    pending_used[idx].store(0, std::memory_order_relaxed);
    uint32_t n = in_flight.load(std::memory_order_relaxed);
    while (n > 0 &&
           !in_flight.compare_exchange_weak(n, n - 1,
                                             std::memory_order_relaxed))
    {
    }
}

void wifi_tx::note_done(uint16_t seq)
{
    const size_t idx = static_cast<size_t>(seq) & (kPendingCap - 1);
    if (pending_used[idx].load(std::memory_order_acquire) == 0)
    {
        return;
    }
    if (pending_seq[idx].load(std::memory_order_relaxed) != seq)
    {
        return;
    }
    pending_used[idx].store(0, std::memory_order_relaxed);
    const bool hol = pending_hol[idx].load(std::memory_order_relaxed) != 0;
    const uint64_t t0 = pending_t0_us[idx].load(std::memory_order_relaxed);
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    uint32_t n = in_flight.load(std::memory_order_relaxed);
    while (n > 0 &&
           !in_flight.compare_exchange_weak(n, n - 1,
                                             std::memory_order_relaxed))
    {
    }
    const uint64_t prev = last_done_us.exchange(now, std::memory_order_relaxed);

    uint64_t dt = 0;
    if (hol)
    {
        if (now >= t0)
        {
            dt = now - t0;
        }
    }
    else if (prev != 0 && now >= prev)
    {
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
    wifi::instance().tx().note_done(seq);
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
