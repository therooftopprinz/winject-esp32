#include "udp_endpoint.h"

#include <errno.h>
#include <string.h>

#include "log.h"

namespace
{
constexpr size_t kMaxUdpQueue = 256;
}

UdpEndpoint::~UdpEndpoint()
{
    close();
}

bool UdpEndpoint::open(Reactor& reactor, const UpstreamConfig& cfg)
{
    reactor_ = &reactor;
    mode_ = cfg.mode;
    sock_ = make_udp4();
    if (sock_.fd() < 0)
    {
        LOG_ERR("udp socket: %s", strerror(errno));
        return false;
    }

    if (mode_ == UpstreamMode::UdpGeneric)
    {
        sockaddr_in bind_addr = {};
        if (!parse_host_port(cfg.rx, &bind_addr) || sock_.bind(bind_addr) < 0)
        {
            LOG_ERR("udp bind %s: %s", cfg.rx.c_str(), strerror(errno));
            return false;
        }
        dest_valid_ = parse_host_port(cfg.tx, &dest_);
        if (!dest_valid_)
        {
            LOG_ERR("udp tx %s invalid", cfg.tx.c_str());
            return false;
        }
    }
    else if (mode_ == UpstreamMode::UdpServer)
    {
        sockaddr_in bind_addr = {};
        if (!parse_host_port(cfg.bind_address, &bind_addr) ||
            sock_.bind(bind_addr) < 0)
        {
            LOG_ERR("udp bind %s: %s", cfg.bind_address.c_str(),
                    strerror(errno));
            return false;
        }
    }
    else if (mode_ == UpstreamMode::UdpClient)
    {
        if (!parse_host_port(cfg.connect_address, &dest_))
        {
            LOG_ERR("udp connect_address %s invalid",
                    cfg.connect_address.c_str());
            return false;
        }
        dest_valid_ = true;
        uint16_t local_port = 0;
        if (!bind_udp_any(sock_.fd(), &local_port))
        {
            LOG_ERR("udp client bind: %s", strerror(errno));
            return false;
        }
    }

    if (cfg.fec_type == FecType::RsBlockErasure)
    {
        if (!fec_.init(cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms))
        {
            LOG_ERR("udp fec init k=%d n=%d failed", cfg.fec_k, cfg.fec_n);
            return false;
        }
        LOG_INF("udp fec RS_BLOCK_ERASURE k=%d n=%d timeout=%d ms (%s)",
                cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms, fec_.impl_name());
    }

    return reactor.add_read_rdy(sock_.fd(),
                                [this]()
                                {
                                    on_app();
                                });
}

void UdpEndpoint::close()
{
    if (sock_.fd() >= 0)
    {
        if (reactor_ != nullptr)
        {
            reactor_->rem_read_rdy(sock_.fd());
        }
        close_socket(&sock_);
    }
}

void UdpEndpoint::enqueue_air(std::vector<uint8_t> pkt)
{
    if (pkt.empty())
    {
        return;
    }
    if (txq_.size() >= kMaxUdpQueue)
    {
        txq_.pop_front();
    }
    txq_.push_back(std::move(pkt));
}

void UdpEndpoint::on_app()
{
    while (true)
    {
        sockaddr_in from = {};
        const ssize_t n = udp_recv_from(sock_.fd(), buf_, sizeof(buf_), &from);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK ||
                errno == ECONNREFUSED)
            {
                return;
            }
            LOG_ERR("udp app recv: %s", strerror(errno));
            return;
        }
        if (n == 0)
        {
            return;
        }
        if (mode_ == UpstreamMode::UdpServer)
        {
            dest_ = from;
            dest_valid_ = true;
        }
        app_rx_pkt_interval_++;
        app_rx_bytes_interval_ += static_cast<uint64_t>(n);
        if (fec_.enabled())
        {
            std::vector<std::vector<uint8_t>> encoded;
            fec_.push_app(buf_, static_cast<size_t>(n), &encoded);
            for (auto& pkt : encoded)
            {
                enqueue_air(std::move(pkt));
            }
            continue;
        }
        if (static_cast<size_t>(n) > kWifiPayloadMax)
        {
            LOG_WRN("drop oversized udp %zd", n);
            continue;
        }
        enqueue_air(std::vector<uint8_t>(buf_, buf_ + n));
    }
}

void UdpEndpoint::on_radio_rx(const uint8_t* data, size_t len)
{
    if (sock_.fd() < 0 || data == nullptr || len == 0 || !dest_valid_)
    {
        return;
    }
    if (fec_.enabled())
    {
        std::vector<std::vector<uint8_t>> payloads;
        fec_.push_air(data, len, &payloads);
        for (const auto& p : payloads)
        {
            radio_rx_pkt_interval_++;
            radio_rx_bytes_interval_ += p.size();
            udp_send_to(sock_.fd(), dest_, p.data(), p.size());
        }
        return;
    }
    radio_rx_pkt_interval_++;
    radio_rx_bytes_interval_ += len;
    udp_send_to(sock_.fd(), dest_, data, len);
}

void UdpEndpoint::on_tick()
{
    if (!fec_.enabled())
    {
        return;
    }
    std::vector<std::vector<uint8_t>> encoded;
    fec_.on_tick(&encoded);
    for (auto& pkt : encoded)
    {
        enqueue_air(std::move(pkt));
    }
}

void UdpEndpoint::announce_down()
{
    if (!fec_.enabled())
    {
        return;
    }
    std::vector<std::vector<uint8_t>> encoded;
    fec_.flush(&encoded);
    for (auto& pkt : encoded)
    {
        enqueue_air(std::move(pkt));
    }
}

uint64_t UdpEndpoint::take_rx_bytes()
{
    const uint64_t n = radio_rx_bytes_interval_;
    radio_rx_bytes_interval_ = 0;
    return n;
}

StreamStats UdpEndpoint::take_stats()
{
    StreamStats s;
    s.proto = fec_.enabled() ? "UDP+RS" : "UDP";
    s.tx_bytes = air_tx_bytes_interval_;
    s.rx_bytes = take_rx_bytes();
    if (fec_.enabled())
    {
        s.fec_recovered = fec_.take_recovered();
        s.fec_fail = fec_.take_decode_fail();
    }
    air_tx_bytes_interval_ = 0;
    app_rx_pkt_interval_ = 0;
    app_rx_bytes_interval_ = 0;
    radio_rx_pkt_interval_ = 0;
    return s;
}

bool UdpEndpoint::has_tx() const
{
    return !txq_.empty();
}

size_t UdpEndpoint::pull_tx(uint8_t* out, size_t max, bool* is_ack)
{
    if (is_ack != nullptr)
    {
        *is_ack = false;
    }
    if (txq_.empty() || out == nullptr || max == 0)
    {
        return 0;
    }
    auto& pkt = txq_.front();
    if (pkt.size() > max)
    {
        return 0;
    }
    memcpy(out, pkt.data(), pkt.size());
    const size_t n = pkt.size();
    txq_.pop_front();
    air_tx_bytes_interval_ += n;
    return n;
}
