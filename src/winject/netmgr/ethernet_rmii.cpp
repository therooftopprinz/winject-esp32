#include "ethernet_rmii.h"

#include "config.h"
#include "dhcp_client.h"
#include "dhcp_server.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_eth_mac.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "eth";

ethernet& ethernet::instance()
{
    return ethernet_rmii::instance();
}

ethernet_rmii& ethernet_rmii::instance()
{
    static ethernet_rmii inst;
    return inst;
}

ethernet_rmii::ethernet_rmii()
    : dhcp_client_(dhcp_client::instance()),
      dhcp_server_(dhcp_server::instance())
{
}

void ethernet_rmii::fill_static_ip(esp_netif_ip_info_t* info, uint32_t ip)
{
    memset(info, 0, sizeof(*info));
    info->ip.addr = ip;
    esp_netif_set_ip4_addr(&info->netmask, 255, 255, 255, 0);
    info->gw.addr = info->ip.addr;
}

bool ethernet_rmii::destroy_netif()
{
    if (eth_handle_ == nullptr)
    {
        return false;
    }
    dhcp_server_.mark_inactive();
    dhcp_server_.bind_netif(nullptr);
    if (esp_eth_stop(eth_handle_) != ESP_OK)
    {
        ESP_LOGE(TAG, "ETH stop failed");
        return false;
    }
    if (eth_glue_ != nullptr)
    {
        if (esp_eth_del_netif_glue(eth_glue_) != ESP_OK)
        {
            ESP_LOGE(TAG, "ETH glue delete failed");
            return false;
        }
        eth_glue_ = nullptr;
    }
    if (eth_netif_ != nullptr)
    {
        esp_netif_destroy(eth_netif_);
        eth_netif_ = nullptr;
    }
    netif_is_dhcp_server_ = false;
    return true;
}

bool ethernet_rmii::attach_and_start()
{
    if (eth_netif_ == nullptr || eth_handle_ == nullptr)
    {
        return false;
    }
    if (esp_netif_set_hostname(eth_netif_, DEVICE_HOSTNAME) != ESP_OK)
    {
        ESP_LOGW(TAG, "ETH hostname set failed");
    }
    eth_glue_ = esp_eth_new_netif_glue(eth_handle_);
    if (eth_glue_ == nullptr)
    {
        ESP_LOGE(TAG, "ETH glue alloc failed");
        return false;
    }
    if (esp_netif_attach(eth_netif_, eth_glue_) != ESP_OK)
    {
        ESP_LOGE(TAG, "ETH attach failed");
        return false;
    }
    dhcp_server_.bind_netif(eth_netif_);
    if (esp_eth_start(eth_handle_) != ESP_OK)
    {
        ESP_LOGE(TAG, "ETH start failed");
        return false;
    }
    return true;
}

void ethernet_rmii::eth_event_handler(void* arg, esp_event_base_t event_base,
                                      int32_t event_id, void* event_data)
{
    (void)event_base;
    (void)event_data;
    auto* self = static_cast<ethernet_rmii*>(arg);
    if (self == nullptr)
    {
        return;
    }
    switch (event_id)
    {
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "ETH started");
            break;
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "ETH link up");
            if (self->netif_is_dhcp_server_ && self->eth_netif_ != nullptr)
            {
                self->connected_.store(true, std::memory_order_relaxed);
                esp_netif_ip_info_t info = {};
                uint32_t ip = 0;
                if (esp_netif_get_ip_info(self->eth_netif_, &info) == ESP_OK)
                {
                    ip = info.ip.addr;
                }
                if (ip != 0)
                {
                    self->dhcp_server_.start(self->eth_netif_, ip);
                }
            }
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "ETH link down");
            if (!self->using_static_.load(std::memory_order_relaxed) &&
                !self->netif_is_dhcp_server_)
            {
                self->connected_.store(false, std::memory_order_relaxed);
            }
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "ETH stopped");
            if (!self->using_static_.load(std::memory_order_relaxed) &&
                !self->netif_is_dhcp_server_)
            {
                self->connected_.store(false, std::memory_order_relaxed);
            }
            break;
        default:
            break;
    }
}

void ethernet_rmii::got_ip_event_handler(void* arg, esp_event_base_t event_base,
                                         int32_t event_id, void* event_data)
{
    (void)event_base;
    (void)event_id;
    auto* self = static_cast<ethernet_rmii*>(arg);
    const auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    if (self == nullptr || event == nullptr ||
        event->esp_netif != self->eth_netif_)
    {
        return;
    }
    self->connected_.store(true, std::memory_order_relaxed);

    uint8_t mac[6] = {};
    self->mac(mac);
    ESP_LOGI(TAG, "ETH MAC: %02X:%02X:%02X:%02X:%02X:%02X  IP: " IPSTR, mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5],
             IP2STR(&event->ip_info.ip));
}

void ethernet_rmii::begin()
{
    netif_lock_.init();
    uint8_t base_mac[6] = {};
    if (esp_read_mac(base_mac, ESP_MAC_WIFI_STA) == ESP_OK)
    {
        ESP_LOGI(TAG, "factory STA MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 base_mac[0], base_mac[1], base_mac[2], base_mac[3],
                 base_mac[4], base_mac[5]);
    }

    gpio_config_t power = {};
    power.pin_bit_mask = 1ULL << ETH_PHY_POWER;
    power.mode = GPIO_MODE_OUTPUT;
    gpio_config(&power);
    gpio_set_level(static_cast<gpio_num_t>(ETH_PHY_POWER), 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif_ = esp_netif_new(&netif_cfg);
    if (eth_netif_ == nullptr)
    {
        ESP_LOGE(TAG, "ETH netif alloc failed");
        return;
    }
    ESP_ERROR_CHECK(esp_netif_set_hostname(eth_netif_, DEVICE_HOSTNAME));
    dhcp_server_.bind_netif(eth_netif_);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    emac_config.smi_gpio.mdc_num = ETH_PHY_MDC;
    emac_config.smi_gpio.mdio_num = ETH_PHY_MDIO;
#else
    emac_config.smi_mdc_gpio_num = ETH_PHY_MDC;
    emac_config.smi_mdio_gpio_num = ETH_PHY_MDIO;
#endif
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
#if ETH_CLK_MODE == ETH_CLK_GPIO17_OUT
    // GPIO17 is LED4 (WiFi TX activity); clock-out mode leaves that LED unused.
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
    emac_config.clock_config.rmii.clock_gpio = 17;
#else
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = 0;
#endif

    esp_eth_mac_t* eth_mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = -1;
    esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phy_config);
    if (eth_mac == nullptr || phy == nullptr)
    {
        ESP_LOGE(TAG, "ETH MAC/PHY alloc failed");
        return;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(eth_mac, phy);
    if (esp_eth_driver_install(&eth_config, &eth_handle_) != ESP_OK)
    {
        ESP_LOGE(TAG, "ETH driver install failed");
        return;
    }

    eth_glue_ = esp_eth_new_netif_glue(eth_handle_);
    if (eth_glue_ == nullptr)
    {
        ESP_LOGE(TAG, "ETH glue alloc failed");
        return;
    }
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif_, eth_glue_));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               &eth_event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &got_ip_event_handler, this));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle_));
    ready_.store(true, std::memory_order_relaxed);
}

bool ethernet_rmii::ready() const
{
    return ready_.load(std::memory_order_relaxed) && eth_handle_ != nullptr &&
           eth_netif_ != nullptr;
}

bool ethernet_rmii::connected() const
{
    return connected_.load(std::memory_order_relaxed);
}

void ethernet_rmii::set_connected(bool connected)
{
    connected_.store(connected, std::memory_order_relaxed);
}

bool ethernet_rmii::using_static() const
{
    return using_static_.load(std::memory_order_relaxed);
}

void ethernet_rmii::set_using_static(bool using_static)
{
    using_static_.store(using_static, std::memory_order_relaxed);
}

bool ethernet_rmii::netif_is_dhcp_server() const
{
    return netif_is_dhcp_server_;
}

esp_netif_t* ethernet_rmii::netif()
{
    return eth_netif_;
}

semaphore& ethernet_rmii::mutex()
{
    return netif_lock_;
}

bool ethernet_rmii::has_ipv4() const
{
    if (eth_netif_ == nullptr)
    {
        return false;
    }
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(eth_netif_, &info) != ESP_OK || info.ip.addr == 0)
    {
        return false;
    }
    return true;
}

bool ethernet_rmii::apply_static_ip(uint32_t ip)
{
    if (eth_netif_ == nullptr)
    {
        return false;
    }
    if (!netif_is_dhcp_server_ && !dhcp_client_.stop(eth_netif_))
    {
        return false;
    }
    esp_netif_ip_info_t info = {};
    fill_static_ip(&info, ip);
    if (esp_netif_set_ip_info(eth_netif_, &info) != ESP_OK)
    {
        ESP_LOGE(TAG, "ETH static IP set failed");
        return false;
    }
    using_static_.store(true, std::memory_order_relaxed);
    connected_.store(true, std::memory_order_relaxed);
    ESP_LOGI(TAG, "ETH static " IPSTR "/24", IP2STR(&info.ip));
    return true;
}

bool ethernet_rmii::rebuild_dhcp_client()
{
    if (!destroy_netif())
    {
        return false;
    }
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif_ = esp_netif_new(&netif_cfg);
    if (eth_netif_ == nullptr)
    {
        ESP_LOGE(TAG, "ETH netif alloc failed");
        return false;
    }
    netif_is_dhcp_server_ = false;
    return attach_and_start();
}

bool ethernet_rmii::rebuild_dhcp_server(uint32_t ip)
{
    if (!destroy_netif())
    {
        return false;
    }
    esp_netif_ip_info_t ip_info = {};
    fill_static_ip(&ip_info, ip);

    esp_netif_inherent_config_t inherent = ESP_NETIF_INHERENT_DEFAULT_ETH();
    inherent.flags = static_cast<esp_netif_flags_t>(
        ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP |
        ESP_NETIF_FLAG_EVENT_IP_MODIFIED | ESP_NETIF_DEFAULT_ARP_FLAGS);
    inherent.ip_info = &ip_info;

    const esp_netif_config_t netif_cfg = {
        .base = &inherent,
        .driver = nullptr,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    eth_netif_ = esp_netif_new(&netif_cfg);
    if (eth_netif_ == nullptr)
    {
        ESP_LOGE(TAG, "ETH dhcps netif alloc failed");
        return false;
    }
    netif_is_dhcp_server_ = true;
    if (!attach_and_start())
    {
        return false;
    }
    using_static_.store(true, std::memory_order_relaxed);
    connected_.store(true, std::memory_order_relaxed);
    return dhcp_server_.start(eth_netif_, ip);
}

bool ethernet_rmii::local_ipv4(uint32_t* out)
{
    if (out == nullptr || !connected_.load(std::memory_order_relaxed))
    {
        return false;
    }
    semaphore::lock lock(netif_lock_);
    if (!lock)
    {
        return false;
    }
    if (eth_netif_ == nullptr)
    {
        return false;
    }
    esp_netif_ip_info_t info = {};
    const bool ok =
        esp_netif_get_ip_info(eth_netif_, &info) == ESP_OK && info.ip.addr != 0;
    if (ok)
    {
        *out = info.ip.addr;
    }
    return ok;
}

bool ethernet_rmii::mac(uint8_t mac[6])
{
    if (mac == nullptr)
    {
        return false;
    }
    {
        semaphore::lock lock(netif_lock_);
        if (lock)
        {
            const bool ok =
                eth_handle_ != nullptr &&
                esp_eth_ioctl(eth_handle_, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK;
            if (ok)
            {
                return true;
            }
        }
    }
    return esp_read_mac(mac, ESP_MAC_ETH) == ESP_OK;
}

bool ethernet_rmii::link_speed_mbps(uint32_t* mbps)
{
    if (mbps == nullptr)
    {
        return false;
    }
    semaphore::lock lock(netif_lock_);
    if (!lock)
    {
        return false;
    }
    if (eth_handle_ == nullptr)
    {
        return false;
    }
    eth_speed_t speed = ETH_SPEED_10M;
    const bool ok =
        esp_eth_ioctl(eth_handle_, ETH_CMD_G_SPEED, &speed) == ESP_OK;
    if (ok)
    {
        *mbps = (speed == ETH_SPEED_100M) ? 100 : 10;
    }
    return ok;
}
