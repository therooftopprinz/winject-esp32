#ifndef WINJECT_MANAGER_TCP_STREAM_H_
#define WINJECT_MANAGER_TCP_STREAM_H_

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

// Reliable byte pipe over radio UDP for TCP forwarding (ARQ + SACK).
class tcp_stream
{
public:
    static constexpr uint8_t k_type_data = 1;
    static constexpr uint8_t k_type_ack = 2;
    static constexpr uint8_t k_type_connect = 3;
    static constexpr uint8_t k_type_close = 4;
    static constexpr uint8_t k_type_abort = 5;
    static constexpr size_t k_header_size = 8;
    static constexpr size_t k_max_udp_payload = 1472;
    static constexpr size_t k_max_segment = 1400;
    static_assert(k_header_size + k_max_segment <= k_max_udp_payload,
                  "TCP radio frame must fit in one unfragmented UDP datagram");
    static constexpr size_t k_window = 256;
    static constexpr size_t k_sack_block_size = 4;  // sn + count
    static constexpr size_t k_max_sack_blocks = 8;
    static constexpr size_t k_tcp_in_max = k_window * k_max_segment * 2;

    void reset();
    void local_up();
    void local_down();
    void local_abort();
    bool established() const;
    bool ended_by_peer() const
    {
        return peer_close_;
    }

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
        return n < k_tcp_in_max ? k_tcp_in_max - n : 0;
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

    void on_radio_rx(const uint8_t* data, size_t len);
    bool has_tx() const;
    bool has_ack() const;
    size_t pull_tx(uint8_t* out, size_t max, bool* is_ack);
    void on_tick();
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
    struct pending_s
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
    std::deque<pending_s> unacked_;
    std::deque<std::vector<uint8_t>> ctrlq_;
    // Out-of-order RX buffer: seq -> payload (selective repeat).
    std::map<uint16_t, std::vector<uint8_t>> reorder_;
};

#endif  // WINJECT_MANAGER_TCP_STREAM_H_
