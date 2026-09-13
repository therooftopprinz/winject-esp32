#ifndef WINJECT_MANAGER_CHANNEL_INFO_H_
#define WINJECT_MANAGER_CHANNEL_INFO_H_

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "net_util.h"
#include "reactor.h"

// Host-side receiver for ESP32 set_upstream_ci UDP telemetry.
// Caches the last FLOW_CTRL and RX_AIR samples; no scheduler coupling.
class channel_info
{
public:
    enum type_e : uint8_t
    {
        flow_ctrl = 1,
        rx_air = 2,
    };

    struct flow_ctrl_s
    {
        uint8_t info_type;
        uint8_t tx_queue_size;
        uint8_t tx_queue_capacity;
    } __attribute__((packed));

    struct rx_air_s
    {
        uint8_t info_type;
        int8_t rssi;
        int8_t snr;
    } __attribute__((packed));

    struct cached_flow_ctrl_s
    {
        bool valid = false;
        uint8_t tx_queue_size = 0;
        uint8_t tx_queue_capacity = 0;
        timespec received{};
    };

    struct cached_rx_air_s
    {
        bool valid = false;
        int8_t rssi = 0;
        int8_t snr = 0;
        timespec received{};
    };

    // Local wall clock of a cached sample, or "-" when !valid.
    static void format_stamp(bool valid, const timespec& ts, char* buf,
                             size_t n);

    channel_info() = default;
    ~channel_info();
    channel_info(const channel_info&) = delete;
    channel_info& operator=(const channel_info&) = delete;

    // Bind an exclusive ephemeral UDP port and register with the reactor.
    bool open(::reactor& reactor);
    void close();

    uint16_t port() const
    {
        return port_;
    }
    cached_flow_ctrl_s last_flow_ctrl() const
    {
        return flow_;
    }
    cached_rx_air_s last_rx_air() const
    {
        return air_;
    }

private:
    void on_datagram();

    ::reactor* reactor = nullptr;
    bfc::socket sock;
    uint16_t port_ = 0;
    cached_flow_ctrl_s flow_{};
    cached_rx_air_s air_{};
    uint8_t buf[64]{};
};

#endif  // WINJECT_MANAGER_CHANNEL_INFO_H_
