#include "app.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "endpoint/tcp_client_endpoint.h"
#include "endpoint/tcp_server_endpoint.h"
#include "endpoint/udp_endpoint.h"
#include "log.h"
#include "net_util.h"
#include "radio/mpdu.h"

bool app::load(const std::string& path)
{
    std::string err;
    if (!cfg.load(path, &err))
    {
        LOG_ERR("%s", err.c_str());
        return false;
    }
    if (!parse_host(cfg.device, &device_ip))
    {
        LOG_ERR("cannot resolve winject.device %s", cfg.device.c_str());
        return false;
    }
    if (!cfg.local_ip.empty() && !parse_host(cfg.local_ip, &local_ip))
    {
        LOG_ERR("invalid winject.local_ip");
        return false;
    }
    return true;
}

bool app::add_upstream(const upstream_config_s& uc)
{
    if (!radio)
    {
        return false;
    }
    std::unique_ptr<stream> up;
    if (uc.mode == upstream_mode_e::tcp_client ||
        uc.mode == upstream_mode_e::tcp_server)
    {
        std::unique_ptr<tcp_endpoint> tcp;
        if (uc.mode == upstream_mode_e::tcp_server)
        {
            tcp = std::make_unique<tcp_server_endpoint>();
        }
        else
        {
            tcp = std::make_unique<tcp_client_endpoint>();
        }
        if (!tcp->open(reactor, uc))
        {
            return false;
        }
        tcp->set_tx_kick(
            [this]()
            {
                scheduler.tick();
            });
        up = std::move(tcp);
    }
    else
    {
        auto udp = std::make_unique<udp_endpoint>();
        if (!udp->open(reactor, uc))
        {
            return false;
        }
        up = std::move(udp);
    }
    scheduler.add(up.get(), radio.get(), uc.bus_tx, uc.bus_rx,
                  uc.scheduler_budget);
    upstreams.push_back(std::move(up));
    LOG_INF("upstream-%zu bus_tx=%s bus_rx=%s", uc.index,
            uc.bus_tx ? bus_to_string(uc.bus_tx).c_str() : "-",
            uc.bus_rx ? bus_to_string(uc.bus_rx).c_str() : "-");
    return true;
}

bool app::setup_radio()
{
    WinjectMode mode = WINJECT_MODE_STANDALONE;
    if (cfg.radio_mode == radio_mode_e::bfc_tunnel_device)
    {
        mode = WINJECT_MODE_BFC_TUNNEL_DEVICE;
    }
    if (!mpdu_init_rules(mode))
    {
        LOG_ERR("mpdu frame rules init failed");
        return false;
    }
    radio = std::make_unique<wifi_udp>();
    sockaddr_in inject = {};
    inject.sin_family = AF_INET;
    inject.sin_addr = device_ip;
    inject.sin_port = htons(cfg.inject_port);
    if (!radio->open(
            reactor, inject, cfg.forward_port,
            [this](const uint8_t* data, size_t len)
            {
                scheduler.on_mpdu_rx(data, len);
            },
            [this]()
            {
                scheduler.tick();
            }))
    {
        return false;
    }
    LOG_INF("radio inject=%u forward=%u", radio->inject_port(),
            radio->forward_port());
    return true;
}

bool app::setup_upstreams()
{
    scheduler.configure(cfg.max_rate_kbps, cfg.domain, cfg.max_data_per_tick);
    scheduler.set_may_emit_data(
        [this]()
        {
            if (!cfg.ci_pace_inject)
            {
                return true;
            }
            if (tx_grant_credits_ > 0)
            {
                return true;
            }
            maybe_send_grant_request();
            return false;
        });
    scheduler.set_on_data_mpdu_sent(
        [this]()
        {
            if (!cfg.ci_pace_inject || tx_grant_credits_ == 0)
            {
                return;
            }
            tx_grant_credits_--;
            if (tx_grant_credits_ < k_grant_low_water)
            {
                maybe_send_grant_request();
            }
        });
    if (!setup_radio())
    {
        return false;
    }
    for (const auto& uc : cfg.upstreams)
    {
        if (!add_upstream(uc))
        {
            return false;
        }
    }
    return true;
}

bool app::set_upstream_fec(size_t index, fec_type_e type, int k, int n,
                           std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    if (index >= upstreams.size() || !upstreams[index])
    {
        return fail("invalid upstream index");
    }
    auto* udp = dynamic_cast<udp_endpoint*>(upstreams[index].get());
    if (udp == nullptr)
    {
        return fail("fec only valid for UDP upstreams");
    }
    if (!udp->set_fec(type, k, n, error))
    {
        return false;
    }
    if (index < cfg.upstreams.size())
    {
        cfg.upstreams[index].fec_type = type;
        cfg.upstreams[index].fec_k = type == fec_type_e::none ? 0 : k;
        cfg.upstreams[index].fec_n = type == fec_type_e::none ? 0 : n;
    }
    return true;
}

bool app::get_upstream_fec(size_t index, fec_type_e* type, int* k, int* n,
                           std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    if (index >= upstreams.size() || !upstreams[index])
    {
        return fail("invalid upstream index");
    }
    auto* udp = dynamic_cast<udp_endpoint*>(upstreams[index].get());
    if (udp == nullptr)
    {
        return fail("fec only valid for UDP upstreams");
    }
    udp->get_fec(type, k, n);
    return true;
}

bool app::set_upstream_scheduler_budget(size_t index, size_t budget,
                                        std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    if (index >= upstreams.size() || budget == 0)
    {
        return fail("invalid index or budget");
    }
    if (!scheduler.set_budget(index, budget))
    {
        return fail("failed to set budget");
    }
    if (index < cfg.upstreams.size())
    {
        cfg.upstreams[index].scheduler_budget = budget;
    }
    LOG_INF("upstream-%zu scheduler_budget=%zu", index, budget);
    return true;
}

bool app::get_upstream_scheduler_budget(size_t index, size_t* budget,
                                        std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    if (!scheduler.get_budget(index, budget))
    {
        return fail("invalid upstream index");
    }
    return true;
}

bool app::set_modulation(const std::string& name, std::string* error)
{
    auto fail = [&](const char* msg) -> bool
    {
        if (error != nullptr)
        {
            *error = msg;
        }
        return false;
    };
    const std::string canonical = config::canonical_modulation(name);
    if (canonical.empty())
    {
        return fail("unknown modulation");
    }
    if (!config::modulation_ok_for_channel(canonical, cfg.channel))
    {
        return fail("channel 14 requires DSSS/CCK modulation");
    }
    if (!console_ok)
    {
        return fail("console not connected");
    }
    std::string err;
    if (!console.set_modulation(canonical, &err))
    {
        if (console.take_pong())
        {
            awaiting_pong = false;
        }
        if (err.find(" -> error") == std::string::npos)
        {
            drop_console();
        }
        return fail(err.empty() ? "failed" : err.c_str());
    }
    if (console.take_pong())
    {
        awaiting_pong = false;
    }
    cfg.modulation = canonical;
    LOG_INF("modulation=%s", canonical.c_str());
    return true;
}

bool app::get_modulation(std::string* name, std::string* error)
{
    if (name == nullptr)
    {
        if (error != nullptr)
        {
            *error = "null out";
        }
        return false;
    }
    *name = cfg.modulation;
    return true;
}

void app::fill_ci_view(channel_info_view_s* out) const
{
    if (out == nullptr)
    {
        return;
    }
    const auto flow = ci.last_flow_ctrl();
    const auto air = ci.last_rx_air();
    out->flow_valid = flow.valid;
    out->tx_queue_size = flow.tx_queue_size;
    out->tx_queue_capacity = flow.tx_queue_capacity;
    channel_info::format_stamp(flow.valid, flow.received, out->flow_t,
                               sizeof(out->flow_t));
    out->air_valid = air.valid;
    out->rssi = air.rssi;
    out->snr = air.snr;
    channel_info::format_stamp(air.valid, air.received, out->rssi_t,
                               sizeof(out->rssi_t));
    out->streams.clear();
    out->tx_byte = 0;
    out->rx_byte = 0;
    out->tx_pkt = 0;
    out->rx_pkt = 0;
    out->rx_pkt_loss = 0;
    out->streams.reserve(upstreams.size());
    for (size_t i = 0; i < upstreams.size(); i++)
    {
        if (!upstreams[i])
        {
            continue;
        }
        const stream_stats_s st = upstreams[i]->peek_stats();
        if (st.proto == nullptr)
        {
            continue;
        }
        stream_rate_view_s row;
        row.index = i;
        row.type = st.proto;
        row.tcp = st.tcp;
        row.queue = st.queue;
        row.unacked = st.unacked;
        row.fec_recovered = st.fec_recovered;
        row.fec_fail = st.fec_fail;
        format_fec(row.fec, sizeof(row.fec), st.fec_k, st.fec_n);
        row.tx_byte = st.tx_bytes_life;
        row.rx_byte = st.rx_bytes_life;
        row.tx_pkt = st.tx_pkt_life;
        row.rx_pkt = st.rx_pkt_life;
        row.air_tx_pkt = st.air_tx_pkt_life;
        row.drop_txq = st.drop_txq;
        uint64_t seq_lost = 0;
        if (scheduler.peek_seq_lost(i, &seq_lost))
        {
            row.rx_pkt_loss = seq_lost;
            out->rx_pkt_loss += seq_lost;
        }
        out->streams.push_back(row);
    }
    if (radio)
    {
        const auto c = radio->peek_counters();
        out->tx_pkt = c.tx_pkt;
        out->rx_pkt = c.rx_pkt;
        out->tx_byte = c.tx_byte;
        out->rx_byte = c.rx_byte;
    }
}

bool app::start_manager_console()
{
    if (cfg.manager_console_in.empty())
    {
        return true;
    }
    sockaddr_in in_addr = {};
    sockaddr_in out_addr = {};
    if (!parse_host_port(cfg.manager_console_in, &in_addr) ||
        !parse_host_port(cfg.manager_console_out, &out_addr))
    {
        LOG_ERR("manager console: invalid console_in/out");
        return false;
    }
    std::string err;
    if (!mgr_console.start(
            reactor, in_addr, out_addr,
            [this](size_t index, fec_type_e type, int k, int n,
                   std::string* error)
            {
                return set_upstream_fec(index, type, k, n, error);
            },
            [this](size_t index, size_t budget, std::string* error)
            {
                return set_upstream_scheduler_budget(index, budget, error);
            },
            [this](size_t index, fec_type_e* type, int* k, int* n,
                   std::string* error)
            {
                return get_upstream_fec(index, type, k, n, error);
            },
            [this](size_t index, size_t* budget, std::string* error)
            {
                return get_upstream_scheduler_budget(index, budget, error);
            },
            [this](channel_info_view_s* out, std::string* error)
            {
                if (out == nullptr)
                {
                    if (error != nullptr)
                    {
                        *error = "null out";
                    }
                    return false;
                }
                fill_ci_view(out);
                return true;
            },
            [this](const std::string& name, std::string* error)
            {
                return set_modulation(name, error);
            },
            [this](std::string* name, std::string* error)
            {
                return get_modulation(name, error);
            },
            &err))
    {
        LOG_ERR("manager console: %s", err.c_str());
        return false;
    }
    return true;
}

bool app::apply_console()
{
    std::string err;
    if (cfg.local_ip.empty())
    {
        local_ip = console.local_ip();
    }
    if (!console.apply_radio(cfg, &err))
    {
        LOG_ERR("%s", err.c_str());
        return false;
    }
    if (!radio)
    {
        LOG_ERR("radio not open");
        return false;
    }
    if (!console.apply_upstream(cfg, radio->inject_port(), radio->forward_port(),
                                local_ip, &err))
    {
        LOG_ERR("%s", err.c_str());
        return false;
    }
    if (!console.apply_ci(local_ip, ci.port(), &err))
    {
        LOG_ERR("%s", err.c_str());
        return false;
    }
    return hold_console();
}

bool app::hold_console()
{
    console.clear_pending();
    awaiting_pong = false;
    heartbeat_ticks = 0;
    if (!set_nonblock(console.fd()))
    {
        LOG_ERR("console fcntl failed");
        return false;
    }
    console_ok = true;
    if (!reactor.add_read_rdy(console.fd(),
                              [this]()
                              {
                                  on_console();
                              }))
    {
        LOG_ERR("console epoll add failed");
        return false;
    }
    return true;
}

void app::drop_console()
{
    const int fd = console.fd();
    if (fd >= 0)
    {
        reactor.rem_read_rdy(fd);
        reactor.rem_write_rdy(fd);
    }
    console.close();
    console_ok = false;
    awaiting_pong = false;
    heartbeat_ticks = 0;
}

void app::begin_console()
{
    if (console_ok)
    {
        return;
    }
    std::string err;
    if (!console.start_connect(cfg, &err) || !console.finish_connect(&err))
    {
        LOG_ERR("console: %s", err.c_str());
        drop_console();
        return;
    }
    reconnect_ticks = 0;
    LOG_INF("console ready %s:%u local %s", cfg.device.c_str(),
            cfg.console_port, ipv4_to_string(console.local_ip()).c_str());
    if (!apply_console())
    {
        drop_console();
        return;
    }
    LOG_INF("console upstreams reapplied");
}

void app::on_console()
{
    if (!console_ok)
    {
        return;
    }
    while (true)
    {
        char buf[2048];
        const ssize_t n = recv(console.fd(), buf, sizeof(buf), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            break;
        }
        if (n < 0)
        {
            LOG_WRN("console recv: %s", strerror(errno));
            drop_console();
            return;
        }
        if (n == 0)
        {
            continue;
        }
        console.append_recv(buf, static_cast<size_t>(n));
    }

    std::string line;
    while (console.pop_line(&line))
    {
        if (line == "pong")
        {
            awaiting_pong = false;
            heartbeat_ticks = 0;
            continue;
        }
        // Ignore unsolicited lines while holding the console.
    }
}

void app::heartbeat_tick()
{
    if (!console_ok)
    {
        return;
    }
    heartbeat_ticks++;
    if (awaiting_pong)
    {
        if (heartbeat_ticks >= k_pong_timeout_ticks)
        {
            LOG_WRN("console ping timeout");
            drop_console();
        }
        return;
    }
    if (heartbeat_ticks < k_ping_interval_ticks)
    {
        return;
    }
    heartbeat_ticks = 0;
    std::string err;
    if (!console.send_ping(&err))
    {
        LOG_WRN("console ping failed: %s", err.c_str());
        drop_console();
        return;
    }
    awaiting_pong = true;
}

void app::reconnect_tick()
{
    if (cfg.ci_pace_inject && grant_outstanding_ > 0)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - grant_sent_at_)
                            .count();
        if (ms >= 200)
        {
            grant_outstanding_ = 0;
            maybe_send_grant_request();
        }
    }
    if (cfg.skip_console)
    {
        return;
    }
    if (console_ok)
    {
        reconnect_ticks = 0;
        heartbeat_tick();
        return;
    }
    reconnect_ticks++;
    if (reconnect_ticks < k_reconnect_ticks)
    {
        return;
    }
    reconnect_ticks = 0;
    LOG_INF("retrying console");
    begin_console();
}

void app::arm_tick()
{
    // 250 us tick: drain host UDP txq faster under OFDM_24M line-rate offers.
    reactor.get_timer().wait_us(250,
                                [this]()
                                {
                                    reconnect_tick();
                                    stats_tick();
                                    scheduler.tick();
                                    arm_tick();
                                });
}

void app::stats_tick()
{
    if (cfg.stats_sec == 0)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_stats.time_since_epoch().count() == 0)
    {
        last_stats = now;
        return;
    }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats)
            .count();
    if (elapsed_ms < static_cast<long>(cfg.stats_sec) * 1000)
    {
        return;
    }
    last_stats = now;
    std::vector<stream*> ups;
    ups.reserve(upstreams.size());
    for (const auto& up : upstreams)
    {
        ups.push_back(up.get());
    }
    scheduler.log_stats(elapsed_ms / 1000.0, ups);
    const auto flow = ci.last_flow_ctrl();
    const auto air = ci.last_rx_air();
    if (flow.valid || air.valid)
    {
        char flow_t[32];
        char rssi_t[32];
        channel_info::format_stamp(flow.valid, flow.received, flow_t,
                                   sizeof(flow_t));
        channel_info::format_stamp(air.valid, air.received, rssi_t,
                                   sizeof(rssi_t));
        if (flow.valid && air.valid)
        {
            LOG_INF("RADIO CI flow=%u/%u flow_t=%s rssi=%d snr=%d rssi_t=%s",
                    flow.tx_queue_size, flow.tx_queue_capacity, flow_t,
                    static_cast<int>(air.rssi), static_cast<int>(air.snr),
                    rssi_t);
        }
        else if (flow.valid)
        {
            LOG_INF("RADIO CI flow=%u/%u flow_t=%s rssi=- snr=- rssi_t=-",
                    flow.tx_queue_size, flow.tx_queue_capacity, flow_t);
        }
        else
        {
            LOG_INF("RADIO CI flow=-/- flow_t=- rssi=%d snr=%d rssi_t=%s",
                    static_cast<int>(air.rssi), static_cast<int>(air.snr),
                    rssi_t);
        }
    }
}

void app::stop()
{
    reactor.stop();
}

void app::flush_shutdown()
{
    for (auto& up : upstreams)
    {
        if (up)
        {
            up->announce_down();
        }
    }
    // CLOSE is ctrl (not rate-limited). One pass injects the repeats.
    scheduler.tick();
}

void app::maybe_send_grant_request()
{
    if (!cfg.ci_pace_inject || console.grant_fd() < 0)
    {
        return;
    }
    if (tx_grant_credits_ >= k_grant_low_water &&
        grant_outstanding_ >= k_max_outstanding_grants)
    {
        return;
    }
    while (grant_outstanding_ < k_max_outstanding_grants &&
           tx_grant_credits_ < k_grant_low_water)
    {
        std::string err;
        if (!console.send_tx_grant_request(&err))
        {
            break;
        }
        grant_outstanding_++;
        grant_sent_at_ = std::chrono::steady_clock::now();
    }
}

void app::on_grant_io()
{
    uint8_t n = 0;
    bool got = false;
    while (console.try_consume_tx_grant(&n))
    {
        got = true;
        const uint16_t sum =
            static_cast<uint16_t>(tx_grant_credits_) + static_cast<uint16_t>(n);
        tx_grant_credits_ =
            sum > 255u ? static_cast<uint8_t>(255) : static_cast<uint8_t>(sum);
        if (grant_outstanding_ > 0)
        {
            grant_outstanding_--;
        }
    }
    if (got)
    {
        maybe_send_grant_request();
    }
}

void app::arm_grant_io()
{
    if (!cfg.ci_pace_inject || grant_io_armed_)
    {
        return;
    }
    const int fd = console.grant_fd();
    if (fd < 0)
    {
        return;
    }
    if (!set_nonblock(fd))
    {
        LOG_WRN("grant channel nonblock failed");
        return;
    }
    if (!reactor.add_read_rdy(fd,
                              [this]()
                              {
                                  on_grant_io();
                              }))
    {
        LOG_WRN("grant channel epoll add failed");
        return;
    }
    grant_io_armed_ = true;
}

int app::run()
{
    if (!setup_upstreams())
    {
        return 1;
    }
    if (!ci.open(reactor))
    {
        return 1;
    }
    if (!start_manager_console())
    {
        return 1;
    }
    std::vector<uint16_t> inject_ports;
    std::vector<uint16_t> forward_ports;
    if (radio)
    {
        inject_ports.push_back(radio->inject_port());
        forward_ports.push_back(radio->forward_port());
    }
    if (!cfg.local_ip.empty() && parse_host(cfg.local_ip, &local_ip))
    {
        // keep configured local_ip for set_upstream_rx
    }
    std::string err;
    if (!cfg.skip_console)
    {
        in_addr console_local = {};
        bool held = false;
        if (!console.program(cfg, inject_ports, forward_ports, ci.port(),
                             &console_local, &err))
        {
            LOG_ERR("%s", err.c_str());
        }
        else
        {
            if (cfg.local_ip.empty())
            {
                local_ip = console_local;
            }
            held = hold_console();
            if (!held)
            {
                drop_console();
            }
        }
        if (held)
        {
            LOG_INF("console ready %s:%u", cfg.device.c_str(), cfg.console_port);
        }
        else
        {
            LOG_WRN("waiting for console %s:%u", cfg.device.c_str(),
                    cfg.console_port);
            reconnect_ticks = k_reconnect_ticks;
        }
    }
    else if (!cfg.local_ip.empty())
    {
        parse_host(cfg.local_ip, &local_ip);
    }
    else
    {
        LOG_ERR("winject.skip_console requires winject.local_ip");
        return 1;
    }
    if (cfg.ci_pace_inject && console.grant_fd() < 0)
    {
        if (!console.connect_grant_peer(cfg, &err))
        {
            LOG_WRN("tx_grant channel: %s", err.c_str());
        }
        else
        {
            LOG_INF("tx_grant channel %s:%u", cfg.device.c_str(),
                    cfg.console_port);
        }
    }
    arm_grant_io();
    maybe_send_grant_request();
    LOG_INF("manager running local %s forward_base %u max_rate %u kbps (%s)",
            ipv4_to_string(local_ip).c_str(), cfg.forward_base,
            cfg.max_rate_kbps, cfg.modulation.c_str());
    last_stats = std::chrono::steady_clock::now();
    if (cfg.stats_sec > 0)
    {
        LOG_INF(
            "stats logging every %u s (WINJECT_STATS_SEC or winject.stats_sec)",
            cfg.stats_sec);
    }
    arm_tick();
    reactor.run();
    flush_shutdown();
    return 0;
}
