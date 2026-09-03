#ifndef WINJECT_MANAGER_UPSTREAM_H_
#define WINJECT_MANAGER_UPSTREAM_H_

#include <stddef.h>
#include <stdint.h>

struct StreamStats
{
    const char* proto = nullptr;
    uint64_t tx_bytes = 0;
    uint64_t rx_bytes = 0;
    size_t queue = 0;
    size_t unacked = 0;
    bool tcp = false;
    uint64_t fec_recovered = 0;
    uint64_t fec_fail = 0;
};

class Upstream
{
public:
    virtual ~Upstream() = default;
    virtual void on_radio_rx(const uint8_t* data, size_t len) = 0;
    virtual bool has_tx() const = 0;
    virtual bool has_ack() const
    {
        return false;
    }
    virtual size_t pull_tx(uint8_t* out, size_t max, bool* is_ack) = 0;
    virtual void on_tick() {}
    // Queue a radio CLOSE/FIN. Call before the last scheduler tick on exit.
    virtual void announce_down() {}
    // True when the stream is backing off due to loss or queue pressure.
    virtual bool congested() const
    {
        return false;
    }
    // Bytes delivered from radio since the last take (for interval kbps).
    virtual uint64_t take_rx_bytes()
    {
        return 0;
    }
    // Interval counters for periodic STREAM stats. Zeros TX/RX byte totals.
    virtual StreamStats take_stats()
    {
        StreamStats s;
        s.rx_bytes = take_rx_bytes();
        return s;
    }
};

#endif  // WINJECT_MANAGER_UPSTREAM_H_
