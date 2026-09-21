#ifndef WINJECT_MANAGER_MPDU_H_
#define WINJECT_MANAGER_MPDU_H_

#include <stddef.h>
#include <stdint.h>

#include "frame.h"

// Host-side 802.11 MPDU stamp / unpack (shared with firmware frame packing).
bool mpdu_init_rules(WinjectMode mode);
bool mpdu_build(uint8_t* out, size_t max, size_t* out_len,
                const pdu_slot_t slots[WIFI_PDU_SLOTS],
                const uint8_t* const bodies[WIFI_PDU_SLOTS], uint16_t domain);
bool mpdu_unpack(const uint8_t* mpdu, size_t len, pdu_slot_t slots[WIFI_PDU_SLOTS],
                 const uint8_t** body, size_t* body_len);

#endif  // WINJECT_MANAGER_MPDU_H_
