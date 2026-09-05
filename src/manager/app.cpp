#include "app.h"

#include "log.h"
#include "net_util.h"
#include "endpoint/tcp_endpoint.h"
#include "endpoint/udp_endpoint.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

bool app::load(const std::string& path)
{
    std::string err;
    if (!cfg_.load(path, &err))
    {
        LOG_ERR("%s", err.c_str());
        return false;
    }
    if (!parse_host(cfg_.device, &device_ip_))
    {
        LOG_ERR("cannot resolve winject.device %s", cfg_.device.c_str());
        return false;
    }
    if (!cfg_.local_ip.empty() && !parse_host(cfg_.local_ip, &local_ip_))
    {
        LOG_ERR("invalid winject.local_ip");
        return false;
    }
    return true;
}

bool app::setup_upstreams()
{
    scheduler_.configure(cfg_.max_rate_kbps);
    for (size_t i = 0; i < cfg_.upstreams.size(); i++)
    {
        const auto& uc = cfg_.upstreams[i];
        auto radio = std::make_unique<wifi_udp>();
        sockaddr_in inject = {};
        inject.sin_family = AF_INET;
        inject.sin_addr = device_ip_;
        inject.sin_port = htons(static_cast<uint16_t>(9000 + i * 10));
        const uint16_t fwd =
            static_cast<uint16_t>(cfg_.forward_base + i);
        wifi_udp* radio_ptr = radio.get();
        if (!radio->open(reactor_, inject, fwd,
                         [this, i](const uint8_t* data, size_t len)
                         {
                             if (i < upstreams_.size() && upstreams_[i])
                             {
                                 upstreams_[i]->on_radio_rx(data, len);
                             }
                         },
                         [this]()
                         {
                             scheduler_.tick();
                         }))
        {
            return false;
        }

        std::unique_ptr<stream> up;
        if (uc.mode == upstream_mode_e::tcp_client ||
            uc.mode == upstream_mode_e::tcp_server)
        {
            auto tcp = std::make_unique<tcp_endpoint>();
            if (!tcp->open(reactor_, uc))
            {
                return false;
            }
            tcp->set_tx_kick([this]()
                             {
                                 scheduler_.tick();
                             });
            up = std::move(tcp);
        }
        else
        {
            auto udp = std::make_unique<udp_endpoint>();
            if (!udp->open(reactor_, uc))
            {
                return false;
            }
            up = std::move(udp);
        }
        scheduler_.add(up.get(), radio_ptr, uc.scheduler_budget);
        radios_.push_back(std::move(radio));
        upstreams_.push_back(std::move(up));
        LOG_INF("upstream-%zu radio inject=%u forward=%u", i,
                radios_.back()->inject_port(), radios_.back()->forward_port());
    }
    return true;
}

bool app::apply_console()
{
    std::string err;
    if (cfg_.local_ip.empty())
    {
        local_ip_ = console_.local_ip();
    }
    if (!console_.apply_radio(cfg_, &err))
    {
        LOG_ERR("%s", err.c_str());
        return false;
    }
    for (size_t i = 0; i < cfg_.upstreams.size(); i++)
    {
        if (!console_.apply_upstream(cfg_, cfg_.upstreams[i],
                                     radios_[i]->inject_port(),
                                     radios_[i]->forward_port(), local_ip_,
                                     &err))
        {
            LOG_ERR("%s", err.c_str());
            return false;
        }
    }
    if (!set_nonblock(console_.fd()))
    {
        LOG_ERR("console fcntl failed");
        return false;
    }
    return true;
}

void app::drop_console()
{
    const int fd = console_.fd();
    if (fd >= 0)
    {
        reactor_.rem_read_rdy(fd);
        reactor_.rem_write_rdy(fd);
    }
    console_.close();
    console_ok_ = false;
    console_connecting_ = false;
}

void app::begin_console()
{
    if (console_ok_ || console_connecting_)
    {
        return;
    }
    std::string err;
    if (!console_.start_connect(cfg_, &err))
    {
        LOG_ERR("console: %s", err.c_str());
        return;
    }
    console_connecting_ = true;
    reconnect_ticks_ = 0;
    if (!reactor_.add_write_rdy(console_.fd(),
                                [this]()
                                {
                                    on_console_connecting();
                                }) ||
        !reactor_.req_write(console_.fd()))
    {
        drop_console();
    }
}

void app::on_console_connecting()
{
    if (!console_connecting_)
    {
        return;
    }
    std::string err;
    if (!console_.finish_connect(&err))
    {
        LOG_ERR("console: %s", err.c_str());
        drop_console();
        return;
    }
    LOG_INF("console connected %s:%u local %s", cfg_.device.c_str(),
            cfg_.console_port, ipv4_to_string(console_.local_ip()).c_str());
    if (!apply_console())
    {
        drop_console();
        return;
    }
    console_connecting_ = false;
    console_ok_ = true;
    reactor_.rem_write_rdy(console_.fd());
    if (!reactor_.add_read_rdy(console_.fd(),
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
    if (!console_ok_)
    {
        return;
    }
    char buf[128];
    const ssize_t n = recv(console_.fd(), buf, sizeof(buf), 0);
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
    if (cfg_.skip_console)
    {
        return;
    }
    if (console_ok_)
    {
        reconnect_ticks_ = 0;
        return;
    }
    reconnect_ticks_++;
    if (console_connecting_)
    {
        if (reconnect_ticks_ >= 4000)
        {
            LOG_ERR("console connect timeout");
            drop_console();
        }
        return;
    }
    if (reconnect_ticks_ < 4000)
    {
        return;
    }
    reconnect_ticks_ = 0;
    LOG_INF("retrying console");
    begin_console();
}

void app::arm_tick()
{
    // Match original winject DEFAULT_SLOT_INTERVAL_US (500 us).
    reactor_.get_timer().wait_us(500,
                                 [this]()
                                 {
                                     reconnect_tick();
                                     stats_tick();
                                     scheduler_.tick();
                                     arm_tick();
                                 });
}

void app::stats_tick()
{
    if (cfg_.stats_sec == 0)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_stats_.time_since_epoch().count() == 0)
    {
        last_stats_ = now;
        return;
    }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_stats_)
            .count();
    if (elapsed_ms < static_cast<long>(cfg_.stats_sec) * 1000)
    {
        return;
    }
    last_stats_ = now;
    std::vector<stream*> ups;
    ups.reserve(upstreams_.size());
    for (const auto& up : upstreams_)
    {
        ups.push_back(up.get());
    }
    scheduler_.log_stats(elapsed_ms / 1000.0, ups);
}

void app::stop()
{
    reactor_.stop();
}

void app::flush_shutdown()
{
    for (auto& up : upstreams_)
    {
        if (up)
        {
            up->announce_down();
        }
    }
    // CLOSE is ctrl (not rate-limited). One pass injects the repeats.
    scheduler_.tick();
}

int app::run()
{
    if (!setup_upstreams())
    {
        return 1;
    }
    std::vector<uint16_t> inject_ports;
    std::vector<uint16_t> forward_ports;
    for (const auto& radio : radios_)
    {
        inject_ports.push_back(radio->inject_port());
        forward_ports.push_back(radio->forward_port());
    }
  if (!cfg_.local_ip.empty() && parse_host(cfg_.local_ip, &local_ip_))
    {
        // keep configured local_ip for set_upstream_tx
    }
    std::string err;
    if (!cfg_.skip_console)
    {
        in_addr console_local = {};
        if (!console_.program(cfg_, inject_ports, forward_ports, &console_local,
                              &err))
        {
            LOG_ERR("%s", err.c_str());
            return 1;
        }
        if (cfg_.local_ip.empty())
        {
            local_ip_ = console_local;
        }
    }
    else if (!cfg_.local_ip.empty())
    {
        parse_host(cfg_.local_ip, &local_ip_);
    }
    else
    {
        LOG_ERR("winject.skip_console requires winject.local_ip");
        return 1;
    }
    LOG_INF("manager running local %s forward_base %u max_rate %u kbps (%s)",
            ipv4_to_string(local_ip_).c_str(), cfg_.forward_base,
            cfg_.max_rate_kbps, cfg_.modulation.c_str());
    if (cfg_.stats_sec > 0)
    {
        LOG_INF("stats logging every %u s (WINJECT_STATS_SEC or winject.stats_sec)",
                cfg_.stats_sec);
    }
    arm_tick();
    reactor_.run();
    flush_shutdown();
    return 0;
}
