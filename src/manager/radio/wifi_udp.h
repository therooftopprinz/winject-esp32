#ifndef WINJECT_MANAGER_WIFI_UDP_H_
#define WINJECT_MANAGER_WIFI_UDP_H_

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

    bool open(reactor& reactor, const sockaddr_in& inject, uint16_t forward_port,
              rx on_rx, idle on_idle = {});
    void close();
    bool send(const uint8_t* data, size_t len);
    uint16_t forward_port() const
    {
        return forward_port_;
    }
    uint16_t inject_port() const
    {
        return ntohs(inject_.sin_port);
    }

private:
    void on_forward();

    reactor* reactor_ = nullptr;
    bfc::socket sock_;
    uint16_t forward_port_ = 0;
    sockaddr_in inject_{};
    rx on_rx_;
    idle on_idle_;
    uint8_t buf_[2048]{};
};

#endif  // WINJECT_MANAGER_WIFI_UDP_H_
