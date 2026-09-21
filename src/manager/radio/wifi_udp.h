#ifndef WINJECT_MANAGER_WIFI_UDP_H_
#define WINJECT_MANAGER_WIFI_UDP_H_

#include "net_util.h"
#include "reactor.h"

#include <netinet/in.h>
#include <stdint.h>

#include <functional>

// Opaque full-MPDU UDP transport to/from the ESP32 radio.
class wifi_udp
{
public:
    using rx = std::function<void(const uint8_t* data, size_t len)>;
    using idle = std::function<void()>;

    static constexpr size_t k_mpdu_max = 1500;

    wifi_udp() = default;
    ~wifi_udp();
    wifi_udp(const wifi_udp&) = delete;
    wifi_udp& operator=(const wifi_udp&) = delete;

    bool open(::reactor& reactor, const sockaddr_in& inject, uint16_t forward_port,
              rx on_rx, idle on_idle = {});
    void close();
    bool send(const uint8_t* mpdu, size_t len);
    uint16_t forward_port() const
    {
        return forward_port_;
    }
    uint16_t inject_port() const
    {
        return ntohs(inject.sin_port);
    }

    struct counters_s
    {
        uint64_t tx_byte = 0;
        uint64_t rx_byte = 0;
        uint64_t tx_pkt = 0;
        uint64_t rx_pkt = 0;
    };
    counters_s peek_counters() const
    {
        counters_s c;
        c.tx_byte = tx_byte_;
        c.rx_byte = rx_byte_;
        c.tx_pkt = tx_pkt_;
        c.rx_pkt = rx_pkt_;
        return c;
    }

private:
    void on_forward();

    ::reactor* reactor = nullptr;
    bfc::socket sock;
    uint16_t forward_port_ = 0;
    sockaddr_in inject{};
    rx on_rx;
    idle on_idle;
    uint64_t tx_byte_ = 0;
    uint64_t rx_byte_ = 0;
    uint64_t tx_pkt_ = 0;
    uint64_t rx_pkt_ = 0;
    uint8_t buf[2048]{};
};

#endif  // WINJECT_MANAGER_WIFI_UDP_H_
