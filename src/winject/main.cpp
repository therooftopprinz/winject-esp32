#include "config.h"
#include "console.h"
#include "ethernet.h"
#include "frame.h"
#include "ota.h"
#include "upstream.h"
#include "wifi_radio.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char* TAG = "winject";

static void dhcpFallbackTask(void* arg)
{
  (void)arg;
  vTaskDelay(pdMS_TO_TICKS(DHCP_FALLBACK_MS));
  if (!wifiRadioApActive() && !ethernetConnected())
  {
    if (!wifiRadioStartApFallback())
    {
      ESP_LOGE(TAG, "wifi AP fallback failed");
    }
  }
  vTaskDelete(nullptr);
}

extern "C" void app_main(void)
{
  ESP_LOGI(TAG, "winject-esp32 starting");

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  ethernetBegin();
  if (!wifiRadioBegin())
  {
    ESP_LOGE(TAG, "wifi radio init failed");
  }
  else if (!frameBegin())
  {
    ESP_LOGE(TAG, "802.11 frame init failed");
  }
  upstreamBegin();
  consoleBegin();
  otaBegin();

  xTaskCreatePinnedToCore(dhcpFallbackTask, "dhcp_fb", 3072, nullptr, 5,
                          nullptr, APP_TASK_CORE);
}
