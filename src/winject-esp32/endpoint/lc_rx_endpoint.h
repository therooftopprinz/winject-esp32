#ifndef WINJECT_LC_RX_ENDPOINT_H_
#define WINJECT_LC_RX_ENDPOINT_H_

#include "config.h"
#include "packet.h"

#include <atomic>
#include <stdint.h>

#include "bfc-esp32/semaphore.hpp"
#include "bfc-esp32/socket.hpp"

struct lc_rx_bind_s
{
    ip_port_t dest;
    bool active;
    uint32_t drop_send_fail;
};

class lc_rx_endpoint
{
public:
    static lc_rx_endpoint& instance();
    lc_rx_endpoint(const lc_rx_endpoint&) = delete;
    lc_rx_endpoint& operator=(const lc_rx_endpoint&) = delete;

    bool init();

    bool set_endpoint(ip_port_t dest);
    bool clear();
    bool get_status(lc_rx_bind_s* out);

    void forward(packet&& mpdu);

private:
    lc_rx_endpoint() = default;

    bool ensure_socket();
    void send_one(ip_port_t d, const uint8_t* data, size_t len);

    bool used = false;
    ip_port_t dest{};
    std::atomic<uint32_t> drop_send_fail{0};
    bfc::socket send_sock;
    bfc::semaphore lock;
};

#endif  // WINJECT_LC_RX_ENDPOINT_H_
