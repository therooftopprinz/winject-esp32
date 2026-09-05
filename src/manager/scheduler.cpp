#include "scheduler.h"

#include <algorithm>

#include "log.h"
#include "net_util.h"
#include "radio_udp.h"
#include "upstream.h"

namespace
{
// Cap DATA MPDUs per wakeup so reverse ACKs can win CCA between bursts.
constexpr size_t kMaxDataPerTick = 8;
}  // namespace

void Scheduler::configure(uint32_t max_rate_kbps)
{
    rate_kbps_ = max_rate_kbps < 64 ? 64 : max_rate_kbps;
    // ~32 wifi frames, or ~8 ms of rate, whichever is larger.
    burst_ = std::max<uint64_t>(kWifiPayloadMax * 32,
                                static_cast<uint64_t>(rate_kbps_) / 8 * 8);
    tokens_ = burst_;
    last_refill_ = std::chrono::steady_clock::now();
}

void Scheduler::add(Upstream* up, RadioUdp* radio, size_t budget)
{
    slots_.push_back(Slot{up, radio, budget});
}

void Scheduler::refill()
{
    const auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  now - last_refill_)
                  .count();
    if (us < 0)
    {
        us = 0;
    }
    last_refill_ = now;
    tokens_ +=
        (static_cast<uint64_t>(rate_kbps_) * static_cast<uint64_t>(us)) / 8000;
    if (tokens_ > burst_)
    {
        tokens_ = burst_;
    }
}

void Scheduler::tick()
{
    if (slots_.empty())
    {
        return;
    }
    // Radio on_idle and TCP kick can nest; coalesce into one follow-up pass.
    if (ticking_)
    {
        tick_again_ = true;
        return;
    }
    ticking_ = true;
    unsigned passes = 0;
    do
    {
        tick_again_ = false;
        refill();
        for (auto& s : slots_)
        {
            if (s.up != nullptr)
            {
                s.up->on_tick();
            }
        }

        std::vector<size_t> remain(slots_.size());
        for (size_t i = 0; i < slots_.size(); i++)
        {
            remain[i] = slots_[i].budget;
        }

        size_t data_sent = 0;

        auto send_one = [&](size_t i, bool acks_only) -> bool
        {
            auto& s = slots_[i];
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
                if (remain[i] == 0 || tokens_ == 0 ||
                    data_sent >= kMaxDataPerTick)
                {
                    return false;
                }
                if (!s.up->has_tx() && !s.up->has_ack())
                {
                    return false;
                }
            }

            size_t max = kWifiPayloadMax;
            if (!acks_only)
            {
                max = remain[i] < tokens_ ? remain[i]
                                          : static_cast<size_t>(tokens_);
                if (max > kWifiPayloadMax)
                {
                    max = kWifiPayloadMax;
                }
                if (max == 0)
                {
                    return false;
                }
            }

            bool is_ack = false;
            const size_t n = s.up->pull_tx(buf_, max, &is_ack);
            if (n == 0)
            {
                return false;
            }
            if (!s.radio->send(buf_, n))
            {
                return false;
            }
            air_bytes_interval_ += n;
            // ACKs/ctrl are exempt from the data rate bucket.
            if (!is_ack)
            {
                if (n <= remain[i])
                {
                    remain[i] -= n;
                }
                else
                {
                    remain[i] = 0;
                }
                if (n <= tokens_)
                {
                    tokens_ -= n;
                }
                else
                {
                    tokens_ = 0;
                }
                data_sent++;
            }
            return true;
        };

        bool ack_progress = true;
        while (ack_progress)
        {
            ack_progress = false;
            for (size_t i = 0; i < slots_.size(); i++)
            {
                if (send_one(i, true))
                {
                    ack_progress = true;
                }
            }
        }

        bool progress = true;
        while (progress && tokens_ > 0 && data_sent < kMaxDataPerTick)
        {
            progress = false;
            for (size_t n = 0; n < slots_.size() && tokens_ > 0 &&
                               data_sent < kMaxDataPerTick;
                 n++)
            {
                const size_t i = (next_ + n) % slots_.size();
                if (send_one(i, false))
                {
                    progress = true;
                    next_ = (i + 1) % slots_.size();
                }
            }
        }
        passes++;
    } while (tick_again_ && passes < 2);
    tick_again_ = false;
    ticking_ = false;
}

uint64_t Scheduler::take_air_bytes()
{
    const uint64_t n = air_bytes_interval_;
    air_bytes_interval_ = 0;
    return n;
}

void Scheduler::log_stats(double interval_sec,
                          const std::vector<Upstream*>& ups)
{
    auto kbps = [interval_sec](uint64_t bytes) -> double
    {
        return interval_sec > 0.0 ? (bytes * 8.0) / interval_sec / 1000.0 : 0.0;
    };

    struct Row
    {
        size_t index = 0;
        StreamStats st;
        double tx_kbps = 0.0;
        double rx_kbps = 0.0;
    };
    std::vector<Row> rows;
    uint64_t total_tx = take_air_bytes();
    uint64_t total_rx = 0;
    for (size_t i = 0; i < ups.size(); i++)
    {
        if (ups[i] == nullptr)
        {
            continue;
        }
        StreamStats st = ups[i]->take_stats();
        if (st.proto == nullptr)
        {
            continue;
        }
        total_rx += st.air_rx_bytes;
        rows.push_back(Row{i, st, kbps(st.tx_bytes), kbps(st.rx_bytes)});
    }

    LOG_INF("STREAM TOTAL TX=%6.0f RX=%6.0f", kbps(total_tx), kbps(total_rx));
    for (const Row& row : rows)
    {
        if (row.st.tcp)
        {
            LOG_INF("STREAM-%zu %s TX=%6.0f RX=%6.0f QUEUE=%zu UNACKED=%zu",
                    row.index, row.st.proto, row.tx_kbps, row.rx_kbps,
                    row.st.queue, row.st.unacked);
        }
        else
        {
            if (row.st.fec_recovered != 0 || row.st.fec_fail != 0)
            {
                LOG_INF(
                    "STREAM-%zu %s TX=%6.0f RX=%6.0f FEC_REC=%llu "
                    "FEC_FAIL=%llu",
                    row.index, row.st.proto, row.tx_kbps, row.rx_kbps,
                    static_cast<unsigned long long>(row.st.fec_recovered),
                    static_cast<unsigned long long>(row.st.fec_fail));
            }
            else
            {
                LOG_INF("STREAM-%zu %s TX=%6.0f RX=%6.0f", row.index,
                        row.st.proto, row.tx_kbps, row.rx_kbps);
            }
        }
    }
}
