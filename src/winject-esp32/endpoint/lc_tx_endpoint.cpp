#include "lc_tx_endpoint.h"

#include "config.h"
#include "packet.h"
#include "udp_logger.h"
#include "wifi.h"

#include <errno.h>
#include <string.h>
#include <utility>

#include "esp_log.h"
#include "lwip/sockets.h"

static const char* TAG = "lc_tx_ep";

lc_tx_endpoint& lc_tx_endpoint::instance()
{
    static lc_tx_endpoint inst;
    return inst;
}

bool lc_tx_endpoint::open_bound(uint16_t port, bfc::socket* out)
{
    if (out == nullptr || port == 0)
    {
        return false;
    }
    if (!out->open_udp(htonl(INADDR_ANY), port))
    {
        ESP_LOGE(TAG, "udp bind %u failed: %d", port, errno);
        return false;
    }
    return true;
}

int lc_tx_endpoint::find_bus(bus_t bus) const
{
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (ep[i].used && ep[i].bus == bus)
        {
            return i;
        }
    }
    return -1;
}

int lc_tx_endpoint::find_port(uint16_t port, int except) const
{
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (i != except && ep[i].used && ep[i].udp_port == port)
        {
            return i;
        }
    }
    return -1;
}

int lc_tx_endpoint::find_free() const
{
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (!ep[i].used)
        {
            return i;
        }
    }
    return -1;
}

void lc_tx_endpoint::watch(entry_s& e)
{
    entry_s* const pe = &e;
    reactor.wake_up(
        [this, pe]()
        {
            if (!pe->used || !pe->sock.valid())
            {
                return;
            }
            const int fd = pe->sock.fd();
            reactor.add_read_rdy(fd, [this, pe]() { on_readable(*pe); });
        });
}

void lc_tx_endpoint::unwatch(entry_s& e)
{
    if (!e.sock.valid())
    {
        return;
    }
    reactor.rem_read_rdy(e.sock.fd());
}

void lc_tx_endpoint::clear_slot(entry_s& e)
{
    unwatch(e);
    e.sock.close();
    e.used = false;
    e.bus = 0;
    e.udp_port = 0;
    e.drop_no_pkt_pool.store(0, std::memory_order_relaxed);
    e.drop_queue_full.store(0, std::memory_order_relaxed);
}

bool lc_tx_endpoint::drop_datagram(entry_s& e)
{
    const int recvd = e.sock.recv(drop_buf, sizeof(drop_buf));
    if (recvd <= 0)
    {
        return false;
    }
    wifi::instance().note_udp_tx_pkt();
    e.drop_no_pkt_pool.fetch_add(1, std::memory_order_relaxed);
    udp_logger::instance().log(log_level_e::warn,
                               "tx_drop no_pkt_pool bus=%02X", e.bus);
    return true;
}

void lc_tx_endpoint::on_readable(entry_s& e)
{
    for (;;)
    {
        bus_t bus = 0;
        {
            bfc::semaphore::lock guard(lock);
            if (!guard || !e.used || !e.sock.valid())
            {
                return;
            }
            bus = e.bus;
        }

        packet p = packet_allocator::tx().allocate();
        if (!p.is_valid())
        {
            if (!drop_datagram(e))
            {
                return;
            }
            continue;
        }
        p.set_packet_offset(WIFI_TX_HEADROOM);
        const size_t cap = p.capacity() - p.offset();
        const int recvd = e.sock.recv(p.data(), cap);
        if (recvd <= 0)
        {
            return;
        }
        if (static_cast<size_t>(recvd) > WIFI_PAYLOAD_MAX)
        {
            wifi::instance().note_udp_tx_pkt();
            continue;
        }
        p.set_packet_size(static_cast<size_t>(recvd));
        wifi::instance().note_udp_tx_pkt();
        if (tx == nullptr || !tx->tx(bus, std::move(p)))
        {
            e.drop_queue_full.fetch_add(1, std::memory_order_relaxed);
            udp_logger::instance().log(log_level_e::warn,
                                       "tx_drop queue_full bus=%02X", bus);
            continue;
        }
    }
}

bool lc_tx_endpoint::init(lc_tx& tx_ref)
{
    if (!lock.init())
    {
        return false;
    }
    tx = &tx_ref;
    return true;
}

bool lc_tx_endpoint::start(BaseType_t core, UBaseType_t prio,
                           uint32_t stack_bytes)
{
    if (!reactor.start_pinned("lc_tx_ep", core, prio, stack_bytes))
    {
        return false;
    }
    started = true;
    return true;
}

bool lc_tx_endpoint::add_endpoint(bus_t bus, uint16_t udp_port)
{
    if (udp_port == 0 || !lock.ready())
    {
        return false;
    }

    bfc::semaphore::lock guard(lock);
    if (!guard)
    {
        return false;
    }

    int idx = find_bus(bus);
    if (idx < 0)
    {
        idx = find_free();
        if (idx < 0)
        {
            ESP_LOGE(TAG, "tx bind table full");
            return false;
        }
    }
    if (ep[idx].used && ep[idx].udp_port == udp_port && ep[idx].sock.valid())
    {
        return true;
    }

    const int taken = find_port(udp_port, idx);
    if (taken >= 0)
    {
        ESP_LOGW(TAG, "udp port %u stolen from other bus", udp_port);
        clear_slot(ep[taken]);
    }

    bfc::socket sock;
    if (!open_bound(udp_port, &sock))
    {
        return false;
    }

    if (ep[idx].used)
    {
        unwatch(ep[idx]);
        ep[idx].sock.close();
    }
    ep[idx].sock = std::move(sock);
    ep[idx].bus = bus;
    ep[idx].udp_port = udp_port;
    ep[idx].used = true;
    ep[idx].drop_no_pkt_pool.store(0, std::memory_order_relaxed);
    ep[idx].drop_queue_full.store(0, std::memory_order_relaxed);
    watch(ep[idx]);

    ESP_LOGI(TAG, "sut bus=%02X UDP %u", bus, udp_port);
    return true;
}

bool lc_tx_endpoint::rem_endpoint(bus_t bus)
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
    const int idx = find_bus(bus);
    if (idx < 0)
    {
        return true;
    }
    clear_slot(ep[idx]);
    return true;
}

bool lc_tx_endpoint::clear()
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
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (ep[i].used)
        {
            clear_slot(ep[i]);
        }
    }
    return true;
}

void lc_tx_endpoint::get_status(lc_tx_bind_s* out, uint8_t* count)
{
    if (out == nullptr || count == nullptr)
    {
        return;
    }
    const uint8_t max = *count;
    *count = 0;
    if (max == 0 || !lock.ready())
    {
        return;
    }
    bfc::semaphore::lock guard(lock);
    if (!guard)
    {
        return;
    }
    for (int i = 0; i < WIFI_AIRPORT_MAX; i++)
    {
        if (ep[i].used)
        {
            const uint8_t n = *count;
            if (n >= max)
            {
                break;
            }
            out[n].bus = ep[i].bus;
            out[n].udp_port = ep[i].udp_port;
            out[n].socket_open = ep[i].sock.valid();
            out[n].drop_no_pkt_pool =
                ep[i].drop_no_pkt_pool.load(std::memory_order_relaxed);
            out[n].drop_queue_full =
                ep[i].drop_queue_full.load(std::memory_order_relaxed);
            *count = static_cast<uint8_t>(n + 1);
        }
    }
}
