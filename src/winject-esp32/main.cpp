#include "channel_info_endpoint.h"
#include "config.h"
#include "console.h"
#include "frame.h"
#include "lc_rx.h"
#include "lc_rx_endpoint.h"
#include "lc_tx.h"
#include "lc_tx_endpoint.h"
#include "ota.h"
#include "packet.h"
#include "settings.h"
#include "udp_logger.h"
#include "wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "manager.h"
#include "nvs_flash.h"

static const char* TAG = "winject";
static manager& g_netmgr = manager::instance();
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

    settings::instance().load_current();
    if (!udp_logger::instance().init())
    {
        ESP_LOGE(TAG, "udp logger init failed");
    }
    if (!g_netmgr.start())
    {
        ESP_LOGE(TAG, "network manager start failed");
    }

    if (settings::instance().configured_mode() == WINJECT_MODE_OTA)
    {
        ESP_LOGI(TAG, "OTA mode: network + console + HTTP update only");
        frameSetMode(WINJECT_MODE_OTA);
        g_console.init(g_netmgr);
        otaBegin(g_netmgr);
        return;
    }

    if (!packet_allocator::tx().init(WIFI_RADIO_TX_QUEUE) ||
        !packet_allocator::rx().init(WIFI_RADIO_RX_QUEUE))
    {
        ESP_LOGE(TAG, "packet allocator init failed");
    }

    lc_tx& lctx = lc_tx::instance();
    lc_rx& lcrx = lc_rx::instance();
    lc_tx_endpoint& tx_ep = lc_tx_endpoint::instance();
    lc_rx_endpoint& rx_ep = lc_rx_endpoint::instance();
    channel_info_endpoint& ci = channel_info_endpoint::instance();

    if (!lctx.init() || !lcrx.init() || !tx_ep.init(lctx) || !rx_ep.init())
    {
        ESP_LOGE(TAG, "lc init failed");
    }
    lcrx.set_endpoint(rx_ep);
    if (!ci.init())
    {
        ESP_LOGE(TAG, "channel info init failed");
    }
    lctx.set_channel_info(ci);

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

    if (radio_ok)
    {
        if (!wifi::instance().tx().init(lctx) ||
            !wifi::instance().rx().init(lcrx))
        {
            ESP_LOGE(TAG, "wifi lc wiring failed");
            radio_ok = false;
        }
    }

    if (radio_ok && !settings::instance().apply_live())
    {
        ESP_LOGE(TAG, "settings apply failed");
    }

    if (!tx_ep.start() || !lcrx.start() || !ci.start())
    {
        ESP_LOGE(TAG, "endpoint start failed");
    }
    if (radio_ok && !wifi::instance().tx().start())
    {
        ESP_LOGE(TAG, "wifi_tx start failed");
    }

    g_console.init(tx_ep, rx_ep, ci, g_netmgr);
    otaBegin(g_netmgr);
}
