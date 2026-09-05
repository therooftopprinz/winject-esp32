#ifndef WINJECT_MANAGER_TCP_STREAM_H_
#define WINJECT_MANAGER_TCP_STREAM_H_

#include "upstream.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

class TcpStream : public Upstream
{
public:
    static constexpr uint8_t kTypeData = 1;
    static constexpr uint8_t kTypeAck = 2;
    static constexpr uint8_t kTypeConnect = 3;
    static constexpr uint8_t kTypeClose = 4;
    static constexpr uint8_t kTypeAbort = 5;
    static constexpr size_t kHeaderSize = 8;
    // Ethernet 1500 - IPv4 20 - UDP 8 = 1472. kMaxSegment 1468 made 1476-byte
    // datagrams that IP-fragment (4-byte second fragment); the sink then sees
    // a short frame, drops DATA, never advances rx_seq, and the window sticks.
    // 1400 matches the working bw_test / RTP size.
    static constexpr size_t kMaxUdpPayload = 1472;
    static constexpr size_t kMaxSegment = 1400;
    static_assert(kHeaderSize + kMaxSegment <= kMaxUdpPayload,
                  "TCP radio frame must fit in one unfragmented UDP datagram");
    static constexpr size_t kWindow = 256;
    static constexpr size_t kSackBlockSize = 4;  // sn + count
    static constexpr size_t kMaxSackBlocks = 8;
    // Cap host TCP ingest so we return to the reactor and send/receive ACKs.
    static constexpr size_t kTcpInMax = kWindow * kMaxSegment * 2;

    void reset();
    void local_up();
    void local_down();
    void local_abort();
    bool established() const;
    bool ended_by_peer() const
    {
        return peer_close_;
    }
    // Peer has sent CONNECT (retransmits keep this true until CLOSE/ABORT).
    bool peer_connected() const
    {
        return peer_connect_ && !peer_close_;
    }
    bool wants_connect() const
    {
        return peer_connect_ && !local_ && !peer_close_;
    }
    size_t tcp_in_size() const
    {
        return tcp_in_.size() - tcp_in_off_;
    }
    size_t tcp_in_room() const
    {
        const size_t n = tcp_in_size();
        return n < kTcpInMax ? kTcpInMax - n : 0;
    }
    size_t unacked_count() const
    {
        return unacked_.size();
    }
    bool accepts_tcp() const
    {
        return established() && tcp_in_room() > 0;
    }
    void on_tcp_bytes(const uint8_t* data, size_t len);
    bool pull_tcp(uint8_t* out, size_t max, size_t* n);

    void on_radio_rx(const uint8_t* data, size_t len) override;
    bool has_tx() const override;
    bool has_ack() const override;
    size_t pull_tx(uint8_t* out, size_t max, bool* is_ack) override;
    void on_tick() override;
    bool should_give_up() const
    {
        return connect_give_up_ || data_stall_give_up_;
    }
    void clear_give_up()
    {
        connect_give_up_ = false;
        data_stall_give_up_ = false;
    }
    bool connect_timed_out() const
    {
        return connect_give_up_;
    }
    bool data_stalled() const
    {
        return data_stall_give_up_;
    }

private:
    struct Pending
    {
        uint16_t seq = 0;
        std::vector<uint8_t> frame;
        std::chrono::steady_clock::time_point sent{};
        bool in_flight = false;
        bool sacked = false;
    };

    static void write_hdr(uint8_t* p, uint8_t type, uint16_t seq, uint16_t ack,
                          uint16_t len);
    void apply_ack(uint16_t ack);
    void apply_sack(const uint8_t* data, size_t len);
    void queue_ctrl(uint8_t type);
    void queue_data();
    void parse_radio(const uint8_t* data, size_t len);
    void compact_tcp_in();
    void deliver_data(uint16_t seq, const uint8_t* payload, size_t plen);
    size_t fill_sack_payload(uint8_t* out, size_t max) const;
    bool has_pending_connect() const;

    bool local_ = false;
    bool peer_connect_ = false;
    bool peer_close_ = false;
    bool ack_pending_ = false;
    bool connect_give_up_ = false;
    bool data_stall_give_up_ = false;
    uint16_t tx_seq_ = 0;
    uint16_t rx_seq_ = 0;
    uint16_t tx_acked_ = 0;
    size_t tcp_in_off_ = 0;
    std::chrono::steady_clock::time_point connect_started_{};
    std::chrono::steady_clock::time_point last_connect_{};
    std::chrono::steady_clock::time_point last_ack_progress_{};
    std::vector<uint8_t> tcp_in_;
    std::deque<uint8_t> tcp_out_;
    std::deque<Pending> unacked_;
    std::deque<std::vector<uint8_t>> ctrlq_;
    // Out-of-order RX buffer: seq -> payload (selective repeat).
    std::map<uint16_t, std::vector<uint8_t>> reorder_;
};

#endif  // WINJECT_MANAGER_TCP_STREAM_H_
