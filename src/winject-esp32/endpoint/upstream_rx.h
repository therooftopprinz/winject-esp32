#ifndef WINJECT_UPSTREAM_RX_H_
#define WINJECT_UPSTREAM_RX_H_

#include "config.h"

#include <stdint.h>

#include "bfc-esp32/semaphore.hpp"
#include "bfc-esp32/socket.hpp"

class ethernet;

struct upstream_bind_s
{
    uint8_t airport[6];
    uint16_t port;
    bool socket_open;
};

class upstream_rx
{
public:
    static upstream_rx& instance();
    upstream_rx(const upstream_rx&) = delete;
    upstream_rx& operator=(const upstream_rx&) = delete;

    bool init(ethernet& eth);
    bool start_task();
    bool load(const upstream_bind_s* rx, uint8_t rx_count);
    void fill_status(upstream_bind_s* out, uint8_t* count);
    bool set(const uint8_t airport[6], uint16_t port);
    bool unset(const uint8_t airport[6]);

private:
    upstream_rx() = default;

    struct entry_s
    {
        bool used = false;
        uint8_t airport[6]{};
        uint16_t port = 0;
        bfc::socket sock;
    };

    static void task(void* arg);

    bool open_bound(uint16_t port, bfc::socket* out);
    int find_airport(const uint8_t airport[6]) const;
    int find_port(uint16_t port, int except) const;
    int find_free() const;
    void clear_slot(int idx);
    void publish_local_airports();
    void close_sockets();
    bool ensure_sockets();
    void inject_one(bfc::socket& sock, const uint8_t sa[6]);
    void inject_pending();
    void run();

    entry_s rx_[WIFI_AIRPORT_MAX];
    uint8_t buf_[WIFI_RADIO_MAX_FRAME]{};
    bfc::semaphore lock_;
    ethernet* eth_ = nullptr;
    bool ready_ = false;
};

#endif  // WINJECT_UPSTREAM_RX_H_
