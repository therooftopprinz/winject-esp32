#include "endpoint/tcp_endpoint.h"

#include "log.h"

#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <cstdio>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

tcp_endpoint::~tcp_endpoint()
{
    close();
}

void tcp_endpoint::set_tx_kick(std::function<void()> kick)
{
    tx_kick_ = std::move(kick);
}

void tcp_endpoint::kick_tx()
{
    if (tx_kick_)
    {
        tx_kick_();
    }
}

void tcp_endpoint::apply_buffers(bfc::socket& sock)
{
    if (cfg_.rcv_buffer_size > 0)
    {
        sock.set_sock_opt(SOL_SOCKET, SO_RCVBUF, cfg_.rcv_buffer_size);
    }
    if (cfg_.snd_buffer_size > 0)
    {
        sock.set_sock_opt(SOL_SOCKET, SO_SNDBUF, cfg_.snd_buffer_size);
    }
    sock.set_sock_opt(IPPROTO_TCP, TCP_NODELAY, 1);
}

bool tcp_endpoint::open(reactor& reactor, const upstream_config_s& cfg)
{
    reactor_ = &reactor;
    cfg_ = cfg;
    if (cfg.mode == upstream_mode_e::tcp_server)
    {
        listen_sock_ = make_tcp4();
        sockaddr_in addr = {};
        if (listen_sock_.fd() < 0 || !parse_host_port(cfg.bind_address, &addr) ||
            listen_sock_.bind(addr) < 0 || listen_sock_.listen(1) < 0)
        {
            LOG_ERR("tcp listen %s: %s", cfg.bind_address.c_str(),
                    strerror(errno));
            return false;
        }
        return reactor.add_read_rdy(listen_sock_.fd(),
                                    [this]()
                                    {
                                        on_listen();
                                    });
    }
    return true;
}

void tcp_endpoint::close()
{
    close_client();
    if (listen_sock_.fd() >= 0)
    {
        if (reactor_ != nullptr)
        {
            reactor_->rem_read_rdy(listen_sock_.fd());
        }
        close_socket(&listen_sock_);
    }
}

void tcp_endpoint::stop_rx_thread()
{
    rx_stop_ = true;
    if (rx_fd_ >= 0)
    {
        // Unblock a sleeping recv without racing the reactor write path.
        ::shutdown(rx_fd_, SHUT_RD);
        ::close(rx_fd_);
        rx_fd_ = -1;
    }
    if (rx_thread_.joinable())
    {
        rx_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(rx_mu_);
        rx_q_.clear();
    }
    rx_wake_pending_ = false;
    rx_accept_ = true;
}

bool tcp_endpoint::start_rx_thread()
{
    stop_rx_thread();
    if (client_sock_.fd() < 0 || reactor_ == nullptr)
    {
        return false;
    }
    rx_fd_ = ::dup(client_sock_.fd());
    if (rx_fd_ < 0)
    {
        LOG_ERR("tcp rx dup: %s", strerror(errno));
        return false;
    }
    if (!set_blocking(rx_fd_))
    {
        LOG_ERR("tcp rx set_blocking failed");
        ::close(rx_fd_);
        rx_fd_ = -1;
        return false;
    }
    rx_stop_ = false;
    rx_wake_pending_ = false;
    rx_accept_ = true;
    rx_thread_ = std::thread([this]()
                             {
                                 rx_loop();
                             });
    return true;
}

void tcp_endpoint::sync_rx_accept()
{
    // Pause when the stream cannot ingest (handshake or tcp_in full). Keep
    // rx_q_ so mpegts PAT/PMT at the start of the gst socket is not dropped.
    bool ok = client_sock_.fd() >= 0 && !connecting_ && stream_.accepts_tcp();
    rx_accept_.store(ok, std::memory_order_relaxed);
    if (!ok)
    {
        return;
    }
    bool has_q = false;
    {
        std::lock_guard<std::mutex> lock(rx_mu_);
        has_q = !rx_q_.empty();
    }
    if (has_q && !rx_wake_pending_.exchange(true) && reactor_ != nullptr)
    {
        reactor_->wake_up([this]()
                          {
                              on_rx_wake();
                          });
    }
}

void tcp_endpoint::rx_loop()
{
    uint8_t buf[16384];
    while (!rx_stop_)
    {
        size_t qn = 0;
        {
            std::lock_guard<std::mutex> lock(rx_mu_);
            qn = rx_q_.size();
        }
        // Pause when the reactor says so, or before rx_q_ grows without bound
        // while CONNECT is still in flight.
        if (!rx_accept_.load(std::memory_order_relaxed) ||
            qn >= tcp_stream::k_tcp_in_max)
        {
            pollfd pfd = {rx_fd_, POLLIN, 0};
            poll(&pfd, 1, 5);
            continue;
        }
        const ssize_t n = ::recv(rx_fd_, buf, sizeof(buf), 0);
        if (rx_stop_)
        {
            break;
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (reactor_ != nullptr)
            {
                reactor_->wake_up([this]()
                                  {
                                      on_local_fin();
                                  });
            }
            break;
        }
        if (n == 0)
        {
            if (reactor_ != nullptr)
            {
                reactor_->wake_up([this]()
                                  {
                                      on_local_fin();
                                  });
            }
            break;
        }
        {
            std::lock_guard<std::mutex> lock(rx_mu_);
            rx_q_.insert(rx_q_.end(), buf, buf + n);
        }
        if (!rx_wake_pending_.exchange(true) && reactor_ != nullptr)
        {
            reactor_->wake_up([this]()
                              {
                                  on_rx_wake();
                              });
        }
    }
}

void tcp_endpoint::on_rx_wake()
{
    rx_wake_pending_ = false;
    if (!stream_.accepts_tcp())
    {
        // Handshake or ingest full: keep rx_q_ until the stream can take it.
        sync_rx_accept();
        return;
    }

    // Peel at most tcp_in_room() from the front of rx_q_. Do not swap the
    // whole queue out: while saturated the RX thread pauses when rx_q_ is
    // full, and an empty rx_q_ during wake would let it recv another cap.
    std::vector<uint8_t> chunk;
    {
        std::lock_guard<std::mutex> lock(rx_mu_);
        if (rx_q_.empty())
        {
            sync_rx_accept();
            return;
        }
        const size_t room = stream_.tcp_in_room();
        const size_t n = room < rx_q_.size() ? room : rx_q_.size();
        if (n == 0)
        {
            sync_rx_accept();
            return;
        }
        chunk.assign(rx_q_.begin(),
                     rx_q_.begin() + static_cast<std::ptrdiff_t>(n));
        rx_q_.erase(rx_q_.begin(),
                    rx_q_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    stream_.on_tcp_bytes(chunk.data(), chunk.size());
    app_rx_bytes_interval_.fetch_add(chunk.size(), std::memory_order_relaxed);
    kick_tx();
    sync_rx_accept();
    bool again = false;
    {
        std::lock_guard<std::mutex> lock(rx_mu_);
        again = !rx_q_.empty();
    }
    if (again && stream_.accepts_tcp() &&
        !rx_wake_pending_.exchange(true) && reactor_ != nullptr)
    {
        reactor_->wake_up([this]()
                          {
                              on_rx_wake();
                          });
    }
}

void tcp_endpoint::close_client()
{
    stop_rx_thread();
    if (client_sock_.fd() >= 0)
    {
        if (reactor_ != nullptr)
        {
            reactor_->rem_write_rdy(client_sock_.fd());
        }
        close_socket(&client_sock_);
    }
    connecting_ = false;
    // Clear any partially-sent TCP payload. Pulling from tcp_stream discards
    // bytes, so unsent tails must not survive across clients.
    tx_pending_len_ = 0;
    tx_pending_off_ = 0;
}

void tcp_endpoint::on_local_fin()
{
    if (client_sock_.fd() >= 0)
    {
        stream_.local_down();
    }
    close_client();
    // Do not reset(): that drops the CLOSE still sitting in ctrlq.
    kick_tx();
    sync_rx_accept();
}

void tcp_endpoint::on_local_abort()
{
    if (client_sock_.fd() >= 0)
    {
        stream_.local_abort();
    }
    close_client();
    kick_tx();
    sync_rx_accept();
}

void tcp_endpoint::on_peer_end()
{
    if (cfg_.mode == upstream_mode_e::tcp_server && client_sock_.fd() >= 0 &&
        !connecting_)
    {
        // Keep the camera gst socket; peer dropped, probe CONNECT again.
        stream_.reset();
        stream_.local_up();
        kick_tx();
        sync_rx_accept();
        return;
    }
    // TCP_CLIENT: close gst so tcpserversrc sees EOF.
    close_client();
    stream_.reset();
}

bool tcp_endpoint::watch_client_write()
{
    const int fd = client_sock_.fd();
    if (reactor_ == nullptr || fd < 0)
    {
        return false;
    }
    return reactor_->add_write_rdy(fd,
                                   [this]()
                                   {
                                       on_client_write();
                                   });
}

void tcp_endpoint::on_listen()
{
    if (listen_sock_.fd() < 0)
    {
        return;
    }
    sockaddr_in addr = {};
    bfc::socket accepted = listen_sock_.accept(addr);
    if (accepted.fd() < 0)
    {
        return;
    }
    if (client_sock_.fd() >= 0)
    {
        LOG_WRN("tcp replacing existing client");
        close_client();
        stream_.reset();
    }
    if (!set_nonblock(accepted.fd()))
    {
        return;
    }
    apply_buffers(accepted);
    client_sock_ = std::move(accepted);
    stream_.reset();
    stream_.local_up();
    if (!watch_client_write() || !start_rx_thread())
    {
        close_client();
        stream_.reset();
        return;
    }
    reactor_->req_write(client_sock_.fd());
    kick_tx();
    LOG_INF("tcp accepted %s", sockaddr_to_string(addr).c_str());
}

bool tcp_endpoint::start_connect()
{
    if (client_sock_.fd() >= 0 || connecting_)
    {
        return true;
    }
    sockaddr_in addr = {};
    if (!parse_host_port(cfg_.connect_address, &addr))
    {
        LOG_ERR("tcp connect_address invalid");
        return false;
    }
    client_sock_ = make_tcp4();
    if (client_sock_.fd() < 0)
    {
        return false;
    }
    apply_buffers(client_sock_);
    const int rv = client_sock_.connect(addr);
    if (rv < 0 && errno != EINPROGRESS)
    {
        LOG_ERR("tcp connect %s: %s", cfg_.connect_address.c_str(),
                strerror(errno));
        close_client();
        return false;
    }
    connecting_ = true;
    if (!watch_client_write())
    {
        close_client();
        return false;
    }
    return reactor_->req_write(client_sock_.fd());
}

void tcp_endpoint::on_connecting()
{
    if (client_sock_.fd() < 0)
    {
        return;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(client_sock_.fd(), SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0)
    {
        LOG_WRN("tcp connect %s failed: %s (retry on next peer CONNECT)",
                cfg_.connect_address.c_str(), strerror(err));
        close_client();
        return;
    }
    connecting_ = false;
    stream_.local_up();
    if (!start_rx_thread())
    {
        close_client();
        return;
    }
    LOG_INF("tcp connected %s", cfg_.connect_address.c_str());
    kick_tx();
    on_client_write();
}

void tcp_endpoint::flush_tcp()
{
    if (client_sock_.fd() < 0 || connecting_)
    {
        return;
    }
    while (true)
    {
        // If we previously hit EAGAIN mid-write, resume from the unsent tail.
        if (tx_pending_len_ > tx_pending_off_)
        {
            const size_t n = tx_pending_len_;
            while (tx_pending_off_ < n)
            {
                const ssize_t w = send(client_sock_.fd(),
                                        buf_ + tx_pending_off_,
                                        n - tx_pending_off_, MSG_NOSIGNAL);
                if (w < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        reactor_->req_write(client_sock_.fd());
                        return;
                    }
                    if (client_sock_.fd() >= 0)
                    {
                        stream_.local_down();
                    }
                    close_client();
                    kick_tx();
                    sync_rx_accept();
                    return;
                }
                tx_pending_off_ += static_cast<size_t>(w);
                app_tx_bytes_interval_ += static_cast<uint64_t>(w);
            }
            // Fully sent current pending chunk; clear and pull the next.
            tx_pending_len_ = 0;
            tx_pending_off_ = 0;
        }
        else
        {
            size_t n = 0;
            if (!stream_.pull_tcp(buf_, sizeof(buf_), &n) || n == 0)
            {
                return;
            }
            tx_pending_len_ = n;
            tx_pending_off_ = 0;
            continue;
        }
    }
}

void tcp_endpoint::on_client_write()
{
    if (connecting_)
    {
        on_connecting();
        return;
    }
    flush_tcp();
}

size_t tcp_endpoint::pull_tx(uint8_t* out, size_t max, bool* is_ack)
{
    const size_t n = stream_.pull_tx(out, max, is_ack);
    air_tx_bytes_interval_ += n;
    return n;
}

uint64_t tcp_endpoint::take_rx_bytes()
{
    const uint64_t n = radio_rx_bytes_interval_;
    radio_rx_bytes_interval_ = 0;
    return n;
}

stream_stats_s tcp_endpoint::take_stats()
{
    stream_stats_s s;
    s.proto = "TCP";
    s.tcp = true;
    s.air_tx_bytes = air_tx_bytes_interval_;
    s.air_rx_bytes = radio_rx_bytes_interval_;
    s.tx_bytes = air_tx_bytes_interval_;
    s.rx_bytes = take_rx_bytes();
    air_tx_bytes_interval_ = 0;
    app_rx_bytes_interval_.exchange(0, std::memory_order_relaxed);
    app_tx_bytes_interval_ = 0;
    size_t rxq = 0;
    {
        std::lock_guard<std::mutex> lock(rx_mu_);
        rxq = rx_q_.size();
    }
    s.queue = stream_.tcp_in_size() + rxq;
    s.unacked = stream_.unacked_count();
    return s;
}

void tcp_endpoint::on_radio_rx(const uint8_t* data, size_t len)
{
    if (data != nullptr && len > 0)
    {
        radio_rx_bytes_interval_ += len;
    }
    const bool connect =
        len >= tcp_stream::k_header_size && data[0] == tcp_stream::k_type_connect;
    // TCP_CLIENT opens the local app socket when the peer CONNECT arrives.
    // Retransmitted CONNECT must never tear down a connecting/live local TCP
    // session — that EOS's tcpserversrc, which sends CLOSE back and makes the
    // server stop air TX (TX LED goes dark right when the client starts).
    if (connect && cfg_.mode == upstream_mode_e::tcp_client &&
        !stream_.peer_connected() && client_sock_.fd() < 0 && !connecting_)
    {
        stream_.reset();
    }
    stream_.on_radio_rx(data, len);
    if (cfg_.mode == upstream_mode_e::tcp_client && stream_.wants_connect())
    {
        start_connect();
    }
    if (stream_.ended_by_peer())
    {
        on_peer_end();
        return;
    }
    sync_rx_accept();
    flush_tcp();
}

bool tcp_endpoint::has_tx() const
{
    return stream_.has_tx();
}

bool tcp_endpoint::has_ack() const
{
    return stream_.has_ack();
}

void tcp_endpoint::announce_down()
{
    stream_.local_down();
    close_client();
}

void tcp_endpoint::on_tick()
{
    stream_.on_tick();
    if (stream_.should_give_up())
    {
        if (stream_.data_stalled())
        {
            LOG_WRN("tcp data stall on radio (no ACK progress); aborting stream");
            on_local_abort();
        }
        else
        {
            LOG_WRN("tcp connect timeout on radio");
            on_local_fin();
        }
        stream_.clear_give_up();
        return;
    }
    if (cfg_.mode == upstream_mode_e::tcp_client && stream_.wants_connect() &&
        client_sock_.fd() < 0 && !connecting_)
    {
        start_connect();
    }
    sync_rx_accept();
    flush_tcp();
}
