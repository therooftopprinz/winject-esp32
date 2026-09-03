#ifndef WINJECT_HOST_IDF_STUB_H_
#define WINJECT_HOST_IDF_STUB_H_

#include <stdint.h>
#include <string.h>

typedef int esp_err_t;
inline constexpr esp_err_t ESP_OK = 0;

enum wifi_interface_t
{
    WIFI_IF_STA = 0,
};

inline const char* esp_err_to_name(esp_err_t)
{
    return "ESP_OK";
}

#define ESP_LOGI(tag, fmt, ...) ((void)0)
#define ESP_LOGE(tag, fmt, ...) ((void)0)

inline esp_err_t esp_wifi_get_mac(wifi_interface_t, uint8_t mac[6])
{
    static const uint8_t kSta[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    memcpy(mac, kSta, 6);
    return ESP_OK;
}

#endif  // WINJECT_HOST_IDF_STUB_H_
