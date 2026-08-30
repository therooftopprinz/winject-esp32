#include "config.h"
#include "ethernet.h"

#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char* TAG = "test";

static const char* resetReasonName(esp_reset_reason_t reason)
{
  switch (reason)
  {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt-wdt";
    case ESP_RST_TASK_WDT:
      return "task-wdt";
    case ESP_RST_WDT:
      return "wdt";
    case ESP_RST_DEEPSLEEP:
      return "deep-sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unknown";
  }
}

static const char* chipModelName(esp_chip_model_t model)
{
  switch (model)
  {
    case CHIP_ESP32:
      return "ESP32";
    default:
      return "unknown";
  }
}

extern "C" void app_main(void)
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_chip_info_t chip = {};
  esp_chip_info(&chip);
  uint32_t flashSize = 0;
  esp_flash_get_size(nullptr, &flashSize);

  ESP_LOGI(TAG, "winject-esp32 hardware test");
  ESP_LOGI(TAG, "chip  %s rev%d, %u core(s) @ %d MHz",
           chipModelName(chip.model), chip.revision, chip.cores,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
  ESP_LOGI(TAG, "flash %lu bytes, reset %s", (unsigned long)flashSize,
           resetReasonName(esp_reset_reason()));
  ESP_LOGI(TAG, "ETH   LAN8720 addr %d  MDC %d  MDIO %d  power %d",
           ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER);

  ethernetBegin();

  uint32_t lastSec = 0;
  for (;;)
  {
    vTaskDelay(pdMS_TO_TICKS(1000));
    const uint32_t sec = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    if (sec == lastSec)
    {
      continue;
    }
    lastSec = sec;

    if (ethernetConnected())
    {
      uint8_t mac[6] = {};
      uint32_t ip = 0;
      uint32_t mbps = 0;
      ethernetMac(mac);
      ethernetLocalIpv4(&ip);
      ethernetLinkSpeedMbps(&mbps);
      const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
      ESP_LOGI(TAG,
               "up %lus  heap %u  ETH %02X:%02X:%02X:%02X:%02X:%02X  "
               "%u.%u.%u.%u  %luMbps",
               (unsigned long)sec, (unsigned)esp_get_free_heap_size(), mac[0],
               mac[1], mac[2], mac[3], mac[4], mac[5], b[0], b[1], b[2], b[3],
               (unsigned long)mbps);
    }
    else
    {
      ESP_LOGI(TAG, "up %lus  heap %u  ETH waiting for link/DHCP",
               (unsigned long)sec, (unsigned)esp_get_free_heap_size());
    }
  }
}
