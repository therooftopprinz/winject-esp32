#include "ota.h"
#include "config.h"
#include "ethernet.h"
#include "wifi_radio.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <WebServer.h>
#include <stdio.h>

static WebServer g_http(OTA_HTTP_PORT);
static bool g_started = false;
static bool g_loggedEth = false;
static bool g_loggedAp = false;

static bool anyIpv4() {
    return ethernetConnected() || wifiRadioApActive();
}

static void ipv4ToString(uint32_t addr, char *out, size_t outLen) {
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&addr);
    snprintf(out, outLen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void handleRoot() {
    char ethStr[16] = "down";
    char apStr[16] = "down";
    uint32_t ip = 0;
    if (ethernetLocalIpv4(&ip)) {
        ipv4ToString(ip, ethStr, sizeof(ethStr));
    }
    if (wifiRadioApIpv4(&ip)) {
        ipv4ToString(ip, apStr, sizeof(apStr));
    }

    char page[640];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html><html><body>"
             "<h1>WInject-ESP32</h1>"
             "<p>eth %s</p>"
             "<p>ap %s %s</p>"
             "<p>ArduinoOTA UDP 3232 &nbsp; HTTP POST /update</p>"
             "<form method='POST' action='/update' enctype='multipart/form-data'>"
             "<input type='file' name='firmware'>"
             "<input type='submit' value='Update'>"
             "</form></body></html>",
             ethStr, wifiRadioApActive() ? wifiRadioApSsid() : "-", apStr);
    g_http.send(200, "text/html", page);
}

static void handleUpdateDone() {
    g_http.sendHeader("Connection", "close");
    if (Update.hasError()) {
        g_http.send(500, "text/plain", "update failed\n");
        return;
    }
    g_http.send(200, "text/plain", "ok, rebooting\n");
    delay(200);
    ESP.restart();
}

static void handleUpdateUpload() {
    HTTPUpload &up = g_http.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("HTTP OTA %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("HTTP OTA %u bytes\n", up.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

void otaBegin() {
    ArduinoOTA.setHostname(DEVICE_HOSTNAME);
    ArduinoOTA.onStart([]() { Serial.println("OTA start"); });
    ArduinoOTA.onEnd([]() { Serial.println("OTA end"); });
    ArduinoOTA.onError([](ota_error_t err) { Serial.printf("OTA error %u\n", err); });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        static unsigned lastPct = 101;
        const unsigned pct = total ? (done * 100u / total) : 0;
        if (pct != lastPct) {
            lastPct = pct;
            Serial.printf("OTA %u%%\n", pct);
        }
    });

    g_http.on("/", HTTP_GET, handleRoot);
    g_http.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
}

void otaPoll() {
    if (!g_started) {
        if (!anyIpv4()) {
            return;
        }
        ArduinoOTA.begin();
        g_http.begin();
        g_started = true;
    }

    uint32_t ip = 0;
    if (!g_loggedEth && ethernetLocalIpv4(&ip)) {
        char ipStr[16];
        ipv4ToString(ip, ipStr, sizeof(ipStr));
        Serial.printf("OTA ArduinoOTA %s:3232  HTTP %s:%u/update\n", ipStr, ipStr, OTA_HTTP_PORT);
        g_loggedEth = true;
    }
    if (!g_loggedAp && wifiRadioApIpv4(&ip)) {
        char ipStr[16];
        ipv4ToString(ip, ipStr, sizeof(ipStr));
        Serial.printf("OTA ArduinoOTA %s:3232  HTTP %s:%u/update (ap)\n", ipStr, ipStr,
                      OTA_HTTP_PORT);
        g_loggedAp = true;
    }

    ArduinoOTA.handle();
    g_http.handleClient();
}

bool otaActive() {
    return g_started;
}
