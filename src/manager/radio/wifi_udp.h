#ifndef WINJECT_MANAGER_WIFI_UDP_H_
#define WINJECT_MANAGER_WIFI_UDP_H_

#include "frames/air_seq.h"
#include "net_util.h"
#include "reactor.h"

#include <netinet/in.h>
#include <stdint.h>

#include <functional>

class wifi_udp
{
public:
    using rx = std::function<void(const uint8_t* data, size_t len)>;
    using idle = std::function<void()>;

    wifi_udp() = default;
    ~wifi_udp();
    wifi_udp(const wifi_udp&) = delete;
    wifi_udp& operator=(const wifi_udp&) = delete;

    bool open(::reactor& reactor, const sockaddr_in& inject, uint16_t forward_port,
              rx on_rx, idle on_idle = {});
    void close();
    bool send(const uint8_t* data, size_t len);
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
        uint64_t rx_pkt_loss = 0;
    };
    // Lifetime air counters (seq prefix included in byte totals). Never reset.
    counters_s peek_counters() const
    {
        counters_s c;
        c.tx_byte = tx_byte_;
        c.rx_byte = rx_byte_;
        c.tx_pkt = tx_pkt_;
        c.rx_pkt = rx_pkt_;
        c.rx_pkt_loss = seq.lost();
        return c;
    }
    // Lost since last stats interval. Does not reset lifetime seq.lost().
    uint64_t take_lost_interval()
    {
        const uint64_t now = seq.lost();
        const uint64_t d = now - lost_seen_;
        lost_seen_ = now;
        return d;
    }

private:
    void on_forward();

    ::reactor* reactor = nullptr;
    bfc::socket sock;
    uint16_t forward_port_ = 0;
    sockaddr_in inject{};
    rx on_rx;
    idle on_idle;
    air_seq seq;
    uint64_t tx_byte_ = 0;
    uint64_t rx_byte_ = 0;
    uint64_t tx_pkt_ = 0;
    uint64_t rx_pkt_ = 0;
    uint64_t lost_seen_ = 0;
    uint8_t buf[2048]{};
    uint8_t txbuf[2048]{};
};

#endif  // WINJECT_MANAGER_WIFI_UDP_H_
