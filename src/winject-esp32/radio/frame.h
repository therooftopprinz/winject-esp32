#ifndef WINJECT_FRAME_H_
#define WINJECT_FRAME_H_

#include "winject-esp32/semaphore.h"

#include <stddef.h>
#include <stdint.h>

enum WinjectMode
{
    WINJECT_MODE_BFC_TUNNEL_DEVICE = 0,
    WINJECT_MODE_STANDALONE = 1,
};

enum FrameRxClass
{
    FRAME_RX_NONE = 0,
    FRAME_RX_SELF = 1,
    FRAME_RX_MATCH = 2,
};

bool frameBegin();
bool frameSetMode(WinjectMode mode);
WinjectMode frameGetMode();
const char* frameModeName(WinjectMode mode);
bool frameParseMode(const char* text, WinjectMode* mode);

size_t frameWrap(const uint8_t* payload, size_t payloadLen, const uint8_t sa[6], uint8_t* out, size_t outMax);
FrameRxClass frameClassify(const uint8_t* mpdu, size_t len);
bool frameBssidMatch(const uint8_t* mpdu, size_t len);
bool frameHeard(const uint8_t* mpdu, size_t len);
bool frameMatch(const uint8_t* mpdu, size_t len);
bool frameUnwrap(const uint8_t* mpdu, size_t len, const uint8_t** payload, size_t* payloadLen);
void frameGetStaMac(uint8_t mac[6]);
void frameGetBssid(uint8_t bssid[6]);
void frameSetLocalAirports(const uint8_t macs[][6], size_t count);
void frameSetPeerAirports(const uint8_t macs[][6], size_t count);

bool frameAirportIsZero(const uint8_t mac[6]);
bool frameAirportValidStandalone(const uint8_t mac[6]);
bool frameAirportIsBroadcastStandalone(const uint8_t mac[6]);
void frameAirportSwap(const uint8_t in[6], uint8_t out[6]);
void frameStandaloneFilterSa(const uint8_t airport[6], uint8_t sa[6]);
bool frameStandaloneSaMatchesAirport(const uint8_t sa[6], const uint8_t airport[6]);

#endif  // WINJECT_FRAME_H_
