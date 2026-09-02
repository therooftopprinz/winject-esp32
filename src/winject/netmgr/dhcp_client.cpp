#include "dhcp_client.h"

#include "esp_log.h"

static const char* TAG = "dhcpc";

dhcp_client& dhcp_client::instance()
{
    static dhcp_client inst;
    return inst;
}

bool dhcp_client::start(esp_netif_t* netif)
{
    if (netif == nullptr)
    {
        return false;
    }
    const esp_err_t err = esp_netif_dhcpc_start(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
    {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool dhcp_client::stop(esp_netif_t* netif)
{
    if (netif == nullptr)
    {
        return true;
    }
    const esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    {
        ESP_LOGE(TAG, "stop failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
