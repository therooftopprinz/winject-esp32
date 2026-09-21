#include "wifi_bench.h"

#include "config.h"
#include "frame.h"
#include "packet.h"
#include "pdu_types.h"
#include "wifi.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "wifi_bench";

wifi_bench& wifi_bench::instance()
{
    static wifi_bench inst;
    return inst;
}

bool wifi_bench::start_tx(uint16_t size, uint32_t count, uint32_t kbps)
{
    if (running_)
    {
        ESP_LOGW(TAG, "already running");
        return false;
    }
    if (!wifi::instance().ready())
    {
        ESP_LOGE(TAG, "wifi not ready");
        return false;
    }
    if (size < WIFI_RADIO_INJECT_MIN || size > WIFI_RADIO_INJECT_MAX ||
        count == 0)
    {
        return false;
    }
    if (wifi::instance().domain() == 0)
    {
        ESP_LOGE(TAG, "domain unset");
        return false;
    }

    size_ = size;
    target_ = count;
    kbps_ = kbps;
    enq_ok_ = 0;
    enq_fail_ = 0;
    elapsed_us_ = 0;
    running_ = true;

    // Same core as lc_tx drain so wifi_tx (core 0) is the only WiFi producer
    // path under test — no Ethernet UDP involved.
    if (xTaskCreatePinnedToCore(task, "wifi_bench", 4096, this,
                                LC_TX_DRAIN_TASK_PRIO, nullptr,
                                APP_TASK_CORE) != pdPASS)
    {
        running_ = false;
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    return true;
}

void wifi_bench::stop()
{
    running_ = false;
}

bool wifi_bench::set_defaults(uint16_t size, uint32_t count, uint32_t kbps)
{
    if (size < WIFI_RADIO_INJECT_MIN || size > WIFI_RADIO_INJECT_MAX ||
        count == 0)
    {
        return false;
    }
    default_size_ = size;
    default_count_ = count;
    default_kbps_ = kbps;
    return true;
}

void wifi_bench::get_defaults(uint16_t* size, uint32_t* count,
                              uint32_t* kbps) const
{
    if (size != nullptr)
    {
        *size = default_size_;
    }
    if (count != nullptr)
    {
        *count = default_count_;
    }
    if (kbps != nullptr)
    {
        *kbps = default_kbps_;
    }
}

void wifi_bench::fill_status(wifi_bench_status_s* out) const
{
    if (out == nullptr)
    {
        return;
    }
    out->running = running_;
    out->size = size_;
    out->kbps = kbps_;
    out->target = target_;
    out->enq_ok = enq_ok_;
    out->enq_fail = enq_fail_;
    out->elapsed_us = elapsed_us_;
}

void wifi_bench::task(void* arg)
{
    static_cast<wifi_bench*>(arg)->run();
    vTaskDelete(nullptr);
}

bool wifi_bench::try_enqueue_one(wifi_tx& tx, uint16_t domain, uint16_t size)
{
    packet p = packet_allocator::tx().allocate();
    if (!p.is_valid() || p.capacity() < size)
    {
        return false;
    }
    p.set_packet_offset(0);
    uint8_t* buf = p.data();
    memset(buf, 0xA5, size);
    pdu_slot_t slots[WIFI_PDU_SLOTS] = {};
    slots[0].bus = 0xB2;
    slots[0].size = static_cast<uint16_t>(size - WIFI_HDR_LEN);
    frameStampHeader(buf, slots, domain);
    p.set_packet_size(size);
    return tx.enqueue(std::move(p));
}

void wifi_bench::wait_until(int64_t target_t)
{
    for (;;)
    {
        const int64_t now = esp_timer_get_time();
        if (now >= target_t)
        {
            break;
        }
        const int64_t left = target_t - now;
        if (left > 2000)
        {
            vTaskDelay(1);
        }
        else
        {
            taskYIELD();
        }
    }
}

void wifi_bench::run()
{
    const uint16_t domain = wifi::instance().domain();
    const uint16_t size = size_;
    const uint32_t target = target_;
    const uint32_t kbps = kbps_;
    const bool flood = kbps == 0;
    const uint64_t interval_us =
        flood ? 0
              : (static_cast<uint64_t>(size) * 8ull * 1000ull) / kbps;

    wifi_tx& tx = wifi::instance().tx();
    const int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG,
             "start size=%u count=%u kbps=%u%s interval_us=%llu domain=%u",
             size, (unsigned)target, (unsigned)kbps,
             flood ? " (flood)" : "", (unsigned long long)interval_us, domain);

    if (flood)
    {
        uint32_t sent = 0;
        while (sent < target && running_)
        {
            if (try_enqueue_one(tx, domain, size))
            {
                enq_ok_++;
                sent++;
            }
            else
            {
                taskYIELD();
            }
        }
    }
    else
    {
        for (uint32_t i = 0; i < target && running_; i++)
        {
            const int64_t target_t =
                t0 + static_cast<int64_t>((i + 1) * interval_us);
            bool enqueued = false;
            while (running_ && esp_timer_get_time() < target_t)
            {
                if (try_enqueue_one(tx, domain, size))
                {
                    enq_ok_++;
                    enqueued = true;
                    break;
                }
                taskYIELD();
            }
            if (!enqueued)
            {
                enq_fail_++;
            }
            wait_until(target_t);
        }
    }

    elapsed_us_ = esp_timer_get_time() - t0;
    running_ = false;
    ESP_LOGI(TAG, "done enq_ok=%u enq_fail=%u elapsed_us=%lld",
             (unsigned)enq_ok_, (unsigned)enq_fail_,
             (long long)elapsed_us_);
}
