#include "tx_scheduler.h"

#include <algorithm>
#include <string.h>

#include "log.h"
#include "net_util.h"
#include "radio/mpdu.h"
#include "radio/wifi_udp.h"
#include "stream/stream.h"

namespace
{
}  // namespace

void tx_scheduler::configure(uint32_t max_rate_kbps, uint16_t domain,
                             size_t max_data_per_tick, size_t tx_burst_size,
                             uint32_t tx_burst_interval_us)
{
    rate_kbps = max_rate_kbps < 64 ? 64 : max_rate_kbps;
    domain_ = domain;
    max_data_per_tick_ = max_data_per_tick < 1 ? 1 : max_data_per_tick;
    if (max_data_per_tick_ > 32)
    {
        max_data_per_tick_ = 32;
    }
    tx_burst_size_ = tx_burst_size < 1 ? 1 : tx_burst_size;
    if (tx_burst_size_ > k_radio_tx_queue_depth)
    {
        tx_burst_size_ = k_radio_tx_queue_depth;
    }
    tx_burst_interval_us_ = tx_burst_interval_us;
    burst = k_wifi_payload_max * 2;
    tokens = burst;
    burst_data_sent_ = 0;
    burst_cooldown_until_ = {};
    last_refill = std::chrono::steady_clock::now();
}

bool tx_scheduler::set_max_rate_kbps(uint32_t max_rate_kbps)
{
    rate_kbps = max_rate_kbps < 64 ? 64 : max_rate_kbps;
    return true;
}

bool tx_scheduler::set_max_data_per_tick(size_t max_data_per_tick)
{
    max_data_per_tick_ = max_data_per_tick < 1 ? 1 : max_data_per_tick;
    if (max_data_per_tick_ > 32)
    {
        max_data_per_tick_ = 32;
    }
    return true;
}

bool tx_scheduler::set_tx_burst_pacing(size_t tx_burst_size,
                                       uint32_t tx_burst_interval_us)
{
    tx_burst_size_ = tx_burst_size < 1 ? 1 : tx_burst_size;
    if (tx_burst_size_ > k_radio_tx_queue_depth)
    {
        tx_burst_size_ = k_radio_tx_queue_depth;
    }
    tx_burst_interval_us_ = tx_burst_interval_us;
    burst_data_sent_ = 0;
    burst_cooldown_until_ = {};
    return true;
}

bool tx_scheduler::data_burst_allows() const
{
    const auto now = std::chrono::steady_clock::now();
    if (now < burst_cooldown_until_)
    {
        return false;
    }
    return burst_data_sent_ < tx_burst_size_;
}

void tx_scheduler::note_data_burst_emit()
{
    burst_data_sent_++;
    if (burst_data_sent_ < tx_burst_size_)
    {
        return;
    }
    burst_cooldown_until_ =
        std::chrono::steady_clock::now() +
        std::chrono::microseconds(tx_burst_interval_us_);
    burst_data_sent_ = 0;
}

void tx_scheduler::add(stream* up, wifi_udp* radio, uint8_t bus_tx,
                       uint8_t bus_rx, size_t budget)
{
    slots.push_back(slot_s{up, radio, bus_tx, bus_rx, budget, {}, 0});
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

bool tx_scheduler::emit_mpdu(size_t primary, bool acks_only,
                             std::vector<size_t>& remain, size_t* data_sent)
{
    if (primary >= slots.size() || data_sent == nullptr || domain_ == 0)
    {
        return false;
    }
    auto& s0 = slots[primary];
    if (s0.up == nullptr || s0.radio == nullptr || s0.bus_tx == 0)
    {
        return false;
    }
    if (acks_only)
    {
        if (!s0.up->has_ack())
        {
            return false;
        }
    }
    else
    {
        if (!data_burst_allows())
        {
            return false;
        }
        if (tokens == 0 || *data_sent >= max_data_per_tick_)
        {
            return false;
        }
        if (!s0.up->has_tx() && !s0.up->has_ack())
        {
            return false;
        }
    }

    pdu_slot_t air_slots[WIFI_PDU_SLOTS] = {};
    const uint8_t* bodies[WIFI_PDU_SLOTS] = {};
    size_t body_total = 0;
    uint8_t n = 0;
    bool any_data = false;

    std::vector<size_t> commit_counts(slots.size(), 0);

    auto try_pull = [&](size_t i) -> bool
    {
        if (n >= WIFI_PDU_SLOTS)
        {
            return false;
        }
        auto& s = slots[i];
        if (s.up == nullptr || s.bus_tx == 0)
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
        else if (!s.up->has_tx() && !s.up->has_ack())
        {
            return false;
        }

        size_t max = k_stream_payload_max;
        if (!acks_only)
        {
            const bool first_data = (remain[i] == slots[i].budget);
            if (!first_data && remain[i] == 0)
            {
                return false;
            }
            const size_t room = k_wifi_payload_max - body_total;
            if (room < k_air_seq_len + 1)
            {
                return false;
            }
            size_t room_payload = room - k_air_seq_len;
            if (tokens < max)
            {
                max = static_cast<size_t>(tokens);
            }
            if (!first_data && remain[i] < max)
            {
                max = remain[i];
            }
            if (room_payload < max)
            {
                max = room_payload;
            }
            if (max == 0)
            {
                return false;
            }
        }
        else
        {
            const size_t room = k_wifi_payload_max - body_total;
            if (room < k_air_seq_len + 1)
            {
                return false;
            }
            max = room - k_air_seq_len;
            if (max > k_stream_payload_max)
            {
                max = k_stream_payload_max;
            }
        }

        bool is_ack = false;
        size_t pulled = 0;
        if (s.up->supports_peek_tx())
        {
            pulled = s.up->peek_tx(pull_buf[n], max, &is_ack);
        }
        else
        {
            pulled = s.up->pull_tx(pull_buf[n], max, &is_ack);
        }
        if (pulled == 0)
        {
            return false;
        }
        if (s.up->supports_peek_tx())
        {
            commit_counts[i]++;
        }
        size_t framed = 0;
        if (!s.seq.stamp(framed_buf[n], sizeof(framed_buf[n]), pull_buf[n],
                         pulled, &framed))
        {
            return false;
        }
        air_slots[n] = {s.bus_tx, static_cast<uint16_t>(framed)};
        bodies[n] = framed_buf[n];
        body_total += framed;
        if (!is_ack)
        {
            any_data = true;
            if (framed <= remain[i])
            {
                remain[i] -= framed;
            }
            else
            {
                remain[i] = 0;
            }
            if (framed <= tokens)
            {
                tokens -= framed;
            }
            else
            {
                tokens = 0;
            }
        }
        n++;
        return true;
    };

    if (!try_pull(primary))
    {
        return false;
    }
    // Opportunistically pack more PDUs into the same MPDU.
    for (size_t step = 1; step < slots.size() && n < WIFI_PDU_SLOTS; step++)
    {
        const size_t i = (primary + step) % slots.size();
        try_pull(i);
    }

    size_t mpdu_len = 0;
    if (!mpdu_build(mpdu_buf, sizeof(mpdu_buf), &mpdu_len, air_slots, bodies,
                    domain_))
    {
        return false;
    }
    if (!s0.radio->send(mpdu_buf, mpdu_len))
    {
        return false;
    }
    for (size_t i = 0; i < slots.size(); i++)
    {
        for (size_t c = 0; c < commit_counts[i]; c++)
        {
            if (slots[i].up != nullptr && slots[i].up->supports_peek_tx())
            {
                slots[i].up->commit_tx();
            }
        }
    }
    air_bytes_interval += body_total;
    if (any_data)
    {
        (*data_sent)++;
        note_data_burst_emit();
    }
    return true;
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

        bool ack_progress = true;
        while (ack_progress)
        {
            ack_progress = false;
            for (size_t i = 0; i < slots.size(); i++)
            {
                if (emit_mpdu(i, true, remain, &data_sent))
                {
                    ack_progress = true;
                }
            }
        }

        bool progress = true;
        while (progress && tokens > 0 && data_sent < max_data_per_tick_)
        {
            progress = false;
            for (size_t n = 0; n < slots.size() && tokens > 0 &&
                               data_sent < max_data_per_tick_;
                 n++)
            {
                const size_t i = (next + n) % slots.size();
                if (emit_mpdu(i, false, remain, &data_sent))
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

void tx_scheduler::on_mpdu_rx(const uint8_t* mpdu, size_t len)
{
    pdu_slot_t air_slots[WIFI_PDU_SLOTS] = {};
    const uint8_t* body = nullptr;
    size_t body_len = 0;
    if (!mpdu_unpack(mpdu, len, air_slots, &body, &body_len) || body == nullptr)
    {
        return;
    }
    size_t off = 0;
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        if (air_slots[i].size == 0)
        {
            continue;
        }
        if (off + air_slots[i].size > body_len)
        {
            return;
        }
        const uint8_t* pdu = body + off;
        const size_t pdu_len = air_slots[i].size;
        off += pdu_len;

        for (auto& s : slots)
        {
            if (s.up == nullptr || s.bus_rx == 0 || s.bus_rx != air_slots[i].bus)
            {
                continue;
            }
            const uint8_t* payload = nullptr;
            size_t plen = 0;
            if (!s.seq.accept(pdu, pdu_len, &payload, &plen))
            {
                break;
            }
            s.up->on_radio_rx(payload, plen);
            break;
        }
    }
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

bool tx_scheduler::peek_seq_lost(size_t index, uint64_t* lost) const
{
    if (lost == nullptr || index >= slots.size())
    {
        return false;
    }
    *lost = slots[index].seq.lost();
    return true;
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
        if (i < slots.size())
        {
            const uint64_t now = slots[i].seq.lost();
            lost = now - slots[i].lost_seen;
            slots[i].lost_seen = now;
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
