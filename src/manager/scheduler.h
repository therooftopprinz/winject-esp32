#ifndef WINJECT_MANAGER_SCHEDULER_H_
#define WINJECT_MANAGER_SCHEDULER_H_

#include "config.h"

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <string>
#include <vector>

class RadioUdp;
class Upstream;

class Scheduler
{
public:
    void configure(uint32_t max_rate_kbps);
    void add(Upstream* up, RadioUdp* radio, size_t budget);
    void tick();
    void log_stats(double interval_sec, const std::vector<Upstream*>& ups);
    uint64_t take_air_bytes();

private:
    void refill();

    struct Slot
    {
        Upstream* up = nullptr;
        RadioUdp* radio = nullptr;
        size_t budget = 0;
    };

    uint32_t rate_kbps_ = 10000;
    uint64_t tokens_ = 0;
    uint64_t burst_ = 0;
    size_t next_ = 0;
    bool ticking_ = false;
    bool tick_again_ = false;
    std::chrono::steady_clock::time_point last_refill_{};
    std::vector<Slot> slots_;
    uint8_t buf_[2048]{};
    uint64_t air_bytes_interval_ = 0;
};

#endif  // WINJECT_MANAGER_SCHEDULER_H_
