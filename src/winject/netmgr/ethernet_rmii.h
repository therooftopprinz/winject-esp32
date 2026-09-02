#ifndef WINJECT_ETHERNET_RMII_H_
#define WINJECT_ETHERNET_RMII_H_

#include "ethernet.h"

#include <atomic>

#include "esp_eth.h"
#include "esp_event.h"

class dhcp_client;
class dhcp_server;

class ethernet_rmii : public ethernet
{
public:
    static ethernet_rmii& instance();
    ethernet_rmii(const ethernet_rmii&) = delete;
    ethernet_rmii& operator=(const ethernet_rmii&) = delete;

    void begin() override;
    bool ready() const override;
    bool connected() const override;
    void set_connected(bool connected) override;
    bool using_static() const override;
    void set_using_static(bool using_static) override;
    bool netif_is_dhcp_server() const override;
    esp_netif_t* netif() override;
    semaphore& mutex() override;

    bool has_ipv4() const override;
    bool apply_static_ip(uint32_t ip) override;
    bool rebuild_dhcp_client() override;
    bool rebuild_dhcp_server(uint32_t ip) override;

    bool local_ipv4(uint32_t* out) override;
    bool mac(uint8_t mac[6]) override;
    bool link_speed_mbps(uint32_t* mbps) override;

private:
    static void fill_static_ip(esp_netif_ip_info_t* info, uint32_t ip);
    static void eth_event_handler(void* arg, esp_event_base_t event_base,
                                  int32_t event_id, void* event_data);
    static void got_ip_event_handler(void* arg, esp_event_base_t event_base,
                                     int32_t event_id, void* event_data);

    ethernet_rmii();
    bool destroy_netif();
    bool attach_and_start();

    dhcp_client& dhcp_client_;
    dhcp_server& dhcp_server_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> using_static_{false};
    std::atomic<bool> ready_{false};
    semaphore netif_lock_;
    esp_eth_handle_t eth_handle_ = nullptr;
    esp_eth_netif_glue_handle_t eth_glue_ = nullptr;
    esp_netif_t* eth_netif_ = nullptr;
    bool netif_is_dhcp_server_ = false;
};

#endif  // WINJECT_ETHERNET_RMII_H_
