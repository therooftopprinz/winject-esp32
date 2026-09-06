#ifndef WINJECT_MANAGER_TCP_ENDPOINT_H_
#define WINJECT_MANAGER_TCP_ENDPOINT_H_

#include "config.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "net_util.h"
#include "reactor.h"
#include "stream/stream.h"
#include "stream/tcp_stream.h"

class tcp_endpoint : public stream
{
public:
    ~tcp_endpoint() override;
    virtual bool open(::reactor& reactor, const upstream_config_s& cfg);
    virtual void close();
    void set_tx_kick(std::function<void()> kick);

    void on_radio_rx(const uint8_t* data, size_t len) override;
    bool has_tx() const override;
    bool has_ack() const override;
    size_t pull_tx(uint8_t* out, size_t max, bool* is_ack) override;
    void on_tick() override;
    void announce_down() override;
    uint64_t take_rx_bytes() override;
    stream_stats_s take_stats() override;

protected:
    tcp_endpoint() = default;

    virtual void on_peer_end() = 0;
    virtual void maybe_connect() {}
    virtual void on_incoming_connect() {}
    virtual bool is_connecting() const
    {
        return false;
    }
    virtual void on_client_write();
    virtual void close_client();

    void kick_tx();
    void apply_buffers(bfc::socket& sock);
    bool start_rx_thread();
    bool watch_client_write();
    void flush_tcp();
    void sync_rx_accept();

    ::reactor* reactor = nullptr;
    upstream_config_s cfg{};
    bfc::socket client_sock;
    tcp_stream stream;

private:
    void stop_rx_thread();
    void rx_loop();
    void on_rx_wake();
    void on_local_fin();
    void on_local_abort();

    std::function<void()> tx_kick;
    uint8_t buf[16384]{};
    // If send() returns EAGAIN mid-write, keep the unsent tail and resume on
    // the next writable callback. Pulling from tcp_stream discards bytes, so
    // we must not lose them on backpressure.
    size_t tx_pending_len = 0;
    size_t tx_pending_off = 0;
    uint64_t radio_rx_bytes_interval = 0;
    uint64_t air_tx_bytes_interval = 0;
    std::atomic<uint64_t> app_rx_bytes_interval{0};
    uint64_t app_tx_bytes_interval = 0;

    // Blocking RX on a dedicated thread; bytes are marshaled to the reactor.
    int rx_fd = -1;
    std::thread rx_thread;
    std::atomic<bool> rx_stop{false};
    std::atomic<bool> rx_wake_pending{false};
    // Updated only on the reactor thread; RX thread reads it to pause/resume.
    std::atomic<bool> rx_accept{true};
    mutable std::mutex rx_mu;
    std::vector<uint8_t> rx_q;
};

#endif  // WINJECT_MANAGER_TCP_ENDPOINT_H_
