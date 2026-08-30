#include <Arduino.h>

#include "config.h"
#include "console.h"
#include "ethernet.h"
#include "frame.h"
#include "ota.h"
#include "upstream.h"
#include "wifi_radio.h"

static uint32_t g_bootMs = 0;

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nwinject-esp32 starting");

    ethernetBegin();
    if (!wifiRadioBegin()) {
        Serial.println("wifi radio init failed");
    } else if (!frameBegin()) {
        Serial.println("802.11 frame init failed");
    }
    upstreamBegin();
    consoleBegin();
    otaBegin();
    g_bootMs = millis();
}

void loop() {
    if (!wifiRadioApActive() && !ethernetConnected() &&
        (millis() - g_bootMs) >= DHCP_FALLBACK_MS) {
        if (!wifiRadioStartApFallback()) {
            Serial.println("wifi AP fallback failed");
        }
    }

    otaPoll();
    consolePoll();
    upstreamPoll();
}
