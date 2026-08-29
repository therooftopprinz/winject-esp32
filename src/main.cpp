#include <Arduino.h>

#include "ethernet.h"

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nwinject-esp32 starting");

    ethernetBegin();
}

void loop() {
    static uint32_t lastMs = 0;
    const uint32_t now = millis();
    if (now - lastMs < 1000) {
        return;
    }
    lastMs = now;

    if (ethernetConnected()) {
        Serial.println("Ethernet is active");
    } else {
        Serial.println("Waiting for Ethernet...");
    }
}
