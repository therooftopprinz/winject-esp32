#include "console.h"

#include "config.h"
#include "ether_bench.h"
#include "manager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "console";

namespace
{
bool set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void close_fd(int* fd)
{
    if (fd == nullptr || *fd < 0)
    {
        return;
    }
    close(*fd);
    *fd = -1;
}

void ipv4_to_string(uint32_t addr, char* out, size_t out_len)
{
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&addr);
    snprintf(out, out_len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}
}  // namespace

void console::unwatch_fd(int fd)
{
    if (reactor != nullptr && fd >= 0)
    {
        reactor->rem_read_rdy(fd);
    }
}

void console::stop_udp()
{
    unwatch_fd(sock_fd);
    close_fd(&sock_fd);
    reply_to_set = false;
    reply_len = 0;
}

void console::write_bytes(const void* data, size_t n)
{
    if (data == nullptr || n == 0 || sock_fd < 0 || !reply_to_set)
    {
        return;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t off = 0;
    while (off < n)
    {
        if (reply_len >= k_reply_max)
        {
            flush_reply();
            if (reply_len >= k_reply_max)
            {
                return;
            }
        }
        const size_t room = k_reply_max - reply_len;
        const size_t chunk = n - off < room ? n - off : room;
        memcpy(reply + reply_len, bytes + off, chunk);
        reply_len += chunk;
        off += chunk;
    }
}

void console::write(const char* text)
{
    if (text == nullptr || *text == '\0')
    {
        return;
    }
    write_bytes(text, strlen(text));
}

void console::flush_reply()
{
    if (sock_fd < 0 || !reply_to_set || reply_len == 0)
    {
        reply_len = 0;
        return;
    }
    const int64_t deadline_us = esp_timer_get_time() + k_send_timeout_us;
    while (true)
    {
        const int sent = sendto(sock_fd, reply, reply_len, 0,
                                reinterpret_cast<struct sockaddr*>(&reply_to),
                                sizeof(reply_to));
        if (sent < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (esp_timer_get_time() >= deadline_us)
                {
                    ESP_LOGW(TAG, "udp send timeout");
                    break;
                }
                vTaskDelay(1);
                continue;
            }
            ESP_LOGW(TAG, "udp send failed: %d", errno);
            break;
        }
        break;
    }
    reply_len = 0;
}

void console::print(const char* fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(buf);
}

void console::reply_ok()
{
    write("ok\n");
}

void console::reply_ok_args(const char* args)
{
    if (args == nullptr || *args == '\0')
    {
        reply_ok();
        return;
    }
    write("ok ");
    write(args);
    const size_t n = strlen(args);
    if (n == 0 || args[n - 1] != '\n')
    {
        write("\n");
    }
}

void console::reply_nok(const char* msg)
{
    print("nok %s\n", msg != nullptr ? msg : "error");
}

void console::reply_usage(const char* usage)
{
    print("nok usage %s\n", usage);
}

void console::reply_done(bool ok, const char* fail)
{
    if (ok)
    {
        reply_ok();
    }
    else
    {
        reply_nok(fail);
    }
}

void console::start_udp()
{
    if (sock_fd >= 0)
    {
        return;
    }

    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "socket failed: %d", errno);
        return;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    const int rcvbuf = 48 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    const int sndbuf = static_cast<int>(k_reply_max);
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROL_CONSOLE_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ESP_LOGE(TAG, "bind *:%u failed: %d", CONTROL_CONSOLE_PORT, errno);
        close(fd);
        return;
    }
    if (!set_nonblock(fd))
    {
        ESP_LOGE(TAG, "udp nonblock failed: %d", errno);
        close(fd);
        return;
    }

    sock_fd = fd;
    if (reactor != nullptr)
    {
        reactor->add_read_rdy(sock_fd,
                              [this]()
                              {
                                  on_datagram();
                              });
    }

    uint32_t ip = 0;
    if (netmgr->local_ipv4(&ip))
    {
        char ip_str[16];
        ipv4_to_string(ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "control console udp %s:%u", ip_str,
                 CONTROL_CONSOLE_PORT);
    }
}

void console::on_datagram()
{
    if (sock_fd < 0)
    {
        return;
    }

    while (true)
    {
        sockaddr_in peer = {};
        socklen_t peer_len = sizeof(peer);
        const int n =
            recvfrom(sock_fd, recv_buf, sizeof(recv_buf) - 1, 0,
                     reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            {
                return;
            }
            ESP_LOGW(TAG, "udp recv failed: %d", errno);
            return;
        }
        if (n == 0)
        {
            continue;
        }
        reply_to = peer;
        reply_to_set = true;
        reply_len = 0;
        handle_datagram(recv_buf, static_cast<size_t>(n));
        flush_reply();
        reply_to_set = false;
    }
}

void console::sync_udp()
{
    if (netmgr == nullptr)
    {
        return;
    }
    if (netmgr->connected())
    {
        start_udp();
    }
    else
    {
        stop_udp();
    }
}

void console::schedule_sync()
{
    if (reactor == nullptr)
    {
        return;
    }
    reactor->get_timer().wait_ms(k_sync_ms,
                                 [this]()
                                 {
                                     sync_udp();
                                     schedule_sync();
                                 });
}

void console::attach_reactor()
{
    schedule_sync();
    sync_udp();
}

bool console::init_common(manager& netmgr)
{
    if (ready)
    {
        return true;
    }

    this->netmgr = &netmgr;
    reactor = &netmgr.reactor();
    reactor->wake_up(
        [this]()
        {
            attach_reactor();
        });

    ready = true;
    ether_bench::instance().init(netmgr);
    return true;
}

bool console::init(manager& netmgr)
{
    tx_ep = nullptr;
    rx_ep = nullptr;
    ci = nullptr;
    return init_common(netmgr);
}

bool console::init(lc_tx_endpoint& tx_ep, lc_rx_endpoint& rx_ep,
                   channel_info_endpoint& ci, manager& netmgr)
{
    this->tx_ep = &tx_ep;
    this->rx_ep = &rx_ep;
    this->ci = &ci;
    return init_common(netmgr);
}
