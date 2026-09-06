#include "endpoint/udp_endpoint.h"

#include <errno.h>
#include <string.h>

#include "log.h"

namespace
{
constexpr size_t k_max_udp_queue = 256;
}

udp_endpoint::~udp_endpoint()
{
    close();
}

bool udp_endpoint::open(::reactor& reactor, const upstream_config_s& cfg)
{
    this->reactor = &reactor;
    mode = cfg.mode;
    sock = make_udp4();
    if (sock.fd() < 0)
    {
        LOG_ERR("udp socket: %s", strerror(errno));
        return false;
    }

    if (mode == upstream_mode_e::udp_generic)
    {
        sockaddr_in bind_addr = {};
        if (!parse_host_port(cfg.rx, &bind_addr) || sock.bind(bind_addr) < 0)
        {
            LOG_ERR("udp bind %s: %s", cfg.rx.c_str(), strerror(errno));
            return false;
        }
        dest_valid = parse_host_port(cfg.tx, &dest);
        if (!dest_valid)
        {
            LOG_ERR("udp tx %s invalid", cfg.tx.c_str());
            return false;
        }
    }
    else if (mode == upstream_mode_e::udp_server)
    {
        sockaddr_in bind_addr = {};
        if (!parse_host_port(cfg.bind_address, &bind_addr) ||
            sock.bind(bind_addr) < 0)
        {
            LOG_ERR("udp bind %s: %s", cfg.bind_address.c_str(),
                    strerror(errno));
            return false;
        }
    }
    else if (mode == upstream_mode_e::udp_client)
    {
        if (!parse_host_port(cfg.connect_address, &dest))
        {
            LOG_ERR("udp connect_address %s invalid",
                    cfg.connect_address.c_str());
            return false;
        }
        dest_valid = true;
        uint16_t local_port = 0;
        if (!bind_udp_any(sock.fd(), &local_port))
        {
            LOG_ERR("udp client bind: %s", strerror(errno));
            return false;
        }
    }

    if (cfg.fec_type == fec_type_e::rs_block_erasure)
    {
        if (!fec.init(cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms))
        {
            LOG_ERR("udp fec init k=%d n=%d failed", cfg.fec_k, cfg.fec_n);
            return false;
        }
        LOG_INF("udp fec RS_BLOCK_ERASURE k=%d n=%d timeout=%d ms (%s)",
                cfg.fec_k, cfg.fec_n, cfg.fec_timeout_ms, fec.impl_name());
    }

    return reactor.add_read_rdy(sock.fd(),
                                [this]()
                                {
                                    on_app();
                                });
}

void udp_endpoint::close()
{
    if (sock.fd() >= 0)
    {
        if (reactor != nullptr)
        {
            reactor->rem_read_rdy(sock.fd());
        }
        close_socket(&sock);
    }
}

void udp_endpoint::enqueue_air(std::vector<uint8_t> pkt)
{
    if (pkt.empty())
    {
        return;
    }
    if (txq.size() >= k_max_udp_queue)
    {
        txq.pop_front();
    }
    txq.push_back(std::move(pkt));
}

void udp_endpoint::on_app()
{
    while (true)
    {
        sockaddr_in from = {};
        const ssize_t n = udp_recv_from(sock.fd(), buf, sizeof(buf), &from);
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
        if (mode == upstream_mode_e::udp_server)
        {
            dest = from;
            dest_valid = true;
        }
        app_rx_pkt_interval++;
        app_rx_bytes_interval += static_cast<uint64_t>(n);
        if (fec.enabled())
        {
            std::vector<std::vector<uint8_t>> encoded;
            fec.push_app(buf, static_cast<size_t>(n), &encoded);
            for (auto& pkt : encoded)
            {
                enqueue_air(std::move(pkt));
            }
            continue;
        }
        if (static_cast<size_t>(n) > k_wifi_payload_max)
        {
            LOG_WRN("drop oversized udp %zd", n);
            continue;
        }
        enqueue_air(std::vector<uint8_t>(buf, buf + n));
    }
}

void udp_endpoint::on_radio_rx(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
    {
        return;
    }
    air_rx_bytes_interval += len;
    if (sock.fd() < 0 || !dest_valid)
    {
        return;
    }
    if (fec.enabled())
    {
        std::vector<std::vector<uint8_t>> payloads;
        fec.push_air(data, len, &payloads);
        for (const auto& p : payloads)
        {
            radio_rx_pkt_interval++;
            radio_rx_bytes_interval += p.size();
            udp_send_to(sock.fd(), dest, p.data(), p.size());
        }
        return;
    }
    radio_rx_pkt_interval++;
    radio_rx_bytes_interval += len;
    udp_send_to(sock.fd(), dest, data, len);
}

void udp_endpoint::on_tick()
{
    if (!fec.enabled())
    {
        return;
    }
    std::vector<std::vector<uint8_t>> encoded;
    fec.on_tick(&encoded);
    for (auto& pkt : encoded)
    {
        enqueue_air(std::move(pkt));
    }
}

void udp_endpoint::announce_down()
{
    if (!fec.enabled())
    {
        return;
    }
    std::vector<std::vector<uint8_t>> encoded;
    fec.flush(&encoded);
    for (auto& pkt : encoded)
    {
        enqueue_air(std::move(pkt));
    }
}

uint64_t udp_endpoint::take_rx_bytes()
{
    const uint64_t n = radio_rx_bytes_interval;
    radio_rx_bytes_interval = 0;
    return n;
}

stream_stats_s udp_endpoint::take_stats()
{
    stream_stats_s s;
    s.proto = fec.enabled() ? "UDP+RS" : "UDP";
    s.tx_bytes = app_rx_bytes_interval;
    s.rx_bytes = take_rx_bytes();
    s.air_tx_bytes = air_tx_bytes_interval;
    s.air_rx_bytes = air_rx_bytes_interval;
    if (fec.enabled())
    {
        s.fec_recovered = fec.take_recovered();
        s.fec_fail = fec.take_decode_fail();
    }
    air_tx_bytes_interval = 0;
    air_rx_bytes_interval = 0;
    app_rx_pkt_interval = 0;
    app_rx_bytes_interval = 0;
    radio_rx_pkt_interval = 0;
    return s;
}

bool udp_endpoint::has_tx() const
{
    return !txq.empty();
}

size_t udp_endpoint::pull_tx(uint8_t* out, size_t max, bool* is_ack)
{
    if (is_ack != nullptr)
    {
        *is_ack = false;
    }
    if (txq.empty() || out == nullptr || max == 0)
    {
        return 0;
    }
    auto& pkt = txq.front();
    if (pkt.size() > max)
    {
        return 0;
    }
    memcpy(out, pkt.data(), pkt.size());
    const size_t n = pkt.size();
    txq.pop_front();
    air_tx_bytes_interval += n;
    return n;
}
