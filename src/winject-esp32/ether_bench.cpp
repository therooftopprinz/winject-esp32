#include "ether_bench.h"

#include "config.h"
#include "manager.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "ether_bench";

namespace
{
// Keep large payloads off task stacks — heap is tight with WiFi+ETH up.
uint8_t g_tx_buf[ETHER_BENCH_MAX];
uint8_t g_rx_buf[ETHER_BENCH_MAX];

bool set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void write_u16_le(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void write_u32_le(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint16_t read_u16_le(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t read_u32_le(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

ether_bench& ether_bench::instance()
{
    static ether_bench inst;
    return inst;
}

bool ether_bench::init(manager& netmgr)
{
    if (ready_)
    {
        return true;
    }
    netmgr_ = &netmgr;
    reactor_ = &netmgr.reactor();
    reactor_->wake_up(
        [this]()
        {
            attach();
        });
    ready_ = true;
    return true;
}

void ether_bench::attach()
{
    if (!ensure_sock())
    {
        return;
    }
    ESP_LOGI(TAG, "udp bench *:%u (task RX)", ETHER_BENCH_PORT);
}

bool ether_bench::ensure_sock()
{
    if (sock_ >= 0)
    {
        return true;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "socket failed: %d", errno);
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    const int rcvbuf = ETHER_BENCH_SOCK_BUF;
    const int sndbuf = ETHER_BENCH_SOCK_BUF;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ETHER_BENCH_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ESP_LOGE(TAG, "bind *:%u failed: %d", ETHER_BENCH_PORT, errno);
        close(fd);
        return false;
    }
    if (!set_nonblock(fd))
    {
        ESP_LOGE(TAG, "nonblock failed: %d", errno);
        close(fd);
        return false;
    }
    sock_ = fd;
    return true;
}

void ether_bench::close_sock()
{
    if (sock_ < 0)
    {
        return;
    }
    close(sock_);
    sock_ = -1;
}

bool ether_bench::start_tx(ip_port_t dest, uint16_t size, uint32_t count)
{
    if (dest.host == 0 || dest.port == 0)
    {
        return false;
    }
    if (size < ETHER_BENCH_HDR || size > ETHER_BENCH_MAX)
    {
        return false;
    }
    if (tx_running_)
    {
        return false;
    }
    if (!ensure_sock())
    {
        return false;
    }

    tx_dest_ = dest;
    size_ = size;
    tx_target_ = count;
    tx_sent_ = 0;
    tx_fail_ = 0;
    tx_bytes_ = 0;
    tx_elapsed_us_ = 0;
    tx_running_ = true;

    const BaseType_t ok = xTaskCreatePinnedToCore(
        tx_task, "ether_bench_tx", ETHER_BENCH_TASK_STACK, this,
        ETHER_BENCH_TX_TASK_PRIO, nullptr, APP_TASK_CORE);
    if (ok != pdPASS)
    {
        tx_running_ = false;
        ESP_LOGE(TAG, "tx task create failed (heap_free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return false;
    }
    return true;
}

bool ether_bench::arm_rx()
{
    if (!ensure_sock())
    {
        return false;
    }
    while (sock_ >= 0)
    {
        const int n = recv(sock_, g_rx_buf, sizeof(g_rx_buf), 0);
        if (n < 0)
        {
            break;
        }
    }
    rx_ok_ = 0;
    rx_bad_ = 0;
    rx_gap_ = 0;
    rx_bytes_ = 0;
    rx_last_sn_ = 0;
    rx_have_sn_ = false;
    rx_armed_ = true;

    if (!rx_task_running_)
    {
        rx_task_running_ = true;
        const BaseType_t ok = xTaskCreatePinnedToCore(
            rx_task, "ether_bench_rx", ETHER_BENCH_TASK_STACK, this,
            ETHER_BENCH_RX_TASK_PRIO, nullptr, APP_TASK_CORE);
        if (ok != pdPASS)
        {
            rx_armed_ = false;
            rx_task_running_ = false;
            ESP_LOGE(TAG, "rx task create failed (heap_free=%u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
            return false;
        }
    }
    return true;
}

void ether_bench::stop()
{
    rx_armed_ = false;
    tx_running_ = false;
}

void ether_bench::fill_status(ether_bench_status_s* out) const
{
    if (out == nullptr)
    {
        return;
    }
    out->tx_running = tx_running_;
    out->rx_armed = rx_armed_;
    out->size = size_;
    out->tx_sent = tx_sent_;
    out->tx_fail = tx_fail_;
    out->tx_bytes = tx_bytes_;
    out->rx_ok = rx_ok_;
    out->rx_bad = rx_bad_;
    out->rx_gap = rx_gap_;
    out->rx_bytes = rx_bytes_;
    out->rx_last_sn = rx_last_sn_;
    out->rx_have_sn = rx_have_sn_;
    out->tx_elapsed_us = tx_elapsed_us_;
}

void ether_bench::handle_rx_datagram(const uint8_t* buf, int n)
{
    if (n < ETHER_BENCH_HDR)
    {
        ++rx_bad_;
        return;
    }
    const uint16_t magic = read_u16_le(buf);
    const uint16_t size = read_u16_le(buf + 2);
    const uint32_t sn = read_u32_le(buf + 4);
    if (magic != ETHER_BENCH_MAGIC || size != static_cast<uint16_t>(n))
    {
        ++rx_bad_;
        return;
    }
    if (rx_have_sn_)
    {
        if (sn > rx_last_sn_)
        {
            rx_gap_ += sn - rx_last_sn_ - 1;
        }
        else if (sn < rx_last_sn_)
        {
            rx_gap_ += (0xffffffffu - rx_last_sn_) + sn;
        }
    }
    rx_last_sn_ = sn;
    rx_have_sn_ = true;
    ++rx_ok_;
    rx_bytes_ += static_cast<uint64_t>(n);
}

void ether_bench::rx_task(void* arg)
{
    auto* self = static_cast<ether_bench*>(arg);
    ESP_LOGI(TAG, "rx task start");

    // Yield every N datagrams (TX uses 32) so IDLE1 can pet TWDT.
    // Drain until EAGAIN; do not cap batch then sleep — that capped host→device
    // well below wire rate while EMAC/lwIP still had data queued.
    uint32_t since_yield = 0;

    while (true)
    {
        if (!self->rx_armed_)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (!self->rx_armed_)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
                if (!self->rx_armed_)
                {
                    break;
                }
            }
            continue;
        }

        int drained = 0;
        while (self->rx_armed_ && self->sock_ >= 0)
        {
            const int n = recv(self->sock_, g_rx_buf, sizeof(g_rx_buf), 0);
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                {
                    break;
                }
                ESP_LOGW(TAG, "recv failed: %d", errno);
                break;
            }
            self->handle_rx_datagram(g_rx_buf, n);
            ++drained;
            if (++since_yield >= ETHER_BENCH_RX_YIELD_EVERY)
            {
                since_yield = 0;
                vTaskDelay(1);
            }
        }

        if (drained == 0)
        {
            vTaskDelay(1);
        }
    }

    self->rx_task_running_ = false;
    ESP_LOGI(TAG, "rx task exit ok=%lu bad=%lu gap=%lu",
             static_cast<unsigned long>(self->rx_ok_),
             static_cast<unsigned long>(self->rx_bad_),
             static_cast<unsigned long>(self->rx_gap_));
    vTaskDelete(nullptr);
}

void ether_bench::tx_task(void* arg)
{
    auto* self = static_cast<ether_bench*>(arg);
    uint8_t* buf = g_tx_buf;
    const uint16_t size = self->size_;
    memset(buf, 0xA5, size);
    write_u16_le(buf, ETHER_BENCH_MAGIC);
    write_u16_le(buf + 2, size);

    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(self->tx_dest_.port);
    dest.sin_addr.s_addr = self->tx_dest_.host;

    const int64_t t0 = esp_timer_get_time();
    uint32_t sn = 0;
    uint32_t since_yield = 0;
    while (self->tx_running_)
    {
        if (self->tx_target_ != 0 && sn >= self->tx_target_)
        {
            break;
        }
        write_u32_le(buf + 4, sn);
        bool advanced = false;
        while (self->tx_running_ && !advanced)
        {
            const int sent =
                sendto(self->sock_, buf, size, 0,
                       reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
            if (sent == static_cast<int>(size))
            {
                ++self->tx_sent_;
                self->tx_bytes_ += static_cast<uint64_t>(size);
                ++sn;
                advanced = true;
                if (++since_yield >= 32)
                {
                    since_yield = 0;
                    // Let IDLE1 reset the task WDT on long floods.
                    vTaskDelay(1);
                }
                break;
            }
            if (sent < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM))
            {
                ++self->tx_fail_;
                vTaskDelay(1);
                continue;
            }
            ++self->tx_fail_;
            ESP_LOGW(TAG, "sendto failed: %d", errno);
            self->tx_running_ = false;
            break;
        }
    }
    self->tx_elapsed_us_ = esp_timer_get_time() - t0;
    self->tx_running_ = false;
    ESP_LOGI(TAG, "tx done sent=%lu fail=%lu bytes=%llu us=%lld",
             static_cast<unsigned long>(self->tx_sent_),
             static_cast<unsigned long>(self->tx_fail_),
             static_cast<unsigned long long>(self->tx_bytes_),
             static_cast<long long>(self->tx_elapsed_us_));
    vTaskDelete(nullptr);
}
