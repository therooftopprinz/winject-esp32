#ifndef WINJECT_MANAGER_TCP_ENDPOINT_H_
#define WINJECT_MANAGER_TCP_ENDPOINT_H_

#include "config.h"
#include "stream/stream.h"
#include "net_util.h"
#include "reactor.h"
#include "stream/tcp_stream.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class tcp_endpoint : public stream
{
public:
    tcp_endpoint() = default;
    ~tcp_endpoint() override;
    bool open(reactor& reactor, const upstream_config_s& cfg);
    void close();
    void set_tx_kick(std::function<void()> kick);

    void on_radio_rx(const uint8_t* data, size_t len) override;
    bool has_tx() const override;
    bool has_ack() const override;
    size_t pull_tx(uint8_t* out, size_t max, bool* is_ack) override;
    void on_tick() override;
    void announce_down() override;
    uint64_t take_rx_bytes() override;
    stream_stats_s take_stats() override;

private:
    void kick_tx();
    void stop_rx_thread();
    bool start_rx_thread();
    void rx_loop();
    void on_rx_wake();
    void close_client();
    void on_local_fin();
    void on_local_abort();
    void on_peer_end();
    void on_listen();
    void on_client_write();
    void on_connecting();
    bool start_connect();
    bool watch_client_write();
    void apply_buffers(bfc::socket& sock);
    void flush_tcp();
    void sync_rx_accept();

    reactor* reactor_ = nullptr;
    upstream_config_s cfg_{};
    bfc::socket listen_sock_;
    bfc::socket client_sock_;
    bool connecting_ = false;
    tcp_stream stream_;
    std::function<void()> tx_kick_;
    uint8_t buf_[16384]{};
    // If send() returns EAGAIN mid-write, keep the unsent tail and resume on
    // the next writable callback. Pulling from tcp_stream discards bytes, so
    // we must not lose them on backpressure.
    size_t tx_pending_len_ = 0;
    size_t tx_pending_off_ = 0;
    uint64_t radio_rx_bytes_interval_ = 0;
    uint64_t air_tx_bytes_interval_ = 0;
    std::atomic<uint64_t> app_rx_bytes_interval_{0};
    uint64_t app_tx_bytes_interval_ = 0;

    // Blocking RX on a dedicated thread; bytes are marshaled to the reactor.
    int rx_fd_ = -1;
    std::thread rx_thread_;
    std::atomic<bool> rx_stop_{false};
    std::atomic<bool> rx_wake_pending_{false};
    // Updated only on the reactor thread; RX thread reads it to pause/resume.
    std::atomic<bool> rx_accept_{true};
    mutable std::mutex rx_mu_;
    std::vector<uint8_t> rx_q_;
};

#endif  // WINJECT_MANAGER_TCP_ENDPOINT_H_
