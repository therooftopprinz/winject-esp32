#include "radio/channel_info.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "log.h"

channel_info::~channel_info()
{
    close();
}

bool channel_info::open(::reactor& reactor)
{
    close();
    this->reactor = &reactor;
    sock = make_udp4();
    if (sock.fd() < 0)
    {
        LOG_ERR("channel_info udp socket failed: %s", strerror(errno));
        close();
        return false;
    }
    // Exclusive bind: avoid a stale manager stealing CI unicast.
    const int zero = 0;
    if (setsockopt(sock.fd(), SOL_SOCKET, SO_REUSEADDR, &zero, sizeof(zero)) <
            0 ||
        !bind_udp_any(sock.fd(), &port_))
    {
        LOG_ERR("channel_info bind failed: %s", strerror(errno));
        close();
        return false;
    }
    if (!reactor.add_read_rdy(sock.fd(),
                              [this]()
                              {
                                  on_datagram();
                              }))
    {
        close();
        return false;
    }
    LOG_INF("channel_info listening on udp %u", port_);
    return true;
}

void channel_info::format_stamp(bool valid, const timespec& ts, char* buf,
                                size_t n)
{
    if (buf == nullptr || n == 0)
    {
        return;
    }
    if (!valid)
    {
        snprintf(buf, n, "-");
        return;
    }
    tm local = {};
    if (localtime_r(&ts.tv_sec, &local) == nullptr)
    {
        snprintf(buf, n, "-");
        return;
    }
    char date[32];
    if (strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &local) == 0)
    {
        snprintf(buf, n, "-");
        return;
    }
    const int ms = static_cast<int>(ts.tv_nsec / 1000000);
    snprintf(buf, n, "%s.%03d", date, ms);
}

void channel_info::close()
{
    if (sock.fd() >= 0)
    {
        if (reactor != nullptr)
        {
            reactor->rem_read_rdy(sock.fd());
        }
        close_socket(&sock);
    }
    port_ = 0;
    flow_ = {};
    air_ = {};
}

void channel_info::on_datagram()
{
    while (true)
    {
        const ssize_t n = udp_recv_from(sock.fd(), buf, sizeof(buf), nullptr);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            LOG_ERR("channel_info recv: %s", strerror(errno));
            break;
        }
        if (n <= 0)
        {
            break;
        }
        if (static_cast<size_t>(n) >= sizeof(flow_ctrl_s) &&
            buf[0] == static_cast<uint8_t>(type_e::flow_ctrl))
        {
            flow_ctrl_s sample = {};
            memcpy(&sample, buf, sizeof(sample));
            flow_.valid = true;
            flow_.tx_queue_size = sample.tx_queue_size;
            flow_.tx_queue_capacity = sample.tx_queue_capacity;
            clock_gettime(CLOCK_REALTIME, &flow_.received);
            continue;
        }
        if (static_cast<size_t>(n) >= sizeof(rx_air_s) &&
            buf[0] == static_cast<uint8_t>(type_e::rx_air))
        {
            rx_air_s sample = {};
            memcpy(&sample, buf, sizeof(sample));
            air_.valid = true;
            air_.rssi = sample.rssi;
            air_.snr = sample.snr;
            clock_gettime(CLOCK_REALTIME, &air_.received);
            continue;
        }
    }
}
