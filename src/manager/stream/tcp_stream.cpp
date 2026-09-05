#include "stream/tcp_stream.h"

#include "net_util.h"

#include <arpa/inet.h>
#include <string.h>

#include <iterator>

namespace
{
constexpr auto k_rexmit = std::chrono::milliseconds(20);
constexpr auto k_connect_timeout = std::chrono::seconds(30);
// No cumulative ACK progress with outstanding DATA: stop retx before the
// radio inject path is wedged by an endless UNACKED window.
constexpr auto k_data_stall_timeout = std::chrono::seconds(5);

uint16_t read_u16(const uint8_t* p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return ntohs(v);
}

void write_u16(uint8_t* p, uint16_t v)
{
    const uint16_t n = htons(v);
    memcpy(p, &n, 2);
}
}  // namespace

void tcp_stream::write_hdr(uint8_t* p, uint8_t type, uint16_t seq, uint16_t ack,
                          uint16_t len)
{
    p[0] = type;
    p[1] = 0;
    write_u16(p + 2, seq);
    write_u16(p + 4, ack);
    write_u16(p + 6, len);
}

void tcp_stream::reset()
{
    local_ = false;
    peer_connect_ = false;
    peer_close_ = false;
    ack_pending_ = false;
    connect_give_up_ = false;
    data_stall_give_up_ = false;
    tx_seq_ = 0;
    rx_seq_ = 0;
    tx_acked_ = 0;
    tcp_in_off_ = 0;
    tcp_in_.clear();
    tcp_out_.clear();
    unacked_.clear();
    ctrlq_.clear();
    reorder_.clear();
}

void tcp_stream::local_up()
{
    if (local_)
    {
        return;
    }
    local_ = true;
    connect_give_up_ = false;
    data_stall_give_up_ = false;
    connect_started_ = std::chrono::steady_clock::now();
    last_connect_ = connect_started_;
    last_ack_progress_ = connect_started_;
    queue_ctrl(k_type_connect);
}

bool tcp_stream::has_pending_connect() const
{
    for (const auto& f : ctrlq_)
    {
        if (f[0] == k_type_connect)
        {
            return true;
        }
    }
    return false;
}

void tcp_stream::local_down()
{
    if (!local_)
    {
        return;
    }
    local_ = false;
    peer_connect_ = false;
    // Repeat CLOSE: the peer has no retx timer for ctrl, and a dying
    // manager may only get one scheduler pass on the way out.
    queue_ctrl(k_type_close);
    queue_ctrl(k_type_close);
    queue_ctrl(k_type_close);
    unacked_.clear();
    tcp_in_.clear();
    tcp_in_off_ = 0;
    reorder_.clear();
}

void tcp_stream::local_abort()
{
    if (!local_)
    {
        return;
    }
    local_ = false;
    peer_connect_ = false;
    queue_ctrl(k_type_abort);
    unacked_.clear();
    tcp_in_.clear();
    tcp_in_off_ = 0;
    reorder_.clear();
}

bool tcp_stream::established() const
{
    return local_ && peer_connect_ && !peer_close_;
}

void tcp_stream::queue_ctrl(uint8_t type)
{
    std::vector<uint8_t> f(k_header_size);
    write_hdr(f.data(), type, tx_seq_, rx_seq_, 0);
    ctrlq_.push_back(std::move(f));
}

void tcp_stream::compact_tcp_in()
{
    if (tcp_in_off_ == 0)
    {
        return;
    }
    if (tcp_in_off_ >= tcp_in_.size())
    {
        tcp_in_.clear();
        tcp_in_off_ = 0;
        return;
    }
    tcp_in_.erase(tcp_in_.begin(),
                  tcp_in_.begin() + static_cast<std::ptrdiff_t>(tcp_in_off_));
    tcp_in_off_ = 0;
}

void tcp_stream::on_tcp_bytes(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0 || !established())
    {
        return;
    }
    tcp_in_.insert(tcp_in_.end(), data, data + len);
    queue_data();
}

void tcp_stream::queue_data()
{
    if (!established())
    {
        return;
    }
    while (unacked_.size() < k_window && tcp_in_off_ < tcp_in_.size())
    {
        const size_t avail = tcp_in_.size() - tcp_in_off_;
        const size_t n = avail < k_max_segment ? avail : k_max_segment;
        pending_s p;
        p.seq = tx_seq_;
        p.frame.resize(k_header_size + n);
        write_hdr(p.frame.data(), k_type_data, tx_seq_, rx_seq_,
                  static_cast<uint16_t>(n));
        memcpy(p.frame.data() + k_header_size, tcp_in_.data() + tcp_in_off_, n);
        tcp_in_off_ += n;
        tx_seq_ = static_cast<uint16_t>(tx_seq_ + 1);
        const bool first_outstanding = unacked_.empty();
        unacked_.push_back(std::move(p));
        if (first_outstanding)
        {
            // Stall timer runs only while DATA is outstanding.
            last_ack_progress_ = std::chrono::steady_clock::now();
        }
    }
    if (tcp_in_off_ > 65536 || tcp_in_off_ == tcp_in_.size())
    {
        compact_tcp_in();
    }
}

bool tcp_stream::pull_tcp(uint8_t* out, size_t max, size_t* n)
{
    if (n == nullptr || out == nullptr || tcp_out_.empty() || max == 0)
    {
        if (n != nullptr)
        {
            *n = 0;
        }
        return false;
    }
    size_t c = tcp_out_.size() < max ? tcp_out_.size() : max;
    for (size_t i = 0; i < c; i++)
    {
        out[i] = tcp_out_.front();
        tcp_out_.pop_front();
    }
    *n = c;
    return true;
}

void tcp_stream::apply_ack(uint16_t ack)
{
    const uint16_t delta = static_cast<uint16_t>(ack - tx_acked_);
    if (delta == 0 || delta > unacked_.size() || delta > k_window)
    {
        return;
    }
    for (uint16_t i = 0; i < delta; i++)
    {
        unacked_.pop_front();
        tx_acked_ = static_cast<uint16_t>(tx_acked_ + 1);
    }
    last_ack_progress_ = std::chrono::steady_clock::now();
    data_stall_give_up_ = false;
}

void tcp_stream::apply_sack(const uint8_t* data, size_t len)
{
    for (size_t off = 0; off + k_sack_block_size <= len; off += k_sack_block_size)
    {
        const uint16_t sn = read_u16(data + off);
        const uint16_t count = read_u16(data + off + 2);
        if (count == 0 || count > k_window)
        {
            continue;
        }
        for (uint16_t i = 0; i < count; i++)
        {
            const uint16_t s = static_cast<uint16_t>(sn + i);
            for (auto& p : unacked_)
            {
                if (p.seq == s)
                {
                    p.sacked = true;
                    break;
                }
            }
        }
    }
}

size_t tcp_stream::fill_sack_payload(uint8_t* out, size_t max) const
{
    if (out == nullptr || max < k_sack_block_size || reorder_.empty())
    {
        return 0;
    }
    size_t nblocks = 0;
    size_t off = 0;
    for (auto it = reorder_.begin(); it != reorder_.end() && nblocks < k_max_sack_blocks;)
    {
        if (off + k_sack_block_size > max)
        {
            break;
        }
        const uint16_t sn = it->first;
        uint16_t count = 1;
        auto jt = std::next(it);
        while (jt != reorder_.end() &&
               jt->first == static_cast<uint16_t>(sn + count))
        {
            count++;
            ++jt;
        }
        write_u16(out + off, sn);
        write_u16(out + off + 2, count);
        off += k_sack_block_size;
        nblocks++;
        it = jt;
    }
    return off;
}

void tcp_stream::deliver_data(uint16_t seq, const uint8_t* payload, size_t plen)
{
    // Buffer as soon as the peer CONNECT is seen. Requiring local_ (full
    // established) drops seq 0 while TCP_CLIENT is still connecting to gst.
    if (payload == nullptr || !peer_connect_ || peer_close_)
    {
        return;
    }
    const uint16_t dist = static_cast<uint16_t>(seq - rx_seq_);
    if (dist >= k_window)
    {
        // Outside window (duplicate behind or too far ahead).
        ack_pending_ = true;
        return;
    }
    if (seq == rx_seq_)
    {
        if (plen > 0)
        {
            tcp_out_.insert(tcp_out_.end(), payload, payload + plen);
        }
        rx_seq_ = static_cast<uint16_t>(rx_seq_ + 1);
        while (true)
        {
            auto it = reorder_.find(rx_seq_);
            if (it == reorder_.end())
            {
                break;
            }
            tcp_out_.insert(tcp_out_.end(), it->second.begin(), it->second.end());
            reorder_.erase(it);
            rx_seq_ = static_cast<uint16_t>(rx_seq_ + 1);
        }
    }
    else if (reorder_.find(seq) == reorder_.end())
    {
        reorder_.emplace(seq, std::vector<uint8_t>(payload, payload + plen));
    }
    ack_pending_ = true;
}

void tcp_stream::parse_radio(const uint8_t* data, size_t len)
{
    if (len < k_header_size)
    {
        return;
    }
    const uint8_t type = data[0];
    const uint16_t seq = read_u16(data + 2);
    const uint16_t ack = read_u16(data + 4);
    const uint16_t plen = read_u16(data + 6);
    if (len < k_header_size + plen)
    {
        return;
    }

    apply_ack(ack);
    if (type == k_type_ack && plen > 0)
    {
        apply_sack(data + k_header_size, plen);
    }

    if (type == k_type_connect)
    {
        if (!peer_connect_)
        {
            rx_seq_ = 0;
            tcp_out_.clear();
            reorder_.clear();
        }
        peer_connect_ = true;
        peer_close_ = false;
        ack_pending_ = true;
        return;
    }
    if (type == k_type_close || type == k_type_abort)
    {
        // Handshake still in progress: keep CONNECT retries until timeout.
        if (!established())
        {
            ack_pending_ = true;
            return;
        }
        peer_close_ = true;
        peer_connect_ = false;
        return;
    }
    if (type == k_type_ack)
    {
        return;
    }
    if (type == k_type_data)
    {
        deliver_data(seq, data + k_header_size, plen);
    }
}

void tcp_stream::on_radio_rx(const uint8_t* data, size_t len)
{
    parse_radio(data, len);
    queue_data();
}

bool tcp_stream::has_ack() const
{
    return ack_pending_ || !ctrlq_.empty();
}

bool tcp_stream::has_tx() const
{
    if (has_ack())
    {
        return true;
    }
    for (const auto& p : unacked_)
    {
        if (!p.in_flight && !p.sacked)
        {
            return true;
        }
    }
    return false;
}

size_t tcp_stream::pull_tx(uint8_t* out, size_t max, bool* is_ack)
{
    if (out == nullptr || max < k_header_size)
    {
        return 0;
    }
    if (!ctrlq_.empty())
    {
        auto& f = ctrlq_.front();
        if (f.size() > max)
        {
            return 0;
        }
        write_hdr(out, f[0], read_u16(f.data() + 2), rx_seq_, 0);
        if (is_ack != nullptr)
        {
            *is_ack = true;
        }
        ctrlq_.pop_front();
        return k_header_size;
    }
    if (ack_pending_)
    {
        const size_t sack_room =
            max > k_header_size ? max - k_header_size : 0;
        const size_t sack_n = fill_sack_payload(out + k_header_size, sack_room);
        write_hdr(out, k_type_ack, tx_seq_, rx_seq_,
                  static_cast<uint16_t>(sack_n));
        ack_pending_ = false;
        if (is_ack != nullptr)
        {
            *is_ack = true;
        }
        return k_header_size + sack_n;
    }
    for (auto& p : unacked_)
    {
        if (p.sacked)
        {
            continue;
        }
        if (!p.in_flight && p.frame.size() <= max)
        {
            write_hdr(p.frame.data(), k_type_data, p.seq, rx_seq_,
                      static_cast<uint16_t>(p.frame.size() - k_header_size));
            memcpy(out, p.frame.data(), p.frame.size());
            p.in_flight = true;
            p.sent = std::chrono::steady_clock::now();
            if (is_ack != nullptr)
            {
                *is_ack = false;
            }
            return p.frame.size();
        }
    }
    return 0;
}

void tcp_stream::on_tick()
{
    const auto now = std::chrono::steady_clock::now();
    // Selective Repeat: retx every timed-out hole that was not SACKed.
    // Do not invalidate later in-flight segments (that was GBN).
    for (auto& p : unacked_)
    {
        if (p.sacked)
        {
            continue;
        }
        if (p.in_flight && now - p.sent >= k_rexmit)
        {
            p.in_flight = false;
        }
    }
    if (local_ && !peer_connect_ && !peer_close_)
    {
        if (now - connect_started_ >= k_connect_timeout)
        {
            connect_give_up_ = true;
            return;
        }
        if (now - last_connect_ >= k_rexmit && !has_pending_connect())
        {
            last_connect_ = now;
            queue_ctrl(k_type_connect);
        }
    }
    if (established() && !unacked_.empty() &&
        now - last_ack_progress_ >= k_data_stall_timeout)
    {
        data_stall_give_up_ = true;
        return;
    }
    queue_data();
}
