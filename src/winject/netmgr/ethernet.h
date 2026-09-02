#ifndef WINJECT_ETHERNET_H_
#define WINJECT_ETHERNET_H_

#include <stdint.h>

#include "esp_netif.h"
#include "winject/semaphore.h"

class ethernet
{
public:
    static ethernet& instance();
    ethernet(const ethernet&) = delete;
    ethernet& operator=(const ethernet&) = delete;
    virtual ~ethernet() = default;

    virtual void begin() = 0;
    virtual bool ready() const = 0;
    virtual bool connected() const = 0;
    virtual void set_connected(bool connected) = 0;
    virtual bool using_static() const = 0;
    virtual void set_using_static(bool using_static) = 0;
    virtual bool netif_is_dhcp_server() const = 0;
    virtual esp_netif_t* netif() = 0;
    virtual semaphore& mutex() = 0;

    virtual bool has_ipv4() const = 0;
    virtual bool apply_static_ip(uint32_t ip) = 0;
    virtual bool rebuild_dhcp_client() = 0;
    virtual bool rebuild_dhcp_server(uint32_t ip) = 0;

    virtual bool local_ipv4(uint32_t* out) = 0;
    virtual bool mac(uint8_t mac[6]) = 0;
    virtual bool link_speed_mbps(uint32_t* mbps) = 0;

protected:
    ethernet() = default;
};

#endif  // WINJECT_ETHERNET_H_
