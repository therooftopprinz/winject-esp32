#ifndef WINJECT_UPSTREAM_TX_H_
#define WINJECT_UPSTREAM_TX_H_

#include "config.h"

#include <stddef.h>
#include <stdint.h>

#include "bfc-esp32/socket.hpp"
#include "winject-esp32/semaphore.h"

class ethernet;

struct upstream_dest_s
{
    uint8_t airport[6];
    uint32_t host;
    uint16_t port;
};

class upstream_tx
{
public:
    upstream_tx() = default;
    upstream_tx(const upstream_tx&) = delete;
    upstream_tx& operator=(const upstream_tx&) = delete;

    bool init(ethernet& eth);
    bool start_task();
    bool load(const upstream_dest_s* tx, uint8_t tx_count);
    void fill_status(upstream_dest_s* out, uint8_t* count);
    bool set(const uint8_t airport[6], uint32_t host, uint16_t port);
    bool unset(const uint8_t airport[6]);

private:
    struct entry_s
    {
        bool used = false;
        uint8_t airport[6]{};
        uint32_t host = 0;
        uint16_t port = 0;
    };

    static void task(void* arg);

    bool open_send(bfc::socket* out);
    int find_airport(const uint8_t airport[6]) const;
    int find_free() const;
    bool any_tx() const;
    void publish_peer_airports();
    void close_sockets();
    bool ensure_sockets();
    void drain_radio();
    void send_payload(const uint8_t* payload, size_t payload_len, uint32_t host,
                      uint16_t port);
    void forward_mpdu(const uint8_t* mpdu, size_t len);
    void forward_rx();
    void run();

    entry_s tx_[WIFI_AIRPORT_MAX];
    bfc::socket send_sock_;
    uint8_t buf_[WIFI_RADIO_MAX_FRAME]{};
    semaphore lock_;
    ethernet* eth_ = nullptr;
    bool ready_ = false;
};

#endif  // WINJECT_UPSTREAM_TX_H_
