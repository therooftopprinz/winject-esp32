#include "lc_rx_endpoint.h"

#include "wifi.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <utility>

#include "esp_log.h"
#include "lwip/sockets.h"

static const char* TAG = "lc_rx_ep";

lc_rx_endpoint& lc_rx_endpoint::instance()
{
    static lc_rx_endpoint inst;
    return inst;
}

bool lc_rx_endpoint::ensure_socket()
{
    if (send_sock.valid())
    {
        return true;
    }
    if (!send_sock.open_udp(htonl(INADDR_ANY), 0))
    {
        ESP_LOGE(TAG, "udp send socket failed: %d", errno);
        return false;
    }
    return true;
}

void lc_rx_endpoint::send_one(ip_port_t d, const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
    {
        return;
    }
    if (!ensure_socket())
    {
        drop_send_fail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = d.host;
    addr.sin_port = htons(d.port);
    const ssize_t n = send_sock.send(data, len, 0,
                                      reinterpret_cast<const sockaddr*>(&addr),
                                      sizeof(addr));
    if (n == static_cast<ssize_t>(len))
    {
        wifi::instance().note_udp_fwd_pkt();
        return;
    }
    drop_send_fail.fetch_add(1, std::memory_order_relaxed);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        ESP_LOGD(TAG, "sendto failed: %d", errno);
    }
}

bool lc_rx_endpoint::init()
{
    return lock.init();
}

bool lc_rx_endpoint::set_endpoint(ip_port_t d)
{
    if (d.port == 0 || d.host == 0 || d.host == 0xFFFFFFFFu || !lock.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(lock);
    if (!guard)
    {
        return false;
    }

    if (used && ip_port_eq(dest, d))
    {
        return true;
    }

    used = true;
    dest = d;
    drop_send_fail.store(0, std::memory_order_relaxed);

    char ip_str[16];
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&d.host);
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    ESP_LOGI(TAG, "sur %s:%u", ip_str, d.port);
    return true;
}

bool lc_rx_endpoint::clear()
{
    if (!lock.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(lock);
    if (!guard)
    {
        return false;
    }
    used = false;
    dest = {};
    drop_send_fail.store(0, std::memory_order_relaxed);
    return true;
}

bool lc_rx_endpoint::get_status(lc_rx_bind_s* out)
{
    if (out == nullptr || !lock.ready())
    {
        return false;
    }
    bfc::semaphore::lock guard(lock);
    if (!guard)
    {
        return false;
    }
    if (!used)
    {
        return false;
    }
    out->dest = dest;
    out->active = send_sock.valid();
    out->drop_send_fail = drop_send_fail.load(std::memory_order_relaxed);
    return true;
}

void lc_rx_endpoint::forward(packet&& mpdu)
{
    ip_port_t d{};
    {
        bfc::semaphore::lock guard(lock);
        if (!guard || !used)
        {
            return;
        }
        d = dest;
    }
    if (!mpdu.is_valid() || mpdu.data() == nullptr || mpdu.size() == 0)
    {
        return;
    }
    send_one(d, mpdu.data(), mpdu.size());
}
