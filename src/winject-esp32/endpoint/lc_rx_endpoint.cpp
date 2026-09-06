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
    if (send_sock_.valid())
    {
        return true;
    }
    if (!send_sock_.open_udp(htonl(INADDR_ANY), 0))
    {
        ESP_LOGE(TAG, "udp send socket failed: %d", errno);
        return false;
    }
    return true;
}

int lc_rx_endpoint::find_exact(bus_t bus, ip_port_t dest) const
{
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (ep_[i].used && ep_[i].bus == bus && ip_port_eq(ep_[i].dest, dest))
        {
            return i;
        }
    }
    return -1;
}

int lc_rx_endpoint::find_free() const
{
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (!ep_[i].used)
        {
            return i;
        }
    }
    return -1;
}

void lc_rx_endpoint::send_one(entry_s& e, const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0)
    {
        return;
    }
    if (!ensure_socket())
    {
        e.drop_send_fail.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = e.dest.host;
    addr.sin_port = htons(e.dest.port);
    const ssize_t n = send_sock_.send(data, len, 0,
                                      reinterpret_cast<const sockaddr*>(&addr),
                                      sizeof(addr));
    if (n == static_cast<ssize_t>(len))
    {
        wifi::instance().note_udp_fwd_pkt();
        return;
    }
    e.drop_send_fail.fetch_add(1, std::memory_order_relaxed);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
    {
        ESP_LOGD(TAG, "sendto failed: %d", errno);
    }
}

bool lc_rx_endpoint::init()
{
    return lock_.init();
}

bool lc_rx_endpoint::add_endpoint(bus_t bus, ip_port_t dest)
{
    if (dest.port == 0 || dest.host == 0 || dest.host == 0xFFFFFFFFu ||
        !lock_.ready())
    {
        return false;
    }
    bfc::semaphore::lock lock(lock_);
    if (!lock)
    {
        return false;
    }
    if (find_exact(bus, dest) >= 0)
    {
        return true;
    }
    const int idx = find_free();
    if (idx < 0)
    {
        ESP_LOGE(TAG, "rx bind table full");
        return false;
    }
    ep_[idx].used = true;
    ep_[idx].bus = bus;
    ep_[idx].dest = dest;
    ep_[idx].drop_send_fail.store(0, std::memory_order_relaxed);

    char ip_str[16];
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&dest.host);
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    ESP_LOGI(TAG, "sur bus=%02X %s:%u", bus, ip_str, dest.port);
    return true;
}

bool lc_rx_endpoint::rem_endpoint(bus_t bus)
{
    if (!lock_.ready())
    {
        return false;
    }
    bfc::semaphore::lock lock(lock_);
    if (!lock)
    {
        return false;
    }
    bool any = false;
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (ep_[i].used && ep_[i].bus == bus)
        {
            ep_[i].used = false;
            ep_[i].bus = 0;
            ep_[i].dest = {};
            ep_[i].drop_send_fail.store(0, std::memory_order_relaxed);
            any = true;
        }
    }
    (void)any;
    return true;
}

bool lc_rx_endpoint::load(const lc_rx_bind_s* binds, uint8_t count)
{
    if (!lock_.ready() || count > WIFI_AIRPORT_MAX ||
        (count > 0 && binds == nullptr))
    {
        return false;
    }
    bfc::semaphore::lock lock(lock_);
    if (!lock)
    {
        return false;
    }
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        ep_[i].used = false;
        ep_[i].bus = 0;
        ep_[i].dest = {};
        ep_[i].drop_send_fail.store(0, std::memory_order_relaxed);
    }
    for (uint8_t i = 0; i < count; i++)
    {
        ep_[i].used = true;
        ep_[i].bus = binds[i].bus;
        ep_[i].dest = binds[i].dest;
        ep_[i].drop_send_fail.store(0, std::memory_order_relaxed);
    }
    return true;
}

void lc_rx_endpoint::fill_status(lc_rx_bind_s* out, uint8_t* count)
{
    if (out == nullptr || count == nullptr)
    {
        return;
    }
    *count = 0;
    if (!lock_.ready())
    {
        return;
    }
    bfc::semaphore::lock lock(lock_);
    if (!lock)
    {
        return;
    }
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (ep_[i].used)
        {
            const uint8_t n = *count;
            out[n].bus = ep_[i].bus;
            out[n].dest = ep_[i].dest;
            out[n].active = send_sock_.valid();
            out[n].drop_send_fail =
                ep_[i].drop_send_fail.load(std::memory_order_relaxed);
            *count = static_cast<uint8_t>(n + 1);
        }
    }
}

void lc_rx_endpoint::forward(bus_t bus, packet&& pdu)
{
    entry_s* entries[WIFI_AIRPORT_MAX];
    uint8_t n = 0;
    {
        bfc::semaphore::lock lock(lock_);
        if (!lock)
        {
            return;
        }
        for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
        {
            if (ep_[i].used && ep_[i].bus == bus)
            {
                entries[n++] = &ep_[i];
            }
        }
    }
    if (n == 0 || !pdu.is_valid() || pdu.data() == nullptr)
    {
        return;
    }
    for (uint8_t i = 0; i < n; i++)
    {
        send_one(*entries[i], pdu.data(), pdu.size());
    }
}
