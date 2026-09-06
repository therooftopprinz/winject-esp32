#ifndef WINJECT_LC_TX_H_
#define WINJECT_LC_TX_H_

#include "config.h"
#include "packet.h"

#include <atomic>
#include <optional>
#include <stdint.h>

#include "bfc-esp32/wait_free_queue.hpp"
#include "freertos/FreeRTOS.h"

class channel_info_endpoint;

class lc_tx
{
public:
    static constexpr uint8_t k_queue_cap = WIFI_RADIO_TX_QUEUE;

    static lc_tx& instance();
    lc_tx(const lc_tx&) = delete;
    lc_tx& operator=(const lc_tx&) = delete;

    bool init();
    void set_channel_info(channel_info_endpoint& ci);

    bool tx(bus_t bus, packet&& pkt);
    bool peek(bus_t* bus, uint16_t* payload_size) const;
    packet pop(bus_t* bus, TickType_t wait);

    uint8_t queue_size() const;
    uint8_t queue_capacity() const
    {
        return k_queue_cap;
    }
    uint32_t drop_count() const;

private:
    lc_tx() = default;

    void publish_flow_ctrl();

    struct slot_s
    {
        bus_t bus = 0;
        std::optional<packet> pkt;
    };

    bfc::wait_free_queue<slot_s, k_queue_cap> q;
    channel_info_endpoint* ci = nullptr;
    std::atomic<uint32_t> drop_count_{0};
};

#endif  // WINJECT_LC_TX_H_
