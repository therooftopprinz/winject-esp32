#ifndef WINJECT_LC_TX_ENDPOINT_H_
#define WINJECT_LC_TX_ENDPOINT_H_

#include "config.h"
#include "lc_tx.h"
#include "packet.h"

#include <atomic>
#include <stdint.h>

#include "bfc-esp32/select_reactor.hpp"
#include "bfc-esp32/semaphore.hpp"
#include "bfc-esp32/socket.hpp"

struct lc_tx_bind_s
{
    bus_t bus;
    uint16_t udp_port;
    bool socket_open;
    uint32_t drop_no_pkt_pool;
    uint32_t drop_queue_full;
};

class lc_tx_endpoint
{
public:
    static lc_tx_endpoint& instance();
    lc_tx_endpoint(const lc_tx_endpoint&) = delete;
    lc_tx_endpoint& operator=(const lc_tx_endpoint&) = delete;

    bool init(lc_tx& tx);
    bool start(BaseType_t core = APP_TASK_CORE, UBaseType_t prio = UPSTREAM_TASK_PRIO, uint32_t stack_bytes = 6144);

    bool add_endpoint(bus_t bus, uint16_t udp_port);
    bool rem_endpoint(bus_t bus);
    bool clear();
    void get_status(lc_tx_bind_s* out, uint8_t* count);

private:
    using reactor_t = bfc::select_reactor<>;

    lc_tx_endpoint() = default;

    struct entry_s
    {
        bool used = false;
        bus_t bus = 0;
        uint16_t udp_port = 0;
        bfc::socket sock;
        std::atomic<uint32_t> drop_no_pkt_pool{0};
        std::atomic<uint32_t> drop_queue_full{0};
    };

    void on_readable(entry_s& e);
    bool drop_datagram(entry_s& e);
    void watch(entry_s& e);
    void unwatch(entry_s& e);
    void clear_slot(entry_s& e);
    int find_bus(bus_t bus) const;
    int find_port(uint16_t port, int except) const;
    int find_free() const;
    bool open_bound(uint16_t port, bfc::socket* out);

    entry_s ep[WIFI_AIRPORT_MAX];
    uint8_t drop_buf[WIFI_PAYLOAD_MAX]{};
    lc_tx* tx = nullptr;
    reactor_t reactor;
    bfc::semaphore lock;
    bool started = false;
};

#endif  // WINJECT_LC_TX_ENDPOINT_H_
