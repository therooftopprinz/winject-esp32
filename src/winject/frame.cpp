#include "frame.h"
#include "config.h"

#include <Arduino.h>
#include <esp_wifi.h>
#include <string.h>

static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t kBssid[6] = {WIFI_BSSID};
static uint8_t g_staMac[6] = {};
static uint16_t g_seq = 0;

static size_t headerLen(const uint8_t *mpdu, size_t len) {
    if (mpdu == nullptr || len < WIFI_HDR_LEN) {
        return 0;
    }
    // Type = Data (bits 2-3 of FC0). Subtype 0x8 = QoS Data.
    if ((mpdu[0] & 0x0C) != 0x08) {
        return 0;
    }
    if ((mpdu[0] & 0xF0) == 0x80) {
        return len >= WIFI_QOS_HDR_LEN ? WIFI_QOS_HDR_LEN : 0;
    }
    if ((mpdu[0] & 0xF0) == 0x00) {
        return WIFI_HDR_LEN;
    }
    return 0;
}

bool frameBegin() {
    const esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, g_staMac);
    if (err != ESP_OK) {
        Serial.printf("frame sta mac failed: %s\n", esp_err_to_name(err));
        return false;
    }
    Serial.printf("frame SA %02X:%02X:%02X:%02X:%02X:%02X  BSSID %02X:%02X:%02X:%02X:%02X:%02X\n",
                  g_staMac[0], g_staMac[1], g_staMac[2], g_staMac[3], g_staMac[4], g_staMac[5],
                  kBssid[0], kBssid[1], kBssid[2], kBssid[3], kBssid[4], kBssid[5]);
    return true;
}

size_t frameWrap(const uint8_t *payload, size_t payloadLen, uint8_t *out, size_t outMax) {
    if (payload == nullptr || out == nullptr) {
        return 0;
    }
    if (payloadLen > WIFI_PAYLOAD_MAX) {
        return 0;
    }
    const size_t total = WIFI_HDR_LEN + payloadLen;
    if (outMax < total) {
        return 0;
    }

    out[0] = 0x08;
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x00;
    memcpy(out + 4, kBroadcast, 6);
    memcpy(out + 10, g_staMac, 6);
    memcpy(out + 16, kBssid, 6);

    const uint16_t seqCtl = static_cast<uint16_t>((g_seq & 0x0FFF) << 4);
    g_seq = static_cast<uint16_t>((g_seq + 1) & 0x0FFF);
    out[22] = static_cast<uint8_t>(seqCtl);
    out[23] = static_cast<uint8_t>(seqCtl >> 8);

    if (payloadLen > 0) {
        memcpy(out + WIFI_HDR_LEN, payload, payloadLen);
    }
    return total;
}

bool frameMatch(const uint8_t *mpdu, size_t len) {
    const size_t hdr = headerLen(mpdu, len);
    if (hdr == 0) {
        return false;
    }
    if (memcmp(mpdu + 4, kBroadcast, 6) != 0) {
        return false;
    }
    return memcmp(mpdu + 16, kBssid, 6) == 0;
}

bool frameUnwrap(const uint8_t *mpdu, size_t len, const uint8_t **payload, size_t *payloadLen) {
    if (!frameMatch(mpdu, len) || payload == nullptr || payloadLen == nullptr) {
        return false;
    }
    const size_t hdr = headerLen(mpdu, len);
    *payload = mpdu + hdr;
    *payloadLen = len - hdr;
    return true;
}

void frameGetStaMac(uint8_t mac[6]) {
    if (mac != nullptr) {
        memcpy(mac, g_staMac, 6);
    }
}

void frameGetBssid(uint8_t bssid[6]) {
    if (bssid != nullptr) {
        memcpy(bssid, kBssid, 6);
    }
}
