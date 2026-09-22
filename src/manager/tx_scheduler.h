#ifndef WINJECT_MANAGER_TX_SCHEDULER_H_
#define WINJECT_MANAGER_TX_SCHEDULER_H_

#include "config.h"
#include "frames/air_seq.h"

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <string>
#include <vector>

class wifi_udp;
class stream;

class tx_scheduler
{
public:
    void configure(uint32_t max_rate_kbps, uint16_t domain,
                   size_t max_data_per_tick = 4,
                   size_t tx_burst_size = k_default_tx_burst_size,
                   uint32_t tx_burst_interval_us =
                       k_default_tx_burst_interval_us);
    bool set_max_rate_kbps(uint32_t max_rate_kbps);
    bool set_max_data_per_tick(size_t max_data_per_tick);
    bool set_tx_burst_pacing(size_t tx_burst_size,
                             uint32_t tx_burst_interval_us);
    uint32_t max_rate_kbps() const
    {
        return rate_kbps;
    }
    size_t max_data_per_tick() const
    {
        return max_data_per_tick_;
    }
    size_t tx_burst_size() const
    {
        return tx_burst_size_;
    }
    uint32_t tx_burst_interval_us() const
    {
        return tx_burst_interval_us_;
    }
    void add(stream* up, wifi_udp* radio, uint8_t bus_tx, uint8_t bus_rx,
             size_t budget);
    bool set_budget(size_t index, size_t budget);
    bool get_budget(size_t index, size_t* budget) const;
    void tick();
    void on_mpdu_rx(const uint8_t* mpdu, size_t len);
    void log_stats(double interval_sec, const std::vector<stream*>& ups);
    uint64_t take_air_bytes();
    uint64_t peek_air_bytes() const;
    bool peek_seq_lost(size_t index, uint64_t* lost) const;

private:
    void refill();
    bool emit_mpdu(size_t primary, bool acks_only, std::vector<size_t>& remain,
                   size_t* data_sent);
    bool data_burst_allows() const;
    void note_data_burst_emit();

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
    size_t tx_burst_size_ = k_default_tx_burst_size;
    uint32_t tx_burst_interval_us_ = k_default_tx_burst_interval_us;
    size_t burst_data_sent_ = 0;
    std::chrono::steady_clock::time_point burst_cooldown_until_{};
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
};

#endif  // WINJECT_MANAGER_TX_SCHEDULER_H_
