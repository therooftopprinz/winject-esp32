#include <Arduino.h>
#include <ETH.h>
#include <esp_system.h>

#include "config.h"
#include "ethernet.h"

static const char *resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
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

void setup() {
    Serial.begin(115200);
    delay(200);

    Serial.println("\nwinject-esp32 hardware test");
    Serial.printf("chip  %s rev%d, %u core(s) @ %u MHz\n",
                  ESP.getChipModel(),
                  ESP.getChipRevision(),
                  ESP.getChipCores(),
                  ESP.getCpuFreqMHz());
    Serial.printf("flash %u bytes, reset %s\n",
                  ESP.getFlashChipSize(),
                  resetReasonName(esp_reset_reason()));
    Serial.printf("ETH   LAN8720 addr %d  MDC %d  MDIO %d  power %d\n",
                  ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER);

    ethernetBegin();
}

void loop() {
    static uint32_t lastMs = 0;
    const uint32_t now = millis();
    if (now - lastMs < 1000) {
        return;
    }
    lastMs = now;

    Serial.printf("up %lus  heap %u  ",
                  now / 1000UL,
                  ESP.getFreeHeap());

    if (ethernetConnected()) {
        Serial.printf("ETH %s  %s  %uMbps\n",
                      ETH.macAddress().c_str(),
                      ETH.localIP().toString().c_str(),
                      ETH.linkSpeed());
    } else {
        Serial.println("ETH waiting for link/DHCP");
    }
}
