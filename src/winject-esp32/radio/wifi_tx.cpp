#include "wifi_tx.h"

#include "config.h"
#include "frame.h"
#include "lc_tx.h"
#include "udp_logger.h"
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

    const int8_t quarter_dbm = static_cast<int8_t>(tx_power_dbm * 4);
    err = esp_wifi_set_max_tx_power(quarter_dbm);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_max_tx_power %d dBm failed: %s", tx_power_dbm,
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

bool wifi_tx::init(lc_tx& tx)
{
    this->tx = &tx;
    return true;
}

void wifi_tx::stamp(packet& out, const pdu_slot_t slots[WIFI_PDU_SLOTS])
{
    const size_t payload = out.size();
    out.set_packet_offset(0);
    frameStampHeader(out.data(), slots, domain_.load(std::memory_order_acquire));
    out.set_packet_size(WIFI_HDR_LEN + payload);
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
        {
            bfc::semaphore::lock lock(radio.lock, pdMS_TO_TICKS(50));
            if (!lock)
            {
                vTaskDelay(1);
                continue;
            }
            if (have_seq)
            {
                note_submit(seq);
            }
        }
        // Do not hold radio.lock across 80211_tx: that call posts into the
        // Wi-Fi task (same core as TX-done / promiscuous). A lock inversion
        // there stalls completions, so the driver ring never drains.
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
                tx != nullptr ? static_cast<unsigned>(tx->queue_size()) : 0u);
            vTaskDelay(1);
            continue;
        }
        tx_retry_other.fetch_add(1, std::memory_order_relaxed);
        udp_logger::instance().log(
            log_level_e::warn,
            "tx_retry err=%s retries=%d in_flight=%u q=%u", esp_err_to_name(err),
            retries,
            static_cast<unsigned>(in_flight.load(std::memory_order_relaxed)),
            tx != nullptr ? static_cast<unsigned>(tx->queue_size()) : 0u);
        if (++fail_tries >= WIFI_RADIO_INJECT_RETRIES)
        {
            break;
        }
        vTaskDelay(1);
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
            tx != nullptr ? static_cast<unsigned>(tx->queue_size()) : 0u);
    }
    return ok;
}

void wifi_tx::run()
{
    uint32_t consecutive_ok = 0;
    for (;;)
    {
        if (tx == nullptr)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        bus_t bus = 0;
        packet out = tx->pop(&bus, portMAX_DELAY);
        if (!out.is_valid())
        {
            continue;
        }
        if (domain_.load(std::memory_order_acquire) == 0)
        {
            continue;
        }

        pdu_slot_t slots[WIFI_PDU_SLOTS] = {};
        slots[0] = {bus, static_cast<uint16_t>(out.size())};
        uint8_t n = 1;
        while (n < WIFI_PDU_SLOTS)
        {
            bus_t next = 0;
            uint16_t nsz = 0;
            if (!tx->peek(&next, &nsz) ||
                out.size() + nsz > WIFI_PAYLOAD_MAX)
            {
                break;
            }
            bus_t dbus = 0;
            packet donor = tx->pop(&dbus, 0);
            if (!donor.is_valid())
            {
                break;
            }
            memcpy(out.data() + out.size(), donor.data(), donor.size());
            out.set_packet_size(out.size() + donor.size());
            slots[n] = {dbus, static_cast<uint16_t>(donor.size())};
            n++;
        }

        stamp(out, slots);
        const bool ok = inject_retry(out.data(), out.size());
        if (ok)
        {
            radio.pulse_tx_led();
            consecutive_ok++;
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
            drop_tx_nomem.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void wifi_tx::task(void* arg)
{
    static_cast<wifi_tx*>(arg)->run();
}

bool wifi_tx::start(BaseType_t core, UBaseType_t prio, uint32_t stack_bytes)
{
    if (tx == nullptr)
    {
        ESP_LOGE(TAG, "wifi_tx start without lc_tx");
        return false;
    }
    if (xTaskCreatePinnedToCore(task, "wifi_tx", stack_bytes, this, prio,
                                nullptr, core) != pdPASS)
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
    status->tx_in_flight =
        static_cast<uint16_t>(in_flight.load(std::memory_order_relaxed));
    status->tx_queue =
        tx != nullptr ? static_cast<uint16_t>(tx->queue_size()) : 0;
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
    wifi::instance().tx.note_done(seq);
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
