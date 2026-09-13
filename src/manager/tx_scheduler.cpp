#include "tx_scheduler.h"

#include <algorithm>

#include "log.h"
#include "net_util.h"
#include "radio/wifi_udp.h"
#include "stream/stream.h"

namespace
{
// Cap DATA MPDUs per wakeup so reverse ACKs can win CCA between bursts.
constexpr size_t k_max_data_per_tick = 8;
}  // namespace

void tx_scheduler::configure(uint32_t max_rate_kbps)
{
    rate_kbps = max_rate_kbps < 64 ? 64 : max_rate_kbps;
    // ~32 wifi frames, or ~8 ms of rate, whichever is larger.
    burst = std::max<uint64_t>(k_wifi_payload_max * 32,
                                static_cast<uint64_t>(rate_kbps) / 8 * 8);
    tokens = burst;
    last_refill = std::chrono::steady_clock::now();
}

void tx_scheduler::add(stream* up, wifi_udp* radio, size_t budget)
{
    slots.push_back(slot_s{up, radio, budget});
}

bool tx_scheduler::set_budget(size_t index, size_t budget)
{
    if (index >= slots.size() || budget == 0)
    {
        return false;
    }
    slots[index].budget = budget;
    return true;
}

bool tx_scheduler::get_budget(size_t index, size_t* budget) const
{
    if (index >= slots.size() || budget == nullptr)
    {
        return false;
    }
    *budget = slots[index].budget;
    return true;
}

void tx_scheduler::refill()
{
    const auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  now - last_refill)
                  .count();
    if (us < 0)
    {
        us = 0;
    }
    last_refill = now;
    tokens +=
        (static_cast<uint64_t>(rate_kbps) * static_cast<uint64_t>(us)) / 8000;
    if (tokens > burst)
    {
        tokens = burst;
    }
}

void tx_scheduler::tick()
{
    if (slots.empty())
    {
        return;
    }
    // Radio on_idle and TCP kick can nest; coalesce into one follow-up pass.
    if (ticking)
    {
        tick_again = true;
        return;
    }
    ticking = true;
    unsigned passes = 0;
    do
    {
        tick_again = false;
        refill();
        for (auto& s : slots)
        {
            if (s.up != nullptr)
            {
                s.up->on_tick();
            }
        }

        std::vector<size_t> remain(slots.size());
        for (size_t i = 0; i < slots.size(); i++)
        {
            remain[i] = slots[i].budget;
        }

        size_t data_sent = 0;

        auto send_one = [&](size_t i, bool acks_only) -> bool
        {
            auto& s = slots[i];
            if (s.up == nullptr || s.radio == nullptr)
            {
                return false;
            }
            if (acks_only)
            {
                if (!s.up->has_ack())
                {
                    return false;
                }
            }
            else
            {
                if (tokens == 0 || data_sent >= k_max_data_per_tick)
                {
                    return false;
                }
                if (!s.up->has_tx() && !s.up->has_ack())
                {
                    return false;
                }
            }

            size_t max = k_stream_payload_max;
            if (!acks_only)
            {
                // First datagram this wakeup may exceed scheduler_budget so a
                // single gci / help reply cannot stall the TX queue forever.
                const bool first_data = (remain[i] == slots[i].budget);
                if (!first_data && remain[i] == 0)
                {
                    return false;
                }
                if (tokens < max)
                {
                    max = static_cast<size_t>(tokens);
                }
                if (!first_data && remain[i] < max)
                {
                    max = remain[i];
                }
                if (max == 0)
                {
                    return false;
                }
            }

            bool is_ack = false;
            const size_t n = s.up->pull_tx(buf, max, &is_ack);
            if (n == 0)
            {
                return false;
            }
            if (!s.radio->send(buf, n))
            {
                return false;
            }
            const size_t air_n = n + k_air_seq_len;
            air_bytes_interval += air_n;
            // ACKs/ctrl are exempt from the data rate bucket.
            if (!is_ack)
            {
                if (air_n <= remain[i])
                {
                    remain[i] -= air_n;
                }
                else
                {
                    remain[i] = 0;
                }
                if (air_n <= tokens)
                {
                    tokens -= air_n;
                }
                else
                {
                    tokens = 0;
                }
                data_sent++;
            }
            return true;
        };

        bool ack_progress = true;
        while (ack_progress)
        {
            ack_progress = false;
            for (size_t i = 0; i < slots.size(); i++)
            {
                if (send_one(i, true))
                {
                    ack_progress = true;
                }
            }
        }

        bool progress = true;
        while (progress && tokens > 0 && data_sent < k_max_data_per_tick)
        {
            progress = false;
            for (size_t n = 0; n < slots.size() && tokens > 0 &&
                               data_sent < k_max_data_per_tick;
                 n++)
            {
                const size_t i = (next + n) % slots.size();
                if (send_one(i, false))
                {
                    progress = true;
                    next = (i + 1) % slots.size();
                }
            }
        }
        passes++;
    } while (tick_again && passes < 2);
    tick_again = false;
    ticking = false;
}

uint64_t tx_scheduler::take_air_bytes()
{
    const uint64_t n = air_bytes_interval;
    air_bytes_interval = 0;
    return n;
}

uint64_t tx_scheduler::peek_air_bytes() const
{
    return air_bytes_interval;
}

void tx_scheduler::log_stats(double interval_sec,
                          const std::vector<stream*>& ups)
{
    auto kbps = [interval_sec](uint64_t bytes) -> double
    {
        return interval_sec > 0.0 ? (bytes * 8.0) / interval_sec / 1000.0 : 0.0;
    };

    struct row_s
    {
        size_t index = 0;
        stream_stats_s st;
        double tx_kbps = 0.0;
        double rx_kbps = 0.0;
        uint64_t rx_lost = 0;
    };
    std::vector<row_s> rows;
    uint64_t total_tx = take_air_bytes();
    uint64_t total_rx = 0;
    uint64_t total_lost = 0;
    for (size_t i = 0; i < ups.size(); i++)
    {
        if (ups[i] == nullptr)
        {
            continue;
        }
        stream_stats_s st = ups[i]->take_stats();
        if (st.proto == nullptr)
        {
            continue;
        }
        total_rx += st.air_rx_bytes;
        uint64_t lost = 0;
        if (i < slots.size() && slots[i].radio != nullptr)
        {
            lost = slots[i].radio->take_lost_interval();
        }
        total_lost += lost;
        rows.push_back(row_s{i, st, kbps(st.tx_bytes), kbps(st.rx_bytes), lost});
    }

    LOG_INF("STREAM TOTAL TX=%6.0f RX=%6.0f LOST=%llu", kbps(total_tx),
            kbps(total_rx), static_cast<unsigned long long>(total_lost));
    for (const row_s& row : rows)
    {
        if (row.st.tcp)
        {
            LOG_INF("STREAM-%zu %s TX=%6.0f RX=%6.0f QUEUE=%zu UNACKED=%zu "
                    "LOST=%llu",
                    row.index, row.st.proto, row.tx_kbps, row.rx_kbps,
                    row.st.queue, row.st.unacked,
                    static_cast<unsigned long long>(row.rx_lost));
        }
        else if (row.st.fec_recovered != 0 || row.st.fec_fail != 0)
        {
            LOG_INF("STREAM-%zu %s TX=%6.0f RX=%6.0f FEC_REC=%llu "
                    "FEC_FAIL=%llu LOST=%llu",
                    row.index, row.st.proto, row.tx_kbps, row.rx_kbps,
                    static_cast<unsigned long long>(row.st.fec_recovered),
                    static_cast<unsigned long long>(row.st.fec_fail),
                    static_cast<unsigned long long>(row.rx_lost));
        }
        else
        {
            LOG_INF("STREAM-%zu %s TX=%6.0f RX=%6.0f LOST=%llu", row.index,
                    row.st.proto, row.tx_kbps, row.rx_kbps,
                    static_cast<unsigned long long>(row.rx_lost));
        }
    }
}
