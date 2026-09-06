#ifndef WINJECT_MANAGER_TX_SCHEDULER_H_
#define WINJECT_MANAGER_TX_SCHEDULER_H_

#include "config.h"

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
    void configure(uint32_t max_rate_kbps);
    void add(stream* up, wifi_udp* radio, size_t budget);
    void tick();
    void log_stats(double interval_sec, const std::vector<stream*>& ups);
    uint64_t take_air_bytes();

private:
    void refill();

    struct slot_s
    {
        stream* up = nullptr;
        wifi_udp* radio = nullptr;
        size_t budget = 0;
    };

    uint32_t rate_kbps = 10000;
    uint64_t tokens = 0;
    uint64_t burst = 0;
    size_t next = 0;
    bool ticking = false;
    bool tick_again = false;
    std::chrono::steady_clock::time_point last_refill{};
    std::vector<slot_s> slots;
    uint8_t buf[2048]{};
    uint64_t air_bytes_interval = 0;
};

#endif  // WINJECT_MANAGER_TX_SCHEDULER_H_
