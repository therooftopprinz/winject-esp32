#ifndef WINJECT_CHANNEL_INFO_ENDPOINT_H_
#define WINJECT_CHANNEL_INFO_ENDPOINT_H_

#include "config.h"
#include "packet.h"

#include <atomic>
#include <stdint.h>

#include "bfc-esp32/semaphore.hpp"
#include "bfc-esp32/socket.hpp"
#include "bfc-esp32/task_reactor.hpp"

enum channel_info_type_e : uint8_t
{
    E_CHANNEL_INFO_TYPE_FLOW_CTRL = 1,
    E_CHANNEL_INFO_TYPE_RX_AIR = 2,
};

struct tx_flow_ctrl_s
{
    uint8_t info_type;
    uint8_t tx_queue_size;
    uint8_t tx_queue_capacity;
} __attribute__((packed));

struct rx_air_info_s
{
    uint8_t info_type;
    int8_t rssi;
    int8_t snr;
} __attribute__((packed));

class channel_info_endpoint
{
public:
    static constexpr uint8_t k_subscriber_max = WIFI_AIRPORT_MAX;
    static constexpr uint32_t k_rx_air_interval_ms = 100;

    static channel_info_endpoint& instance();
    channel_info_endpoint(const channel_info_endpoint&) = delete;
    channel_info_endpoint& operator=(const channel_info_endpoint&) = delete;

    bool init();
    bool start(BaseType_t core = APP_TASK_CORE,
               UBaseType_t prio = NETMGR_TASK_PRIO,
               uint32_t stack_bytes = 6144);

    bool add_subscriber(ip_port_t subscriber);
    bool rem_subscriber(ip_port_t subscriber);
    bool load(const ip_port_t* subs, uint8_t count);
    void fill_status(ip_port_t* out, uint8_t* count);

    void on_rx_air_info(int8_t rssi, int8_t snr);
    void on_flow_ctrl_info(uint8_t queue_size, uint8_t queue_cap);

private:
    using reactor_t = bfc::task_reactor<>;

    channel_info_endpoint() = default;

    void on_rx_air_timer();
    void fanout(const void* data, size_t len);
    bool ensure_socket();
    bool send_one(const ip_port_t& dest, const void* data, size_t len);
    int find_sub(ip_port_t subscriber) const;

    ip_port_t subs[k_subscriber_max]{};
    uint8_t n_subs = 0;
    bfc::socket send_sock;
    bfc::semaphore lock;
    reactor_t reactor;
    std::atomic<int8_t> rssi{0};
    std::atomic<int8_t> snr{0};
    std::atomic<bool> air_valid{false};
};

#endif  // WINJECT_CHANNEL_INFO_ENDPOINT_H_
