#ifndef WINJECT_MANAGER_RADIO_UDP_H_
#define WINJECT_MANAGER_RADIO_UDP_H_

#include "net_util.h"
#include "reactor.h"

#include <netinet/in.h>
#include <stdint.h>

#include <functional>

class RadioUdp
{
public:
    using Rx = std::function<void(const uint8_t* data, size_t len)>;
    using Idle = std::function<void()>;

    RadioUdp() = default;
    ~RadioUdp();
    RadioUdp(const RadioUdp&) = delete;
    RadioUdp& operator=(const RadioUdp&) = delete;

    bool open(Reactor& reactor, const sockaddr_in& inject, uint16_t forward_port,
              Rx on_rx, Idle on_idle = {});
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

    Reactor* reactor_ = nullptr;
    bfc::socket sock_;
    uint16_t forward_port_ = 0;
    sockaddr_in inject_{};
    Rx on_rx_;
    Idle on_idle_;
    uint8_t buf_[2048]{};
};

#endif  // WINJECT_MANAGER_RADIO_UDP_H_
