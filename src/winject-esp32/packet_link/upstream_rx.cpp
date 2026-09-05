#include "upstream_rx.h"

#include "config.h"
#include "ethernet.h"
#include "frame.h"
#include "wifi.h"

#include <errno.h>
#include <string.h>
#include <utility>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "upstream_rx";

upstream_rx& upstream_rx::instance()
{
    static upstream_rx inst;
    return inst;
}

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

bool upstream_rx::open_bound(uint16_t port, bfc::socket* out)
{
    uint32_t ip = 0;
    if (out == nullptr)
    {
        return false;
    }
    if (eth_ == nullptr || !eth_->local_ipv4(&ip))
    {
        ESP_LOGE(TAG, "udp: ethernet has no ipv4");
        return false;
    }
    if (!out->open_udp(ip, port))
    {
        ESP_LOGE(TAG, "udp bind %u failed: %d", port, errno);
        return false;
    }
    return true;
}

int upstream_rx::find_airport(const uint8_t airport[6]) const
{
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (rx_[i].used && memcmp(rx_[i].airport, airport, 6) == 0)
        {
            return i;
        }
    }
    return -1;
}

int upstream_rx::find_port(uint16_t port, int except) const
{
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (i != except && rx_[i].used && rx_[i].port == port)
        {
            return i;
        }
    }
    return -1;
}

void upstream_rx::clear_slot(int idx)
{
    rx_[idx].sock.close();
    rx_[idx].used = false;
    rx_[idx].port = 0;
    memset(rx_[idx].airport, 0, sizeof(rx_[idx].airport));
}

int upstream_rx::find_free() const
{
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (!rx_[i].used)
        {
            return i;
        }
    }
    return -1;
}

void upstream_rx::publish_local_airports()
{
    uint8_t local_airports[WIFI_AIRPORT_MAX][6];
    size_t n_local = 0;
    for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (rx_[i].used)
        {
            memcpy(local_airports[n_local], rx_[i].airport, 6);
            n_local++;
        }
    }
    frameSetLocalAirports(local_airports, n_local);
}

void upstream_rx::close_sockets()
{
    for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        rx_[i].sock.close();
    }
}

bool upstream_rx::ensure_sockets()
{
    for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (!rx_[i].used || rx_[i].sock.valid())
        {
            continue;
        }
        if (!open_bound(rx_[i].port, &rx_[i].sock))
        {
            return false;
        }
    }
    return true;
}

void upstream_rx::inject_one(bfc::socket& sock, const uint8_t sa[6])
{
    wifi& radio = wifi::instance();
    for (;;)
    {
        const int n = sock.recv(buf_, sizeof(buf_));
        if (n < 0)
        {
            break;
        }
        if (n == 0)
        {
            continue;
        }
        radio.note_udp_tx_pkt();
        if (n > static_cast<int>(WIFI_PAYLOAD_MAX))
        {
            continue;
        }
        // Non-blocking: never sleep on the TX pool while holding lock_ (that
        // deadlocks the console reactor in fill_status / set_upstream_*).
        wifi::tx_slot_s* slot = radio.take_tx(0);
        if (slot == nullptr)
        {
            // Datagram already consumed from the UDP socket; drop under
            // backpressure and stop so we release lock_ promptly.
            break;
        }
        const size_t framed = frameWrap(buf_, static_cast<size_t>(n), sa,
                                        slot->data, sizeof(slot->data));
        if (framed == 0)
        {
            radio.release_tx(slot);
            continue;
        }
        slot->len = static_cast<uint16_t>(framed);
        if (!radio.post_tx(slot, 0))
        {
            continue;
        }
    }
}

void upstream_rx::inject_pending()
{
    const bool standalone = frameGetMode() == WINJECT_MODE_STANDALONE;
    for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (!rx_[i].used || !rx_[i].sock.valid())
        {
            continue;
        }
        inject_one(rx_[i].sock, standalone ? rx_[i].airport : nullptr);
    }
}

void upstream_rx::run()
{
    for (;;)
    {
        if (eth_ == nullptr || !eth_->connected())
        {
            {
                bfc::semaphore::lock lock(lock_);
                close_sockets();
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int maxfd = -1;
        fd_set rfds;
        bool sockets_ok = false;
        {
            bfc::semaphore::lock lock(lock_);
            sockets_ok = ensure_sockets();
            if (sockets_ok)
            {
                FD_ZERO(&rfds);
                for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
                {
                    const int fd = rx_[i].sock.fd();
                    if (!rx_[i].used || fd < 0)
                    {
                        continue;
                    }
                    FD_SET(fd, &rfds);
                    if (fd > maxfd)
                    {
                        maxfd = fd;
                    }
                }
            }
        }
        if (!sockets_ok)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (maxfd >= 0)
        {
            struct timeval tv = {};
            tv.tv_usec = 2000;
            select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        // Hold lock_ only for the inject pass; take_tx/post_tx are non-blocking
        // so a full WiFi TX pool cannot pin this lock (and the console) forever.
        {
            bfc::semaphore::lock lock(lock_);
            inject_pending();
        }
    }
}

void upstream_rx::task(void* arg)
{
    static_cast<upstream_rx*>(arg)->run();
}

bool upstream_rx::init(ethernet& eth)
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

bool upstream_rx::start_task()
{
    if (xTaskCreatePinnedToCore(task, "upstream_rx", 6144, this,
                                UPSTREAM_TASK_PRIO, nullptr,
                                APP_TASK_CORE) != pdPASS)
    {
        ESP_LOGE(TAG, "upstream_rx task failed");
        return false;
    }
    return true;
}

bool upstream_rx::load(const upstream_bind_s* rx, uint8_t rx_count)
{
    if (!lock_.ready() || rx_count > WIFI_AIRPORT_MAX ||
        (rx_count > 0 && rx == nullptr))
    {
        return false;
    }

    bfc::semaphore::lock lock(lock_);
    close_sockets();
    for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        rx_[i].used = false;
    }
    for (uint8_t i = 0; i < rx_count; i++)
    {
        memcpy(rx_[i].airport, rx[i].airport, 6);
        rx_[i].port = rx[i].port;
        rx_[i].used = true;
    }
    publish_local_airports();
    if (eth_ != nullptr && eth_->connected())
    {
        ensure_sockets();
    }
    return true;
}

void upstream_rx::fill_status(upstream_bind_s* out, uint8_t* count)
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
    for (size_t i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (rx_[i].used)
        {
            const uint8_t n = *count;
            memcpy(out[n].airport, rx_[i].airport, 6);
            out[n].port = rx_[i].port;
            out[n].socket_open = rx_[i].sock.valid();
            *count = static_cast<uint8_t>(n + 1);
        }
    }
}

bool upstream_rx::set(const uint8_t airport[6], uint16_t port)
{
    if (port == 0 || !lock_.ready() || !airport_ok(airport))
    {
        return false;
    }

    bfc::semaphore::lock lock(lock_);
    int idx = find_airport(airport);
    if (idx < 0)
    {
        idx = find_free();
        if (idx < 0)
        {
            ESP_LOGE(TAG, "rx airport table full");
            return false;
        }
    }
    if (rx_[idx].used && rx_[idx].port == port && rx_[idx].sock.valid())
    {
        return true;
    }
    const int taken = find_port(port, idx);
    if (taken >= 0)
    {
        ESP_LOGW(TAG, "udp port %u stolen from other airport", port);
        clear_slot(taken);
    }

    bfc::socket sock;
    if (!open_bound(port, &sock))
    {
        return false;
    }

    rx_[idx].sock = std::move(sock);
    memcpy(rx_[idx].airport, airport, 6);
    rx_[idx].port = port;
    rx_[idx].used = true;
    publish_local_airports();

    ESP_LOGI(TAG, "rx %02X:%02X:%02X:%02X:%02X:%02X UDP %u", airport[0],
             airport[1], airport[2], airport[3], airport[4], airport[5], port);
    return true;
}

bool upstream_rx::unset(const uint8_t airport[6])
{
    if (airport == nullptr || !lock_.ready() || !airport_ok(airport))
    {
        return false;
    }

    bfc::semaphore::lock lock(lock_);
    int idx = find_airport(airport);
    if (idx < 0)
    {
        return true;  // already unset
    }

    clear_slot(idx);
    publish_local_airports();
    return true;
}
