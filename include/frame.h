#pragma once

#include <stddef.h>
#include <stdint.h>

bool frameBegin();
size_t frameWrap(const uint8_t* payload, size_t payloadLen, uint8_t* out,
                 size_t outMax);
bool frameMatch(const uint8_t* mpdu, size_t len);
bool frameUnwrap(const uint8_t* mpdu, size_t len, const uint8_t** payload,
                 size_t* payloadLen);
void frameGetStaMac(uint8_t mac[6]);
void frameGetBssid(uint8_t bssid[6]);
