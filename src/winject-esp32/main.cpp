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

// Never abort on optional bring-up: a dead component must leave Ethernet +
// HTTP OTA (+ console) alive for rescue.
static void idle_forever()
{
    while (true)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "winject-esp32 starting  reset %d", (int)esp_reset_reason());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        err = nvs_flash_erase();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "nvs erase failed: %s", esp_err_to_name(err));
        }
        else
        {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs init failed: %s (continuing)", esp_err_to_name(err));
    }

    if (esp_netif_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_netif_init failed — cannot start network/OTA");
        idle_forever();
    }
    if (esp_event_loop_create_default() != ESP_OK)
    {
        ESP_LOGE(TAG, "event loop failed — cannot start network/OTA");
        idle_forever();
    }

    settings::instance().load_current();
    if (!udp_logger::instance().init())
    {
        ESP_LOGE(TAG, "udp logger init failed");
    }
    if (!g_netmgr.start())
    {
        ESP_LOGE(TAG, "network manager start failed");
    }

    // HTTP OTA before WiFi/LC so radio init failure still leaves rescue.
    otaBegin(g_netmgr);

    if (settings::instance().configured_mode() == WINJECT_MODE_OTA)
    {
        ESP_LOGI(TAG, "OTA mode: network + console + HTTP update only");
        frameSetMode(WINJECT_MODE_OTA);
        if (!g_console.init(g_netmgr))
        {
            ESP_LOGE(TAG, "console init failed");
        }
        return;
    }

    bool packets_ok = packet_allocator::tx().init(WIFI_RADIO_TX_QUEUE) &&
                      packet_allocator::rx().init(WIFI_RADIO_RX_QUEUE);
    if (!packets_ok)
    {
        ESP_LOGE(TAG, "packet allocator init failed");
    }

    lc_tx& lctx = lc_tx::instance();
    lc_rx& lcrx = lc_rx::instance();
    lc_tx_endpoint& tx_ep = lc_tx_endpoint::instance();
    lc_rx_endpoint& rx_ep = lc_rx_endpoint::instance();
    channel_info_endpoint& ci = channel_info_endpoint::instance();

    bool lc_ok = packets_ok && lctx.init() && lcrx.init() && tx_ep.init(lctx) &&
                 rx_ep.init();
    if (!lc_ok)
    {
        ESP_LOGE(TAG, "lc init failed");
    }
    else
    {
        lcrx.set_endpoint(rx_ep);
    }
    if (!ci.init())
    {
        ESP_LOGE(TAG, "channel info init failed");
        lc_ok = false;
    }
    else if (lc_ok)
    {
        lctx.set_channel_info(ci);
    }

    bool radio_ok = false;
    if (lc_ok)
    {
        radio_ok = wifi::instance().initialize();
        if (!radio_ok)
        {
            ESP_LOGE(TAG, "wifi radio init failed — continuing without radio");
        }
        else if (!frameBegin())
        {
            ESP_LOGE(TAG, "802.11 frame init failed — continuing without radio");
            radio_ok = false;
        }
    }

    if (radio_ok)
    {
        if (!wifi::instance().tx().init(lctx) ||
            !wifi::instance().rx().init(lcrx))
        {
            ESP_LOGE(TAG, "wifi lc wiring failed — continuing without radio");
            radio_ok = false;
        }
    }

    if (radio_ok && !settings::instance().apply_live())
    {
        ESP_LOGE(TAG, "settings apply failed");
    }

    if (lc_ok)
    {
        if (!tx_ep.start() || !lcrx.start() || !ci.start())
        {
            ESP_LOGE(TAG, "endpoint start failed");
        }
    }
    if (radio_ok && !wifi::instance().tx().start())
    {
        ESP_LOGE(TAG, "wifi_tx start failed");
        radio_ok = false;
    }

    // Full console if LC endpoints exist; otherwise network-only rescue console.
    if (lc_ok)
    {
        if (!g_console.init(tx_ep, rx_ep, ci, g_netmgr))
        {
            ESP_LOGE(TAG, "console init failed");
        }
    }
    else if (!g_console.init(g_netmgr))
    {
        ESP_LOGE(TAG, "rescue console init failed");
    }

    if (!radio_ok)
    {
        ESP_LOGW(TAG, "running degraded: OTA/console up, radio down");
    }
}
