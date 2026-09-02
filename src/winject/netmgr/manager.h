#ifndef WINJECT_NETMGR_MANAGER_H_
#define WINJECT_NETMGR_MANAGER_H_

#include <atomic>
#include <stdint.h>

#include "bfc/select_reactor.hpp"
#include "bfc/timer.hpp"
#include "freertos/semphr.h"

class dhcp_client;
class dhcp_server;
class ethernet;

enum NetmgrMode
{
    NETMGR_MODE_STATIC = 0,
    NETMGR_MODE_AUTO = 1,
};

class manager
{
public:
    using reactor_t = bfc::select_reactor<>;

    static manager& instance();
    manager(const manager&) = delete;
    manager& operator=(const manager&) = delete;
    ~manager();

    bool start();
    reactor_t& reactor();
    bool connected() const;
    NetmgrMode network_mode() const;
    static const char* network_mode_name(NetmgrMode mode);
    static bool parse_network_mode(const char* text, NetmgrMode* mode);
    bool set_network_mode(NetmgrMode mode);
    bool dhcp_server_enabled() const;
    bool dhcp_server_active() const;
    bool set_dhcp_server_enabled(bool enabled);
    bool set_ip(uint32_t ip);
    bool static_ipv4(uint32_t* out) const;
    bool dhcp_pool(uint32_t* start, uint32_t* end) const;
    bool local_ipv4(uint32_t* out) const;

private:
    using timer_id_t = bfc::timer<>::timer_id_t;

    manager();
    static void reactor_task(void* arg);

    void run_reactor();
    void bring_up();
    void on_static_fallback(uint32_t gen);
    void schedule_static_fallback();
    void cancel_static_fallback();

    void ensure_static_ip() const;
    uint32_t static_ip() const;
    bool dhcp_server_should_run() const;
    bool apply_network_locked();
    bool apply_network();

    ethernet& eth_;
    dhcp_client& dhcp_client_;
    dhcp_server& dhcp_server_;
    reactor_t reactor_;
    std::atomic<bool> started_{false};
    std::atomic<bool> init_ok_{false};
    std::atomic<bool> dhcp_server_wanted_{false};
    mutable std::atomic<uint32_t> static_ip_{0};
    std::atomic<uint32_t> auto_gen_{0};
    std::atomic<NetmgrMode> network_mode_{NETMGR_MODE_AUTO};
    timer_id_t fallback_timer_id_{};
    bool fallback_timer_set_ = false;
    SemaphoreHandle_t init_done_ = nullptr;
};

#endif  // WINJECT_NETMGR_MANAGER_H_
