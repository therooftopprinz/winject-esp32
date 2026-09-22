#include "upstream_rx_endpoint.h"

#include "wifi.h"
#include "wifi_rx.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <utility>

#include "esp_log.h"
#include "lwip/sockets.h"

static const char* TAG = "upstream_rx";

upstream_rx_endpoint& upstream_rx_endpoint::instance()
{
    static upstream_rx_endpoint inst;
    return inst;
}

bool upstream_rx_endpoint::ensure_socket()
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

void upstream_rx_endpoint::send_one(ip_port_t d, const uint8_t* data, size_t len)
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

bool upstream_rx_endpoint::init(wifi_rx& rx_ref)
{
    if (!lock.init())
    {
        return false;
    }
    rx = &rx_ref;
    return true;
}

void upstream_rx_endpoint::run_drain()
{
    for (;;)
    {
        if (rx == nullptr)
        {
            vTaskDelay(1);
            continue;
        }
        packet mpdu = rx->pop(portMAX_DELAY);
        if (!mpdu.is_valid())
        {
            continue;
        }
        if (mpdu.size() < WIFI_RADIO_INJECT_MIN)
        {
            continue;
        }
        rx->on_upstream_deliver();
        if (!dest_active_.load(std::memory_order_acquire))
        {
            continue;
        }
        ip_port_t d = {};
        d.host = dest_host_.load(std::memory_order_relaxed);
        d.port = dest_port_.load(std::memory_order_relaxed);
        send_one(d, mpdu.data(), mpdu.size());
    }
}

void upstream_rx_endpoint::drain_task(void* arg)
{
    static_cast<upstream_rx_endpoint*>(arg)->run_drain();
}

bool upstream_rx_endpoint::start(BaseType_t core, UBaseType_t prio,
                                 uint32_t stack_bytes)
{
    if (started)
    {
        return true;
    }
    if (rx == nullptr)
    {
        ESP_LOGE(TAG, "start without wifi_rx");
        return false;
    }
    if (xTaskCreatePinnedToCore(drain_task, "upstream_rx", stack_bytes, this,
                                prio, &drain_task_handle_, core) != pdPASS)
    {
        ESP_LOGE(TAG, "drain task create failed");
        return false;
    }
    started = true;
    ESP_LOGI(TAG, "air→UDP drain core=%d prio=%u", static_cast<int>(core),
             static_cast<unsigned>(prio));
    return true;
}

bool upstream_rx_endpoint::set_endpoint(ip_port_t d)
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
    dest_host_.store(d.host, std::memory_order_relaxed);
    dest_port_.store(d.port, std::memory_order_relaxed);
    dest_active_.store(true, std::memory_order_release);
    drop_send_fail.store(0, std::memory_order_relaxed);

    char ip_str[16];
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&d.host);
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    ESP_LOGI(TAG, "sur %s:%u", ip_str, d.port);
    return true;
}

bool upstream_rx_endpoint::clear()
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
    dest_host_.store(0, std::memory_order_relaxed);
    dest_port_.store(0, std::memory_order_relaxed);
    dest_active_.store(false, std::memory_order_release);
    drop_send_fail.store(0, std::memory_order_relaxed);
    return true;
}

bool upstream_rx_endpoint::get_status(upstream_rx_bind_s* out)
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
