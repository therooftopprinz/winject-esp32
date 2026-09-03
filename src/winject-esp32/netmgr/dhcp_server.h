#ifndef WINJECT_DHCP_SERVER_H_
#define WINJECT_DHCP_SERVER_H_

#include <atomic>
#include <stdint.h>

#include "esp_event.h"
#include "esp_netif.h"

class dhcp_server
{
public:
    static dhcp_server& instance();
    dhcp_server(const dhcp_server&) = delete;
    dhcp_server& operator=(const dhcp_server&) = delete;

    void bind_netif(esp_netif_t* netif);
    void register_events();

    bool start(esp_netif_t* netif, uint32_t device_ip);
    bool stop(esp_netif_t* netif);
    bool active() const;
    void mark_inactive();

    static bool pool_range(uint32_t device_ip, uint32_t* start, uint32_t* end);

private:
    static uint8_t ip_host(uint32_t addr);
    static uint32_t ip_with_host(uint32_t base, uint8_t host);
    static void pool_hosts(uint8_t device_host, uint8_t* start, uint8_t* end);
    static void assigned_ip_event_handler(void* arg,
                                          esp_event_base_t event_base,
                                          int32_t event_id, void* event_data);

    dhcp_server() = default;
    bool apply_pool(esp_netif_t* netif, uint32_t device_ip);

    std::atomic<bool> active_{false};
    std::atomic<esp_netif_t*> netif_{nullptr};
};

#endif  // WINJECT_DHCP_SERVER_H_
