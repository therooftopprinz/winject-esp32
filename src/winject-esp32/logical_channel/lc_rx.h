#ifndef WINJECT_LC_RX_H_
#define WINJECT_LC_RX_H_

#include "config.h"
#include "packet.h"

#include <atomic>
#include <optional>
#include <stdint.h>

#include "bfc-esp32/wait_free_queue.hpp"
#include "freertos/FreeRTOS.h"

class lc_rx_endpoint;

class lc_rx
{
public:
    static constexpr uint8_t k_queue_cap = WIFI_RADIO_RX_QUEUE;

    static lc_rx& instance();
    lc_rx(const lc_rx&) = delete;
    lc_rx& operator=(const lc_rx&) = delete;

    bool init();
    bool start(BaseType_t core = APP_TASK_CORE,
               UBaseType_t prio = UPSTREAM_TASK_PRIO,
               uint32_t stack_bytes = 6144);

    void set_endpoint(lc_rx_endpoint& ep);

    // Wait-free. From wifi_rx CB only: enqueue and return.
    // Do not allocate, take mutexes, or call wake_up here — that runs on
    // the Wi-Fi task, which also completes esp_wifi_80211_tx.
    bool rx(packet&& pkt);

    uint8_t queue_size() const;
    uint8_t queue_capacity() const
    {
        return k_queue_cap;
    }
    uint32_t drop_count() const;
    uint32_t bad_mpdu_count() const;

private:
    lc_rx() = default;

    static void task(void* arg);
    void run();
    void handle_mpdu(packet&& mpdu);
    packet pop(TickType_t wait);

    bfc::wait_free_queue<std::optional<packet>, k_queue_cap> q_;
    lc_rx_endpoint* ep_ = nullptr;
    std::atomic<uint32_t> drop_count_{0};
    std::atomic<uint32_t> bad_mpdu_count_{0};
};

#endif  // WINJECT_LC_RX_H_
