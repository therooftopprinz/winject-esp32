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
    bus_t bus;
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

    bool add_endpoint(bus_t bus, ip_port_t dest);
    bool rem_endpoint(bus_t bus);
    bool load(const lc_rx_bind_s* binds, uint8_t count);
    void fill_status(lc_rx_bind_s* out, uint8_t* count);

    void forward(bus_t bus, packet&& pdu);

private:
    lc_rx_endpoint() = default;

    struct entry_s
    {
        bool used = false;
        bus_t bus = 0;
        ip_port_t dest{};
        std::atomic<uint32_t> drop_send_fail{0};
    };

    bool ensure_socket();
    void send_one(entry_s& e, const uint8_t* data, size_t len);
    int find_exact(bus_t bus, ip_port_t dest) const;
    int find_free() const;

    entry_s ep[WIFI_AIRPORT_MAX];
    bfc::socket send_sock;
    bfc::semaphore lock;
};

#endif  // WINJECT_LC_RX_ENDPOINT_H_
