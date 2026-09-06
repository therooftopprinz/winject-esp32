#include "endpoint/tcp_endpoint.h"

#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"

tcp_endpoint::~tcp_endpoint()
{
    close();
}

void tcp_endpoint::set_tx_kick(std::function<void()> kick)
{
    tx_kick = std::move(kick);
}

void tcp_endpoint::kick_tx()
{
    if (tx_kick)
    {
        tx_kick();
    }
}

void tcp_endpoint::apply_buffers(bfc::socket& sock)
{
    if (cfg.rcv_buffer_size > 0)
    {
        sock.set_sock_opt(SOL_SOCKET, SO_RCVBUF, cfg.rcv_buffer_size);
    }
    if (cfg.snd_buffer_size > 0)
    {
        sock.set_sock_opt(SOL_SOCKET, SO_SNDBUF, cfg.snd_buffer_size);
    }
    sock.set_sock_opt(IPPROTO_TCP, TCP_NODELAY, 1);
}

bool tcp_endpoint::open(::reactor& reactor, const upstream_config_s& cfg)
{
    this->reactor = &reactor;
    this->cfg = cfg;
    return true;
}

void tcp_endpoint::close()
{
    close_client();
}

void tcp_endpoint::stop_rx_thread()
{
    rx_stop = true;
    if (rx_fd >= 0)
    {
        // Unblock a sleeping recv without racing the reactor write path.
        ::shutdown(rx_fd, SHUT_RD);
        ::close(rx_fd);
        rx_fd = -1;
    }
    if (rx_thread.joinable())
    {
        rx_thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(rx_mu);
        rx_q.clear();
    }
    rx_wake_pending = false;
    rx_accept = true;
}

bool tcp_endpoint::start_rx_thread()
{
    stop_rx_thread();
    if (client_sock.fd() < 0 || reactor == nullptr)
    {
        return false;
    }
    rx_fd = ::dup(client_sock.fd());
    if (rx_fd < 0)
    {
        LOG_ERR("tcp rx dup: %s", strerror(errno));
        return false;
    }
    if (!set_blocking(rx_fd))
    {
        LOG_ERR("tcp rx set_blocking failed");
        ::close(rx_fd);
        rx_fd = -1;
        return false;
    }
    rx_stop = false;
    rx_wake_pending = false;
    rx_accept = true;
    rx_thread = std::thread(
        [this]()
        {
            rx_loop();
        });
    return true;
}

void tcp_endpoint::sync_rx_accept()
{
    // Pause when the stream cannot ingest (handshake or tcp_in full). Keep
    // rx_q so mpegts PAT/PMT at the start of the gst socket is not dropped.
    bool ok = client_sock.fd() >= 0 && !is_connecting() && stream.accepts_tcp();
    rx_accept.store(ok, std::memory_order_relaxed);
    if (!ok)
    {
        return;
    }
    bool has_q = false;
    {
        std::lock_guard<std::mutex> lock(rx_mu);
        has_q = !rx_q.empty();
    }
    if (has_q && !rx_wake_pending.exchange(true) && reactor != nullptr)
    {
        reactor->wake_up(
            [this]()
            {
                on_rx_wake();
            });
    }
}

void tcp_endpoint::rx_loop()
{
    uint8_t buf[16384];
    while (!rx_stop)
    {
        size_t qn = 0;
        {
            std::lock_guard<std::mutex> lock(rx_mu);
            qn = rx_q.size();
        }
        // Pause when the reactor says so, or before rx_q grows without bound
        // while CONNECT is still in flight.
        if (!rx_accept.load(std::memory_order_relaxed) ||
            qn >= tcp_stream::k_tcp_in_max)
        {
            pollfd pfd = {rx_fd, POLLIN, 0};
            poll(&pfd, 1, 5);
            continue;
        }
        const ssize_t n = ::recv(rx_fd, buf, sizeof(buf), 0);
        if (rx_stop)
        {
            break;
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (reactor != nullptr)
            {
                reactor->wake_up(
                    [this]()
                    {
                        on_local_fin();
                    });
            }
            break;
        }
        if (n == 0)
        {
            if (reactor != nullptr)
            {
                reactor->wake_up(
                    [this]()
                    {
                        on_local_fin();
                    });
            }
            break;
        }
        {
            std::lock_guard<std::mutex> lock(rx_mu);
            rx_q.insert(rx_q.end(), buf, buf + n);
        }
        if (!rx_wake_pending.exchange(true) && reactor != nullptr)
        {
            reactor->wake_up(
                [this]()
                {
                    on_rx_wake();
                });
        }
    }
}

void tcp_endpoint::on_rx_wake()
{
    rx_wake_pending = false;
    if (!stream.accepts_tcp())
    {
        // Handshake or ingest full: keep rx_q until the stream can take it.
        sync_rx_accept();
        return;
    }

    // Peel at most tcp_in_room() from the front of rx_q. Do not swap the
    // whole queue out: while saturated the RX thread pauses when rx_q is
    // full, and an empty rx_q during wake would let it recv another cap.
    // Never call sync_rx_accept() while holding rx_mu: it locks the same
    // non-recursive mutex (stale wake with an empty queue deadlocks the
    // reactor and the RX thread).
    std::vector<uint8_t> chunk;
    {
        std::lock_guard<std::mutex> lock(rx_mu);
        if (!rx_q.empty())
        {
            const size_t room = stream.tcp_in_room();
            const size_t n = room < rx_q.size() ? room : rx_q.size();
            if (n > 0)
            {
                chunk.assign(rx_q.begin(),
                             rx_q.begin() + static_cast<std::ptrdiff_t>(n));
                rx_q.erase(rx_q.begin(),
                           rx_q.begin() + static_cast<std::ptrdiff_t>(n));
            }
        }
    }
    if (chunk.empty())
    {
        sync_rx_accept();
        return;
    }
    stream.on_tcp_bytes(chunk.data(), chunk.size());
    app_rx_bytes_interval.fetch_add(chunk.size(), std::memory_order_relaxed);
    kick_tx();
    sync_rx_accept();
    bool again = false;
    {
        std::lock_guard<std::mutex> lock(rx_mu);
        again = !rx_q.empty();
    }
    if (again && stream.accepts_tcp() && !rx_wake_pending.exchange(true) &&
        reactor != nullptr)
    {
        reactor->wake_up(
            [this]()
            {
                on_rx_wake();
            });
    }
}

void tcp_endpoint::close_client()
{
    stop_rx_thread();
    if (client_sock.fd() >= 0)
    {
        if (reactor != nullptr)
        {
            reactor->rem_write_rdy(client_sock.fd());
        }
        close_socket(&client_sock);
    }
    // Clear any partially-sent TCP payload. Pulling from tcp_stream discards
    // bytes, so unsent tails must not survive across clients.
    tx_pending_len = 0;
    tx_pending_off = 0;
}

void tcp_endpoint::on_local_fin()
{
    if (client_sock.fd() >= 0)
    {
        stream.local_down();
    }
    close_client();
    // Do not reset(): that drops the CLOSE still sitting in ctrlq.
    kick_tx();
    sync_rx_accept();
}

void tcp_endpoint::on_local_abort()
{
    if (client_sock.fd() >= 0)
    {
        stream.local_abort();
    }
    close_client();
    kick_tx();
    sync_rx_accept();
}

bool tcp_endpoint::watch_client_write()
{
    const int fd = client_sock.fd();
    if (reactor == nullptr || fd < 0)
    {
        return false;
    }
    return reactor->add_write_rdy(fd,
                                  [this]()
                                  {
                                      on_client_write();
                                  });
}

void tcp_endpoint::flush_tcp()
{
    if (client_sock.fd() < 0 || is_connecting())
    {
        return;
    }
    while (true)
    {
        // If we previously hit EAGAIN mid-write, resume from the unsent tail.
        if (tx_pending_len > tx_pending_off)
        {
            const size_t n = tx_pending_len;
            while (tx_pending_off < n)
            {
                const ssize_t w = send(client_sock.fd(), buf + tx_pending_off,
                                       n - tx_pending_off, MSG_NOSIGNAL);
                if (w < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        reactor->req_write(client_sock.fd());
                        return;
                    }
                    if (client_sock.fd() >= 0)
                    {
                        stream.local_down();
                    }
                    close_client();
                    kick_tx();
                    sync_rx_accept();
                    return;
                }
                tx_pending_off += static_cast<size_t>(w);
                app_tx_bytes_interval += static_cast<uint64_t>(w);
            }
            // Fully sent current pending chunk; clear and pull the next.
            tx_pending_len = 0;
            tx_pending_off = 0;
        }
        else
        {
            size_t n = 0;
            if (!stream.pull_tcp(buf, sizeof(buf), &n) || n == 0)
            {
                return;
            }
            tx_pending_len = n;
            tx_pending_off = 0;
            continue;
        }
    }
}

void tcp_endpoint::on_client_write()
{
    flush_tcp();
}

size_t tcp_endpoint::pull_tx(uint8_t* out, size_t max, bool* is_ack)
{
    const size_t n = stream.pull_tx(out, max, is_ack);
    air_tx_bytes_interval += n;
    return n;
}

uint64_t tcp_endpoint::take_rx_bytes()
{
    const uint64_t n = radio_rx_bytes_interval;
    radio_rx_bytes_interval = 0;
    return n;
}

stream_stats_s tcp_endpoint::take_stats()
{
    stream_stats_s s;
    s.proto = "TCP";
    s.tcp = true;
    s.air_tx_bytes = air_tx_bytes_interval;
    s.air_rx_bytes = radio_rx_bytes_interval;
    s.tx_bytes = air_tx_bytes_interval;
    s.rx_bytes = take_rx_bytes();
    air_tx_bytes_interval = 0;
    app_rx_bytes_interval.exchange(0, std::memory_order_relaxed);
    app_tx_bytes_interval = 0;
    size_t rxq = 0;
    {
        std::lock_guard<std::mutex> lock(rx_mu);
        rxq = rx_q.size();
    }
    s.queue = stream.tcp_in_size() + rxq;
    s.unacked = stream.unacked_count();
    return s;
}

void tcp_endpoint::on_radio_rx(const uint8_t* data, size_t len)
{
    if (data != nullptr && len > 0)
    {
        radio_rx_bytes_interval += len;
    }
    const bool connect = len >= tcp_stream::k_header_size &&
                         data[0] == tcp_stream::k_type_connect;
    if (connect)
    {
        on_incoming_connect();
    }
    stream.on_radio_rx(data, len);
    maybe_connect();
    if (stream.ended_by_peer())
    {
        on_peer_end();
        return;
    }
    sync_rx_accept();
    flush_tcp();
}

bool tcp_endpoint::has_tx() const
{
    return stream.has_tx();
}

bool tcp_endpoint::has_ack() const
{
    return stream.has_ack();
}

void tcp_endpoint::announce_down()
{
    stream.local_down();
    close_client();
}

void tcp_endpoint::on_tick()
{
    stream.on_tick();
    if (stream.should_give_up())
    {
        if (stream.data_stalled())
        {
            LOG_WRN(
                "tcp data stall on radio (no ACK progress); aborting stream");
            on_local_abort();
        }
        else
        {
            LOG_WRN("tcp connect timeout on radio");
            on_local_fin();
        }
        stream.clear_give_up();
        return;
    }
    maybe_connect();
    sync_rx_accept();
    flush_tcp();
}
