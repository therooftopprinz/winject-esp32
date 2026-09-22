#ifndef WINJECT_MANAGER_APP_H_
#define WINJECT_MANAGER_APP_H_

#include <cstdint>

#include "config.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "console_client.h"
#include "console_service.h"
#include "radio/wifi_udp.h"
#include "reactor.h"
#include "stream/stream.h"
#include "tx_scheduler.h"

class app
{
public:
    bool load(const std::string& path);
    int run();
    void stop();

private:
    bool add_upstream(const upstream_config_s& uc);
    bool setup_radio();
    bool setup_upstreams();
    bool start_manager_console();
    bool apply_console();
    bool hold_console();
    void begin_console();
    void drop_console();
    void on_console();
    void reconnect_tick();
    void heartbeat_tick();
    void arm_tick();
    void stats_tick();
    void flush_shutdown();

    bool set_upstream_fec(size_t index, fec_type_e type, int k, int n,
                          std::string* error);
    bool get_upstream_fec(size_t index, fec_type_e* type, int* k, int* n,
                          std::string* error);
    bool set_upstream_scheduler_budget(size_t index, size_t budget,
                                       std::string* error);
    bool get_upstream_scheduler_budget(size_t index, size_t* budget,
                                       std::string* error);
    bool set_modulation(const std::string& name, std::string* error);
    bool get_modulation(std::string* name, std::string* error);
    bool set_tx_pacing(const uint32_t* max_rate_kbps,
                       const size_t* max_data_per_tick,
                       const size_t* tx_burst_size,
                       const uint32_t* tx_burst_interval_us,
                       std::string* error);
    bool get_tx_pacing(uint32_t* max_rate_kbps, size_t* max_data_per_tick,
                       size_t* tx_burst_size, uint32_t* tx_burst_interval_us,
                       std::string* error) const;
    void fill_ci_view(channel_info_view_s* out) const;

    static constexpr int k_ping_interval_ticks = 1000;
    static constexpr int k_pong_timeout_ticks = 2000;
    static constexpr int k_reconnect_ticks = 4000;

    config cfg;
    ::reactor reactor;
    console_client console;
    console_service mgr_console;
    tx_scheduler scheduler;
    std::unique_ptr<wifi_udp> radio;
    std::vector<std::unique_ptr<stream>> upstreams;
    in_addr device_ip{};
    in_addr local_ip{};
    int reconnect_ticks = 0;
    int heartbeat_ticks = 0;
    bool console_ok = false;
    bool awaiting_pong = false;
    std::chrono::steady_clock::time_point last_stats{};
};

#endif  // WINJECT_MANAGER_APP_H_
