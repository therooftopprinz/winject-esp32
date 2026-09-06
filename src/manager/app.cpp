#include "app.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

#include "endpoint/tcp_client_endpoint.h"
#include "endpoint/tcp_server_endpoint.h"
#include "endpoint/udp_endpoint.h"
#include "log.h"
#include "net_util.h"

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

bool app::setup_upstreams()
{
    scheduler.configure(cfg.max_rate_kbps);
    for (size_t i = 0; i < cfg.upstreams.size(); i++)
    {
        const auto& uc = cfg.upstreams[i];
        auto radio = std::make_unique<wifi_udp>();
        sockaddr_in inject = {};
        inject.sin_family = AF_INET;
        inject.sin_addr = device_ip;
        inject.sin_port = htons(static_cast<uint16_t>(9000 + i * 10));
        const uint16_t fwd = static_cast<uint16_t>(cfg.forward_base + i);
        wifi_udp* radio_ptr = radio.get();
        if (!radio->open(
                reactor, inject, fwd,
                [this, i](const uint8_t* data, size_t len)
                {
                    if (i < upstreams.size() && upstreams[i])
                    {
                        upstreams[i]->on_radio_rx(data, len);
                    }
                },
                [this]()
                {
                    scheduler.tick();
                }))
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
        scheduler.add(up.get(), radio_ptr, uc.scheduler_budget);
        radios.push_back(std::move(radio));
        upstreams.push_back(std::move(up));
        LOG_INF("upstream-%zu radio inject=%u forward=%u", i,
                radios.back()->inject_port(), radios.back()->forward_port());
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
    for (size_t i = 0; i < cfg.upstreams.size(); i++)
    {
        if (!console.apply_upstream(
                cfg, cfg.upstreams[i], radios[i]->inject_port(),
                radios[i]->forward_port(), local_ip, &err))
        {
            LOG_ERR("%s", err.c_str());
            return false;
        }
    }
    if (!set_nonblock(console.fd()))
    {
        LOG_ERR("console fcntl failed");
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
    console_connecting = false;
}

void app::begin_console()
{
    if (console_ok || console_connecting)
    {
        return;
    }
    std::string err;
    if (!console.start_connect(cfg, &err))
    {
        LOG_ERR("console: %s", err.c_str());
        return;
    }
    console_connecting = true;
    reconnect_ticks = 0;
    if (!reactor.add_write_rdy(console.fd(),
                                [this]()
                                {
                                    on_console_connecting();
                                }) ||
        !reactor.req_write(console.fd()))
    {
        drop_console();
    }
}

void app::on_console_connecting()
{
    if (!console_connecting)
    {
        return;
    }
    std::string err;
    if (!console.finish_connect(&err))
    {
        LOG_ERR("console: %s", err.c_str());
        drop_console();
        return;
    }
    LOG_INF("console connected %s:%u local %s", cfg.device.c_str(),
            cfg.console_port, ipv4_to_string(console.local_ip()).c_str());
    if (!apply_console())
    {
        drop_console();
        return;
    }
    console_connecting = false;
    console_ok = true;
    reactor.rem_write_rdy(console.fd());
    if (!reactor.add_read_rdy(console.fd(),
                               [this]()
                               {
                                   on_console();
                               }))
    {
        drop_console();
    }
}

void app::on_console()
{
    if (!console_ok)
    {
        return;
    }
    char buf[128];
    const ssize_t n = recv(console.fd(), buf, sizeof(buf), 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        return;
    }
    if (n <= 0)
    {
        LOG_WRN("console disconnected");
        drop_console();
    }
}

void app::reconnect_tick()
{
    if (cfg.skip_console)
    {
        return;
    }
    if (console_ok)
    {
        reconnect_ticks = 0;
        return;
    }
    reconnect_ticks++;
    if (console_connecting)
    {
        if (reconnect_ticks >= 4000)
        {
            LOG_ERR("console connect timeout");
            drop_console();
        }
        return;
    }
    if (reconnect_ticks < 4000)
    {
        return;
    }
    reconnect_ticks = 0;
    LOG_INF("retrying console");
    begin_console();
}

void app::arm_tick()
{
    // Match original winject DEFAULT_SLOT_INTERVAL_US (500 us).
    reactor.get_timer().wait_us(500,
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

int app::run()
{
    if (!setup_upstreams())
    {
        return 1;
    }
    std::vector<uint16_t> inject_ports;
    std::vector<uint16_t> forward_ports;
    for (const auto& radio : radios)
    {
        inject_ports.push_back(radio->inject_port());
        forward_ports.push_back(radio->forward_port());
    }
    if (!cfg.local_ip.empty() && parse_host(cfg.local_ip, &local_ip))
    {
        // keep configured local_ip for set_upstream_tx
    }
    std::string err;
    if (!cfg.skip_console)
    {
        in_addr console_local = {};
        if (!console.program(cfg, inject_ports, forward_ports, &console_local,
                              &err))
        {
            LOG_ERR("%s", err.c_str());
            return 1;
        }
        if (cfg.local_ip.empty())
        {
            local_ip = console_local;
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
    LOG_INF("manager running local %s forward_base %u max_rate %u kbps (%s)",
            ipv4_to_string(local_ip).c_str(), cfg.forward_base,
            cfg.max_rate_kbps, cfg.modulation.c_str());
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
