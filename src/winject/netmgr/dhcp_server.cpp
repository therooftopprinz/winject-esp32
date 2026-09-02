#include "dhcp_server.h"

#include "config.h"

#include "dhcpserver/dhcpserver.h"
#include "esp_log.h"

static const char* TAG = "dhcps";

dhcp_server& dhcp_server::instance()
{
    static dhcp_server inst;
    return inst;
}

uint8_t dhcp_server::ip_host(uint32_t addr)
{
    return reinterpret_cast<const uint8_t*>(&addr)[3];
}

uint32_t dhcp_server::ip_with_host(uint32_t base, uint8_t host)
{
    uint32_t addr = base;
    reinterpret_cast<uint8_t*>(&addr)[3] = host;
    return addr;
}

bool dhcp_server::apply_pool(esp_netif_t* netif, uint32_t device_ip)
{
    uint8_t start_host = 0;
    uint8_t end_host = 0;
    pool_hosts(ip_host(device_ip), &start_host, &end_host);
    if (start_host < 1 || end_host > 254 || start_host > end_host)
    {
        return false;
    }

    dhcps_lease_t lease = {};
    lease.enable = true;
    lease.start_ip.addr = ip_with_host(device_ip, start_host);
    lease.end_ip.addr = ip_with_host(device_ip, end_host);
    if (esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease, sizeof(lease)) != ESP_OK)
    {
        ESP_LOGE(TAG, "pool set failed");
        return false;
    }
    return true;
}

void dhcp_server::assigned_ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    auto* self = static_cast<dhcp_server*>(arg);
    const auto* event =
        static_cast<ip_event_assigned_ip_to_client_t*>(event_data);
    if (self == nullptr || event == nullptr)
    {
        return;
    }
    esp_netif_t* netif = self->netif_.load(std::memory_order_relaxed);
    if (event->esp_netif != netif)
    {
        return;
    }
    ESP_LOGI(TAG, "lease " IPSTR " to %02X:%02X:%02X:%02X:%02X:%02X",
             IP2STR(&event->ip), event->mac[0], event->mac[1], event->mac[2],
             event->mac[3], event->mac[4], event->mac[5]);
}

void dhcp_server::bind_netif(esp_netif_t* netif)
{
    netif_.store(netif, std::memory_order_relaxed);
}

void dhcp_server::register_events()
{
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &assigned_ip_event_handler, this));
}

bool dhcp_server::start(esp_netif_t* netif, uint32_t device_ip)
{
    if (netif == nullptr || device_ip == 0)
    {
        return false;
    }

    if (!apply_pool(netif, device_ip))
    {
        return false;
    }

    const esp_err_t err = esp_netif_dhcps_start(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
    {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return false;
    }

    active_.store(true, std::memory_order_relaxed);
    uint8_t lo = 0;
    uint8_t hi = 0;
    pool_hosts(ip_host(device_ip), &lo, &hi);
    ESP_LOGI(TAG, IPSTR "/24  pool .%u-.%u", IP2STR(reinterpret_cast<esp_ip4_addr_t*>(&device_ip)), lo, hi);
    return true;
}

bool dhcp_server::stop(esp_netif_t* netif)
{
    if (netif != nullptr && active_.load(std::memory_order_relaxed))
    {
        const esp_err_t err = esp_netif_dhcps_stop(netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
        {
            ESP_LOGE(TAG, "stop failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    active_.store(false, std::memory_order_relaxed);
    return true;
}

bool dhcp_server::active() const
{
    return active_.load(std::memory_order_relaxed);
}

void dhcp_server::mark_inactive()
{
    active_.store(false, std::memory_order_relaxed);
}

void dhcp_server::pool_hosts(uint8_t device_host, uint8_t* start, uint8_t* end)
{
    uint8_t lo = ETH_FALLBACK_DHCP_HOST_MIN;
    uint8_t hi = ETH_FALLBACK_DHCP_HOST_MAX;
    if (device_host >= lo && device_host <= hi)
    {
        if (device_host - lo >= hi - device_host)
        {
            hi = static_cast<uint8_t>(device_host - 1);
        }
        else
        {
            lo = static_cast<uint8_t>(device_host + 1);
        }
    }
    *start = lo;
    *end = hi;
}

bool dhcp_server::pool_range(uint32_t device_ip, uint32_t* start, uint32_t* end)
{
    if (start == nullptr || end == nullptr || device_ip == 0)
    {
        return false;
    }
    uint8_t lo = 0;
    uint8_t hi = 0;
    pool_hosts(ip_host(device_ip), &lo, &hi);
    *start = ip_with_host(device_ip, lo);
    *end = ip_with_host(device_ip, hi);
    return true;
}
