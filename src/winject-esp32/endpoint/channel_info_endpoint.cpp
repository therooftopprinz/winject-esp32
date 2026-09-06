#include "channel_info_endpoint.h"

#include <errno.h>
#include <string.h>
#include <utility>

#include "esp_log.h"
#include "lwip/sockets.h"

static const char* TAG = "ci_ep";

channel_info_endpoint& channel_info_endpoint::instance()
{
    static channel_info_endpoint inst;
    return inst;
}

int channel_info_endpoint::find_sub(ip_port_t subscriber) const
{
    for (uint8_t i = 0; i < n_subs; i++)
    {
        if (ip_port_eq(subs[i], subscriber))
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool channel_info_endpoint::ensure_socket()
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

bool channel_info_endpoint::send_one(const ip_port_t& dest, const void* data,
                                     size_t len)
{
    if (!ensure_socket() || data == nullptr || len == 0 || dest.port == 0)
    {
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = dest.host;
    addr.sin_port = htons(dest.port);
    const ssize_t n = send_sock.send(data, len, 0,
                                      reinterpret_cast<const sockaddr*>(&addr),
                                      sizeof(addr));
    return n == static_cast<ssize_t>(len);
}

void channel_info_endpoint::fanout(const void* data, size_t len)
{
    ip_port_t copy[k_subscriber_max];
    uint8_t n = 0;
    {
        bfc::semaphore::lock lock(lock);
        if (!lock)
        {
            return;
        }
        n = n_subs;
        memcpy(copy, subs, n * sizeof(ip_port_t));
    }
    for (uint8_t i = 0; i < n; i++)
    {
        send_one(copy[i], data, len);
    }
}

void channel_info_endpoint::on_rx_air_timer()
{
    if (air_valid.load(std::memory_order_relaxed))
    {
        rx_air_info_s sample = {};
        sample.info_type = E_CHANNEL_INFO_TYPE_RX_AIR;
        sample.rssi = rssi.load(std::memory_order_relaxed);
        sample.snr = snr.load(std::memory_order_relaxed);
        fanout(&sample, sizeof(sample));
    }
    reactor.get_timer().wait_ms(k_rx_air_interval_ms,
                                 [this]()
                                 {
                                     on_rx_air_timer();
                                 });
}

bool channel_info_endpoint::init()
{
    return lock.init();
}

bool channel_info_endpoint::start(BaseType_t core, UBaseType_t prio,
                                  uint32_t stack_bytes)
{
    if (!reactor.start_pinned("ci_ep", core, prio, stack_bytes))
    {
        return false;
    }
    reactor.get_timer().wait_ms(k_rx_air_interval_ms,
                                 [this]()
                                 {
                                     on_rx_air_timer();
                                 });
    reactor.wake_up();
    return true;
}

bool channel_info_endpoint::add_subscriber(ip_port_t subscriber)
{
    if (subscriber.port == 0 || subscriber.host == 0 ||
        subscriber.host == 0xFFFFFFFFu || !lock.ready())
    {
        return false;
    }
    bfc::semaphore::lock lock(lock);
    if (!lock)
    {
        return false;
    }
    if (find_sub(subscriber) >= 0)
    {
        return true;
    }
    if (n_subs >= k_subscriber_max)
    {
        return false;
    }
    subs[n_subs++] = subscriber;
    return true;
}

bool channel_info_endpoint::rem_subscriber(ip_port_t subscriber)
{
    if (!lock.ready())
    {
        return false;
    }
    bfc::semaphore::lock lock(lock);
    if (!lock)
    {
        return false;
    }
    const int idx = find_sub(subscriber);
    if (idx < 0)
    {
        return false;
    }
    for (uint8_t i = static_cast<uint8_t>(idx); i + 1 < n_subs; i++)
    {
        subs[i] = subs[i + 1];
    }
    n_subs--;
    subs[n_subs] = {};
    return true;
}

bool channel_info_endpoint::load(const ip_port_t* subs, uint8_t count)
{
    if (!lock.ready() || count > k_subscriber_max ||
        (count > 0 && subs == nullptr))
    {
        return false;
    }
    bfc::semaphore::lock lock(lock);
    if (!lock)
    {
        return false;
    }
    n_subs = count;
    if (count > 0)
    {
        memcpy(subs, subs, count * sizeof(ip_port_t));
    }
    for (uint8_t i = count; i < k_subscriber_max; i++)
    {
        subs[i] = {};
    }
    return true;
}

void channel_info_endpoint::fill_status(ip_port_t* out, uint8_t* count)
{
    if (out == nullptr || count == nullptr)
    {
        return;
    }
    *count = 0;
    if (!lock.ready())
    {
        return;
    }
    bfc::semaphore::lock lock(lock);
    if (!lock)
    {
        return;
    }
    *count = n_subs;
    if (n_subs > 0)
    {
        memcpy(out, subs, n_subs * sizeof(ip_port_t));
    }
}

void channel_info_endpoint::on_rx_air_info(int8_t rssi, int8_t snr)
{
    rssi.store(rssi, std::memory_order_relaxed);
    snr.store(snr, std::memory_order_relaxed);
    air_valid.store(true, std::memory_order_relaxed);
}

void channel_info_endpoint::on_flow_ctrl_info(uint8_t queue_size,
                                             uint8_t queue_cap)
{
    tx_flow_ctrl_s sample = {};
    sample.info_type = E_CHANNEL_INFO_TYPE_FLOW_CTRL;
    sample.tx_queue_size = queue_size;
    sample.tx_queue_capacity = queue_cap;
    fanout(&sample, sizeof(sample));
}
