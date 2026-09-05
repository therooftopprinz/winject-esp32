#ifndef WINJECT_MANAGER_APP_H_
#define WINJECT_MANAGER_APP_H_

#include "config.h"
#include "console_client.h"
#include "radio/wifi_udp.h"
#include "reactor.h"
#include "tx_scheduler.h"
#include "stream/stream.h"

#include <memory>
#include <chrono>
#include <string>
#include <vector>

class app
{
public:
    bool load(const std::string& path);
    int run();
    void stop();

private:
    bool setup_upstreams();
    bool apply_console();
    void begin_console();
    void drop_console();
    void on_console_connecting();
    void on_console();
    void reconnect_tick();
    void arm_tick();
    void stats_tick();
    void flush_shutdown();

    config cfg_;
    reactor reactor_;
    console_client console_;
    tx_scheduler scheduler_;
    std::vector<std::unique_ptr<wifi_udp>> radios_;
    std::vector<std::unique_ptr<stream>> upstreams_;
    in_addr device_ip_{};
    in_addr local_ip_{};
    int reconnect_ticks_ = 0;
    bool console_ok_ = false;
    bool console_connecting_ = false;
    std::chrono::steady_clock::time_point last_stats_{};
};

#endif  // WINJECT_MANAGER_APP_H_
