#include "config.h"
#include "console.h"
#include "ethernet.h"
#include "frame.h"
#include "ota.h"
#include "upstream_rx.h"
#include "upstream_tx.h"
#include "wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "manager.h"
#include "nvs_flash.h"
#include "settings.h"

static const char* TAG = "winject";
static ethernet& g_ethernet = ethernet::instance();
static manager& g_netmgr = manager::instance();
static upstream_rx g_upstream_rx;
static upstream_tx g_upstream_tx;
static console g_console;

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "winject-esp32 starting  reset %d", (int)esp_reset_reason());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    settingsLoadCurrent(g_netmgr);
    if (!g_netmgr.start())
    {
        ESP_LOGE(TAG, "network manager start failed");
    }

    bool radio_ok = wifi::instance().initialize();
    if (!radio_ok)
    {
        ESP_LOGE(TAG, "wifi radio init failed");
    }
    else if (!frameBegin())
    {
        ESP_LOGE(TAG, "802.11 frame init failed");
        radio_ok = false;
    }

    if (!g_upstream_rx.init(g_ethernet) || !g_upstream_tx.init(g_ethernet))
    {
        ESP_LOGE(TAG, "upstream init failed");
    }
    else if (radio_ok &&
             !settingsApplyLive(g_upstream_rx, g_upstream_tx, g_netmgr))
    {
        ESP_LOGE(TAG, "settings apply failed");
    }

    if (!g_upstream_rx.start_task() || !g_upstream_tx.start_task())
    {
        ESP_LOGE(TAG, "upstream task create failed");
    }
    g_console.init(g_upstream_rx, g_upstream_tx, g_netmgr);
    otaBegin(g_netmgr);
}
