#ifndef WINJECT_MANAGER_STREAM_H_
#define WINJECT_MANAGER_STREAM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct stream_stats_s
{
    const char* proto = nullptr;
    // STREAM-N interval: unfecced / decoded payload bytes (reset by take_stats).
    uint64_t tx_bytes = 0;
    uint64_t rx_bytes = 0;
    // Lifetime unfecced / decoded payload bytes (never reset).
    uint64_t tx_bytes_life = 0;
    uint64_t rx_bytes_life = 0;
    // STREAM TOTAL interval: on-air datagram bytes (FEC shards, TCP headers).
    uint64_t air_tx_bytes = 0;
    uint64_t air_rx_bytes = 0;
    // Lifetime packet counts (UDP): app ingest, air pull, radio deliver.
    uint64_t tx_pkt_life = 0;
    uint64_t rx_pkt_life = 0;
    uint64_t air_tx_pkt_life = 0;
    // Lifetime silent drops when txq hit k_max_udp_queue.
    uint64_t drop_txq = 0;
    size_t queue = 0;
    size_t unacked = 0;
    bool tcp = false;
    int fec_k = 0;
    int fec_n = 0;
    uint64_t fec_recovered = 0;
    uint64_t fec_fail = 0;
};

inline void format_fec(char* buf, size_t n, int k, int n_shards)
{
    if (buf == nullptr || n == 0)
    {
        return;
    }
    if (k > 0 && n_shards > k)
    {
        snprintf(buf, n, "block(%d,%d)", k, n_shards);
    }
    else
    {
        snprintf(buf, n, "none");
    }
}

class stream
{
public:
    virtual ~stream() = default;
    virtual void on_radio_rx(const uint8_t* data, size_t len) = 0;
    virtual bool has_tx() const = 0;
    virtual bool has_ack() const
    {
        return false;
    }
    virtual size_t pull_tx(uint8_t* out, size_t max, bool* is_ack) = 0;
    // UDP-style TX queue: copy without dequeue until commit_tx after air send.
    virtual bool supports_peek_tx() const
    {
        return false;
    }
    virtual size_t peek_tx(uint8_t* out, size_t max, bool* is_ack)
    {
        (void)out;
        (void)max;
        (void)is_ack;
        return 0;
    }
    virtual void commit_tx() {}
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
    // Current interval counters. Does not reset.
    virtual stream_stats_s peek_stats() const
    {
        return stream_stats_s{};
    }
    // Interval counters for periodic STREAM stats. Zeros payload and air totals.
    virtual stream_stats_s take_stats()
    {
        stream_stats_s s;
        s.rx_bytes = take_rx_bytes();
        return s;
    }
};

#endif  // WINJECT_MANAGER_STREAM_H_
