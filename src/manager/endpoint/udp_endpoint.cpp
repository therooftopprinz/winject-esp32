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
        fec_timeout_ms = cfg.fec_timeout_ms;
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
            if (errno == ECONNREFUSED)
            {
                // Reply dest is a closed local port; ICMP is queued on this
                // socket. Drop it and accept the next sender (nc -w1, or a
                // new interactive nc after the previous one quit).
                dest_valid = false;
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
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
            // Last sender gets replies. Camera/master also seed with "ok\n"
            // at connect; that is just another datagram. A stray nc to the
            // camera bind steals until camera sends again — don't nc :21092
            // while camera is running.
            dest = from;
            dest_valid = true;
        }
        app_rx_pkt_interval++;
        app_rx_bytes_interval += static_cast<uint64_t>(n);
        app_rx_bytes_life += static_cast<uint64_t>(n);
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
        if (static_cast<size_t>(n) > k_stream_payload_max)
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
    // Always FEC-aware on RX: shard header carries k/n. Non-FEC passes through.
    std::vector<std::vector<uint8_t>> payloads;
    fec.push_air(data, len, &payloads);
    for (const auto& p : payloads)
    {
        radio_rx_pkt_interval++;
        radio_rx_bytes_interval += p.size();
        radio_rx_bytes_life += p.size();
        udp_send_to(sock.fd(), dest, p.data(), p.size());
    }
}

void udp_endpoint::on_tick()
{
    // Always run: expires incomplete RX FEC blocks; encode flush only if init'd.
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

bool udp_endpoint::set_fec(fec_type_e type, int k, int n, std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    // Flush any partial TX block before changing encode params.
    announce_down();
    if (type == fec_type_e::none)
    {
        fec.disable();
        LOG_INF("udp fec disabled");
        return true;
    }
    if (type != fec_type_e::rs_block_erasure)
    {
        return fail("unsupported fec type");
    }
    const int timeout_ms =
        fec_timeout_ms > 0 ? fec_timeout_ms
                           : rs_block_erasure::k_default_timeout_ms;
    if (!fec.init(k, n, timeout_ms))
    {
        return fail("invalid fec k/n (need 1 <= k < n <= 255)");
    }
    LOG_INF("udp fec RS_BLOCK_ERASURE k=%d n=%d timeout=%d ms (%s)", k, n,
            timeout_ms, fec.impl_name());
    return true;
}

void udp_endpoint::get_fec(fec_type_e* type, int* k, int* n) const
{
    if (type != nullptr)
    {
        *type = fec.enabled() ? fec_type_e::rs_block_erasure : fec_type_e::none;
    }
    if (k != nullptr)
    {
        *k = fec.enabled() ? fec.k() : 0;
    }
    if (n != nullptr)
    {
        *n = fec.enabled() ? fec.n() : 0;
    }
}

uint64_t udp_endpoint::take_rx_bytes()
{
    const uint64_t n = radio_rx_bytes_interval;
    radio_rx_bytes_interval = 0;
    return n;
}

stream_stats_s udp_endpoint::peek_stats() const
{
    stream_stats_s s;
    s.proto = "UDP";
    s.tx_bytes = app_rx_bytes_interval;
    s.rx_bytes = radio_rx_bytes_interval;
    s.tx_bytes_life = app_rx_bytes_life;
    s.rx_bytes_life = radio_rx_bytes_life;
    s.air_tx_bytes = air_tx_bytes_interval;
    s.air_rx_bytes = air_rx_bytes_interval;
    s.fec_k = fec.enabled() ? fec.k() : 0;
    s.fec_n = fec.enabled() ? fec.n() : 0;
    s.fec_recovered = fec.recovered();
    s.fec_fail = fec.decode_fail();
    return s;
}

stream_stats_s udp_endpoint::take_stats()
{
    stream_stats_s s = peek_stats();
    take_rx_bytes();
    s.fec_recovered = fec.take_recovered();
    s.fec_fail = fec.take_decode_fail();
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
