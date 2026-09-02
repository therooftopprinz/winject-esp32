#include "upstream_tx.h"

#include "config.h"
#include "ethernet.h"
#include "frame.h"
#include "wifi.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <utility>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "upstream_tx";

static bool airport_ok(const uint8_t airport[6])
{
    if (airport == nullptr)
    {
        return false;
    }
    if (frameGetMode() == WINJECT_MODE_STANDALONE)
    {
        return frameAirportValidStandalone(airport);
    }
    return frameAirportIsZero(airport);
}

bool upstream_tx::open_send(bfc::socket* out)
{
    if (out == nullptr)
    {
        return false;
    }
    if (!out->open_udp(htonl(INADDR_ANY), 0))
    {
        ESP_LOGE(TAG, "udp send socket failed: %d", errno);
        return false;
    }
    return true;
}

int upstream_tx::find_airport(const uint8_t airport[6]) const
{
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (tx_[i].used && memcmp(tx_[i].airport, airport, 6) == 0)
        {
            return i;
        }
    }
    return -1;
}

int upstream_tx::find_free() const
{
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (!tx_[i].used)
        {
            return i;
        }
    }
    return -1;
}

bool upstream_tx::any_tx() const
{
    for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (tx_[i].used)
        {
            return true;
        }
    }
    return false;
}

void upstream_tx::publish_peer_airports()
{
    size_t n_peer = 0;
    for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (tx_[i].used)
        {
            memcpy(peer_airports_[n_peer], tx_[i].airport, 6);
            n_peer++;
        }
    }
    frameSetPeerAirports(peer_airports_, n_peer);
}

void upstream_tx::close_sockets()
{
    send_sock_.close();
}

bool upstream_tx::ensure_sockets()
{
    if (any_tx() && !send_sock_.valid() && !open_send(&send_sock_))
    {
        return false;
    }
    return true;
}

void upstream_tx::drain_radio()
{
    size_t len = 0;
    while (wifi::instance().pop_rx(buf_, &len, sizeof(buf_)))
    {
    }
}

void upstream_tx::send_payload(const uint8_t* payload, size_t payload_len,
                               uint32_t host, uint16_t port)
{
    if (!send_sock_.valid() || payload == nullptr)
    {
        return;
    }

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = host;
    send_sock_.send(payload, payload_len, 0,
                    reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
}

void upstream_tx::forward_rx()
{
    if (!any_tx())
    {
        drain_radio();
        return;
    }

    const WinjectMode mode = frameGetMode();
    size_t len = 0;
    while (wifi::instance().pop_rx(buf_, &len, sizeof(buf_)))
    {
        const uint8_t* payload = nullptr;
        size_t payload_len = 0;
        if (!frameUnwrap(buf_, len, &payload, &payload_len))
        {
            continue;
        }

        if (mode == WINJECT_MODE_BFC_TUNNEL_DEVICE)
        {
            for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
            {
                if (tx_[i].used)
                {
                    send_payload(payload, payload_len, tx_[i].host,
                                 tx_[i].port);
                }
            }
            continue;
        }

        const uint8_t* sa = buf_ + 10;
        for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
        {
            if (tx_[i].used &&
                frameStandaloneSaMatchesAirport(sa, tx_[i].airport))
            {
                send_payload(payload, payload_len, tx_[i].host, tx_[i].port);
                break;
            }
        }
    }
}

void upstream_tx::run()
{
    for (;;)
    {
        if (eth_ == nullptr || !eth_->connected())
        {
            {
                semaphore::lock lock(lock_);
                close_sockets();
            }
            drain_radio();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        bool sockets_ok = false;
        {
            semaphore::lock lock(lock_);
            sockets_ok = ensure_sockets();
            if (sockets_ok)
            {
                forward_rx();
            }
            else
            {
                drain_radio();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(sockets_ok ? 2 : 50));
    }
}

void upstream_tx::task(void* arg)
{
    static_cast<upstream_tx*>(arg)->run();
}

bool upstream_tx::init(ethernet& eth)
{
    if (ready_)
    {
        return true;
    }
    if (!lock_.init())
    {
        return false;
    }
    eth_ = &eth;
    ready_ = true;
    return true;
}

bool upstream_tx::start_task()
{
    if (xTaskCreatePinnedToCore(task, "upstream_tx", 6144, this,
                                UPSTREAM_TASK_PRIO, nullptr,
                                APP_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "upstream_tx task failed");
        return false;
    }
    return true;
}

bool upstream_tx::load(const upstream_dest_s* tx, uint8_t tx_count)
{
    if (!lock_.ready() || tx_count > WIFI_AIRPORT_MAX ||
        (tx_count > 0 && tx == nullptr))
    {
        return false;
    }

    semaphore::lock lock(lock_);
    close_sockets();
    for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        tx_[i].used = false;
    }
    for (uint8_t i = 0; i < tx_count; i++)
    {
        memcpy(tx_[i].airport, tx[i].airport, 6);
        tx_[i].host = tx[i].host;
        tx_[i].port = tx[i].port;
        tx_[i].used = true;
    }
    publish_peer_airports();
    if (eth_ != nullptr && eth_->connected())
    {
        ensure_sockets();
    }
    return true;
}

void upstream_tx::fill_status(upstream_dest_s* out, uint8_t* count)
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

    semaphore::lock lock(lock_);
    for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (tx_[i].used)
        {
            const uint8_t n = *count;
            memcpy(out[n].airport, tx_[i].airport, 6);
            out[n].host = tx_[i].host;
            out[n].port = tx_[i].port;
            *count = static_cast<uint8_t>(n + 1);
        }
    }
}

bool upstream_tx::set(const uint8_t airport[6], uint32_t host, uint16_t port)
{
    if (port == 0 || host == 0 || host == 0xFFFFFFFFu || !lock_.ready() ||
        !airport_ok(airport))
    {
        return false;
    }

    semaphore::lock lock(lock_);
    int idx = find_airport(airport);
    if (idx < 0)
    {
        idx = find_free();
        if (idx < 0)
        {
            ESP_LOGE(TAG, "tx airport table full");
            return false;
        }
    }
    if (!send_sock_.valid() && !open_send(&send_sock_))
    {
        return false;
    }

    memcpy(tx_[idx].airport, airport, 6);
    tx_[idx].host = host;
    tx_[idx].port = port;
    tx_[idx].used = true;
    publish_peer_airports();

    char ip_str[16];
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&host);
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    ESP_LOGI(TAG, "tx %02X:%02X:%02X:%02X:%02X:%02X %s:%u", airport[0],
             airport[1], airport[2], airport[3], airport[4], airport[5], ip_str,
             port);
    return true;
}
