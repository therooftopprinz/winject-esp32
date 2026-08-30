#include "ethernet.h"

#include "config.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "eth";

static volatile bool g_ethConnected = false;
static esp_eth_handle_t g_ethHandle = nullptr;
static esp_netif_t* g_ethNetif = nullptr;

static void ethEventHandler(void* arg, esp_event_base_t eventBase,
                            int32_t eventId, void* eventData)
{
  (void)arg;
  (void)eventBase;
  (void)eventData;
  switch (eventId)
  {
    case ETHERNET_EVENT_START:
      ESP_LOGI(TAG, "ETH started");
      break;
    case ETHERNET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "ETH link up");
      break;
    case ETHERNET_EVENT_DISCONNECTED:
      g_ethConnected = false;
      ESP_LOGI(TAG, "ETH link down");
      break;
    case ETHERNET_EVENT_STOP:
      g_ethConnected = false;
      ESP_LOGI(TAG, "ETH stopped");
      break;
    default:
      break;
  }
}

static void gotIpEventHandler(void* arg, esp_event_base_t eventBase,
                              int32_t eventId, void* eventData)
{
  (void)arg;
  (void)eventBase;
  (void)eventId;
  const auto* event = static_cast<ip_event_got_ip_t*>(eventData);
  if (event == nullptr || event->esp_netif != g_ethNetif)
  {
    return;
  }
  g_ethConnected = true;

  uint8_t mac[6] = {};
  ethernetMac(mac);
  ESP_LOGI(TAG, "ETH MAC: %02X:%02X:%02X:%02X:%02X:%02X  IP: " IPSTR, mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5], IP2STR(&event->ip_info.ip));
}

void ethernetBegin()
{
  // Chip-unique eFuse MAC. Do not override: one firmware image is flashed to
  // multiple radios, and a shared base MAC collapses Ethernet and 802.11 SA.
  uint8_t baseMac[6] = {};
  if (esp_read_mac(baseMac, ESP_MAC_WIFI_STA) == ESP_OK)
  {
    ESP_LOGI(TAG, "factory STA MAC %02X:%02X:%02X:%02X:%02X:%02X", baseMac[0],
             baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
  }

  gpio_config_t power = {};
  power.pin_bit_mask = 1ULL << ETH_PHY_POWER;
  power.mode = GPIO_MODE_OUTPUT;
  gpio_config(&power);
  gpio_set_level(static_cast<gpio_num_t>(ETH_PHY_POWER), 1);
  vTaskDelay(pdMS_TO_TICKS(50));

  esp_netif_config_t netifCfg = ESP_NETIF_DEFAULT_ETH();
  g_ethNetif = esp_netif_new(&netifCfg);
  if (g_ethNetif == nullptr)
  {
    ESP_LOGE(TAG, "ETH netif alloc failed");
    return;
  }
  ESP_ERROR_CHECK(esp_netif_set_hostname(g_ethNetif, DEVICE_HOSTNAME));

  eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
  eth_esp32_emac_config_t emacConfig = ETH_ESP32_EMAC_DEFAULT_CONFIG();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
  emacConfig.smi_gpio.mdc_num = ETH_PHY_MDC;
  emacConfig.smi_gpio.mdio_num = ETH_PHY_MDIO;
#else
  emacConfig.smi_mdc_gpio_num = ETH_PHY_MDC;
  emacConfig.smi_mdio_gpio_num = ETH_PHY_MDIO;
#endif
  emacConfig.interface = EMAC_DATA_INTERFACE_RMII;
#if ETH_CLK_MODE == ETH_CLK_GPIO17_OUT
  emacConfig.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
  emacConfig.clock_config.rmii.clock_gpio = 17;
#else
  emacConfig.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
  emacConfig.clock_config.rmii.clock_gpio = 0;
#endif

  esp_eth_mac_t* ethMac = esp_eth_mac_new_esp32(&emacConfig, &macConfig);
  eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
  phyConfig.phy_addr = ETH_PHY_ADDR;
  phyConfig.reset_gpio_num = -1;
  esp_eth_phy_t* phy = esp_eth_phy_new_generic(&phyConfig);
  if (ethMac == nullptr || phy == nullptr)
  {
    ESP_LOGE(TAG, "ETH MAC/PHY alloc failed");
    return;
  }

  esp_eth_config_t ethConfig = ETH_DEFAULT_CONFIG(ethMac, phy);
  if (esp_eth_driver_install(&ethConfig, &g_ethHandle) != ESP_OK)
  {
    ESP_LOGE(TAG, "ETH driver install failed");
    return;
  }

  ESP_ERROR_CHECK(
      esp_netif_attach(g_ethNetif, esp_eth_new_netif_glue(g_ethHandle)));
  ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                             &ethEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                             &gotIpEventHandler, nullptr));
  ESP_ERROR_CHECK(esp_eth_start(g_ethHandle));
}

bool ethernetConnected()
{
  return g_ethConnected;
}

bool ethernetLocalIpv4(uint32_t* out)
{
  if (out == nullptr || !g_ethConnected || g_ethNetif == nullptr)
  {
    return false;
  }
  esp_netif_ip_info_t info = {};
  if (esp_netif_get_ip_info(g_ethNetif, &info) != ESP_OK || info.ip.addr == 0)
  {
    return false;
  }
  *out = info.ip.addr;
  return true;
}

bool ethernetMac(uint8_t mac[6])
{
  if (mac == nullptr)
  {
    return false;
  }
  if (g_ethHandle != nullptr &&
      esp_eth_ioctl(g_ethHandle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK)
  {
    return true;
  }
  return esp_read_mac(mac, ESP_MAC_ETH) == ESP_OK;
}

bool ethernetLinkSpeedMbps(uint32_t* mbps)
{
  if (mbps == nullptr || g_ethHandle == nullptr)
  {
    return false;
  }
  eth_speed_t speed = ETH_SPEED_10M;
  if (esp_eth_ioctl(g_ethHandle, ETH_CMD_G_SPEED, &speed) != ESP_OK)
  {
    return false;
  }
  *mbps = (speed == ETH_SPEED_100M) ? 100 : 10;
  return true;
}
