#include "radio/wifi_udp.h"

#include "log.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>

wifi_udp::~wifi_udp()
{
    close();
}

bool wifi_udp::open(::reactor& reactor, const sockaddr_in& inject,
                    uint16_t forward_port, rx on_rx, idle on_idle)
{
    close();
    this->reactor = &reactor;
    this->inject = inject;
    this->on_rx = std::move(on_rx);
    this->on_idle = std::move(on_idle);
    forward_port_ = forward_port;
    sock = make_udp4();
    if (sock.fd() < 0)
    {
        LOG_ERR("radio udp socket failed: %s", strerror(errno));
        close();
        return false;
    }
    // make_udp4 enables SO_REUSEADDR for app binds. The radio forward port must
    // be exclusive or a stale manager can keep stealing unicast packets while
    // this process shows radio_rx=0.
    const int zero = 0;
    if (setsockopt(sock.fd(), SOL_SOCKET, SO_REUSEADDR, &zero, sizeof(zero)) <
            0 ||
        !bind_udp_port(sock.fd(), forward_port_))
    {
        LOG_ERR("radio udp bind %u failed: %s", forward_port_, strerror(errno));
        close();
        return false;
    }
    if (!reactor.add_read_rdy(sock.fd(),
                              [this]()
                              {
                                  on_forward();
                              }))
    {
        close();
        return false;
    }
    return true;
}

void wifi_udp::close()
{
    if (sock.fd() >= 0)
    {
        if (reactor != nullptr)
        {
            reactor->rem_read_rdy(sock.fd());
        }
        close_socket(&sock);
    }
    forward_port_ = 0;
}

bool wifi_udp::send(const uint8_t* data, size_t len)
{
    if (sock.fd() < 0 || data == nullptr || len == 0)
    {
        return false;
    }
    return udp_send_to(sock.fd(), inject, data, len);
}

void wifi_udp::on_forward()
{
    bool any = false;
    while (true)
    {
        const ssize_t n = udp_recv_from(sock.fd(), buf, sizeof(buf), nullptr);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            LOG_ERR("radio recv: %s", strerror(errno));
            break;
        }
        if (n == 0)
        {
            break;
        }
        any = true;
        if (on_rx)
        {
            on_rx(buf, static_cast<size_t>(n));
        }
    }
    // One scheduler pass after the burst so cumulative/SACK ACK uses the latest
    // rx state and goes out immediately (do not wait for the timer).
    if (any && on_idle)
    {
        on_idle();
    }
}
