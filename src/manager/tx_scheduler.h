#ifndef WINJECT_MANAGER_TX_SCHEDULER_H_
#define WINJECT_MANAGER_TX_SCHEDULER_H_

#include "config.h"
#include "frames/air_seq.h"

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

class wifi_udp;
class stream;

class tx_scheduler
{
public:
    void configure(uint32_t max_rate_kbps, uint16_t domain,
                   size_t max_data_per_tick = 4);
    // When false, defer DATA MPDUs (ACKs still sent). Used for radio TX queue CI.
    void set_may_emit_data(std::function<bool()> gate);
    void set_on_data_mpdu_sent(std::function<void()> hook);
    void add(stream* up, wifi_udp* radio, uint8_t bus_tx, uint8_t bus_rx,
             size_t budget);
    // Update per-upstream inject budget (bytes per scheduler wakeup).
    bool set_budget(size_t index, size_t budget);
    bool get_budget(size_t index, size_t* budget) const;
    void tick();
    // Demux a full MPDU: air_seq strip per LCP, deliver by rx_bus.
    void on_mpdu_rx(const uint8_t* mpdu, size_t len);
    void log_stats(double interval_sec, const std::vector<stream*>& ups);
    uint64_t take_air_bytes();
    uint64_t peek_air_bytes() const;
    // Lifetime air_seq gap count for upstream index (RX demux side).
    bool peek_seq_lost(size_t index, uint64_t* lost) const;

private:
    void refill();
    bool emit_mpdu(size_t primary, bool acks_only, std::vector<size_t>& remain,
                   size_t* data_sent);

    struct slot_s
    {
        stream* up = nullptr;
        wifi_udp* radio = nullptr;
        uint8_t bus_tx = 0;
        uint8_t bus_rx = 0;
        size_t budget = 0;
        air_seq seq;
        uint64_t lost_seen = 0;
    };

    uint32_t rate_kbps = 10000;
    uint16_t domain_ = 0;
    size_t max_data_per_tick_ = 4;
    uint64_t tokens = 0;
    uint64_t burst = 0;
    size_t next = 0;
    bool ticking = false;
    bool tick_again = false;
    std::chrono::steady_clock::time_point last_refill{};
    std::vector<slot_s> slots;
    uint8_t pull_buf[5][2048]{};
    uint8_t framed_buf[5][2048]{};
    uint8_t mpdu_buf[1500]{};
    uint64_t air_bytes_interval = 0;
    std::function<bool()> may_emit_data_;
    std::function<void()> on_data_mpdu_sent_;
};

#endif  // WINJECT_MANAGER_TX_SCHEDULER_H_
