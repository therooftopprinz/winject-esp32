#ifndef WINJECT_UPSTREAM_RX_ENDPOINT_H_
#define WINJECT_UPSTREAM_RX_ENDPOINT_H_

#include "config.h"
#include "packet.h"

#include <atomic>
#include <stdint.h>

#include "bfc-esp32/semaphore.hpp"
#include "bfc-esp32/socket.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class wifi_rx;

struct upstream_rx_bind_s
{
    ip_port_t dest;
    bool active;
    uint32_t drop_send_fail;
};

class upstream_rx_endpoint
{
public:
    static upstream_rx_endpoint& instance();
    upstream_rx_endpoint(const upstream_rx_endpoint&) = delete;
    upstream_rx_endpoint& operator=(const upstream_rx_endpoint&) = delete;

    bool init(wifi_rx& rx);
    bool start(BaseType_t core = APP_TASK_CORE,
               UBaseType_t prio = UPSTREAM_RX_TASK_PRIO,
               uint32_t stack_bytes = UPSTREAM_RX_TASK_STACK);

    bool set_endpoint(ip_port_t dest);
    bool clear();
    bool get_status(upstream_rx_bind_s* out);

private:
    upstream_rx_endpoint() = default;

    static void drain_task(void* arg);
    void run_drain();

    bool ensure_socket();
    void send_one(ip_port_t d, const uint8_t* data, size_t len);

    wifi_rx* rx = nullptr;
    bool started = false;
    TaskHandle_t drain_task_handle_ = nullptr;

    bool used = false;
    ip_port_t dest{};
    std::atomic<uint32_t> dest_host_{0};
    std::atomic<uint16_t> dest_port_{0};
    std::atomic<bool> dest_active_{false};
    std::atomic<uint32_t> drop_send_fail{0};
    bfc::socket send_sock;
    bfc::semaphore lock;
};

#endif  // WINJECT_UPSTREAM_RX_ENDPOINT_H_
