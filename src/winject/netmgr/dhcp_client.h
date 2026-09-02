#ifndef WINJECT_DHCP_CLIENT_H_
#define WINJECT_DHCP_CLIENT_H_

#include "esp_netif.h"

class dhcp_client
{
public:
    static dhcp_client& instance();
    dhcp_client(const dhcp_client&) = delete;
    dhcp_client& operator=(const dhcp_client&) = delete;

    bool start(esp_netif_t* netif);
    bool stop(esp_netif_t* netif);

private:
    dhcp_client() = default;
};

#endif  // WINJECT_DHCP_CLIENT_H_
