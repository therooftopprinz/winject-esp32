#include "ethernet.h"
#include "config.h"

#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <esp_system.h>

static volatile bool g_ethConnected = false;

static void onNetworkEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            ETH.setHostname(DEVICE_HOSTNAME);
            Serial.println("ETH started");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("ETH link up");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            g_ethConnected = true;
            Serial.printf("ETH MAC: %s  IP: %s\n",
                          ETH.macAddress().c_str(),
                          ETH.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            g_ethConnected = false;
            Serial.println("ETH link down");
            break;
        case ARDUINO_EVENT_ETH_STOP:
            g_ethConnected = false;
            Serial.println("ETH stopped");
            break;
        default:
            break;
    }
}

void ethernetBegin() {
    uint8_t mac[] = {ETH_MAC_BYTES};
    const esp_err_t macErr = esp_base_mac_addr_set(mac);
    if (macErr != ESP_OK) {
        Serial.printf("Failed to set base MAC (%s), using factory MAC\n",
                      esp_err_to_name(macErr));
    }

    WiFi.onEvent(onNetworkEvent);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER,
              ETH_CLK_MODE);
#else
    ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE,
              ETH_CLK_MODE);
#endif
}

bool ethernetConnected() {
    return g_ethConnected;
}

bool ethernetLocalIpv4(uint32_t *out) {
    if (out == nullptr || !g_ethConnected) {
        return false;
    }
    *out = static_cast<uint32_t>(ETH.localIP());
    return *out != 0;
}
