#ifndef WINJECT_FRAME_H_
#define WINJECT_FRAME_H_

#include "packet.h"

#include <stddef.h>
#include <stdint.h>

enum WinjectMode
{
    WINJECT_MODE_BFC_TUNNEL_DEVICE = 0,
    WINJECT_MODE_STANDALONE = 1,
    // Boot-only: network + console + HTTP OTA; no WiFi / LC / endpoints.
    WINJECT_MODE_OTA = 2,
};

bool frameBegin();
bool frameSetMode(WinjectMode mode);
WinjectMode frameGetMode();
const char* frameModeName(WinjectMode mode);
bool frameParseMode(const char* text, WinjectMode* mode);

void frameGetStaMac(uint8_t mac[6]);
void frameGetBssidPrefix(uint8_t prefix[4]);
void frameBuildAddr3(uint8_t addr3[6], uint16_t domain);
bool frameAddr3Accept(const uint8_t* mpdu, size_t len, uint16_t domain);

void framePackSlots(uint8_t addr1[6], uint8_t addr2[6],
                    const pdu_slot_t slots[WIFI_PDU_SLOTS]);
void frameUnpackSlots(const uint8_t addr1[6], const uint8_t addr2[6],
                      pdu_slot_t slots[WIFI_PDU_SLOTS]);
size_t frameSlotPayloadBytes(const pdu_slot_t slots[WIFI_PDU_SLOTS]);
void frameStampHeader(uint8_t* hdr, const pdu_slot_t slots[WIFI_PDU_SLOTS],
                      uint16_t domain);

#endif  // WINJECT_FRAME_H_
