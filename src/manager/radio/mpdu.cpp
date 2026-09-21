#include "radio/mpdu.h"

#include "frame.h"

#include <string.h>

bool mpdu_init_rules(WinjectMode mode)
{
    if (!frameBegin())
    {
        return false;
    }
    return frameSetMode(mode);
}

bool mpdu_build(uint8_t* out, size_t max, size_t* out_len,
                const pdu_slot_t slots[WIFI_PDU_SLOTS],
                const uint8_t* const bodies[WIFI_PDU_SLOTS], uint16_t domain)
{
    if (out == nullptr || out_len == nullptr || slots == nullptr ||
        bodies == nullptr || domain == 0)
    {
        return false;
    }
    const size_t payload = frameSlotPayloadBytes(slots);
    if (payload == 0 || payload > WIFI_PAYLOAD_MAX)
    {
        return false;
    }
    const size_t total = WIFI_HDR_LEN + payload;
    if (total > max || total > WIFI_RADIO_INJECT_MAX)
    {
        return false;
    }
    size_t off = WIFI_HDR_LEN;
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        if (slots[i].size == 0)
        {
            continue;
        }
        if (bodies[i] == nullptr)
        {
            return false;
        }
        memcpy(out + off, bodies[i], slots[i].size);
        off += slots[i].size;
    }
    frameStampHeader(out, slots, domain);
    *out_len = total;
    return true;
}

bool mpdu_unpack(const uint8_t* mpdu, size_t len, pdu_slot_t slots[WIFI_PDU_SLOTS],
                 const uint8_t** body, size_t* body_len)
{
    if (mpdu == nullptr || slots == nullptr || body == nullptr ||
        body_len == nullptr || len < WIFI_HDR_LEN)
    {
        return false;
    }
    frameUnpackSlots(mpdu + 4, mpdu + 10, slots);
    const size_t blen = len - WIFI_HDR_LEN;
    if (frameSlotPayloadBytes(slots) != blen)
    {
        return false;
    }
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        if (slots[i].size > WIFI_PAYLOAD_MAX)
        {
            return false;
        }
    }
    *body = mpdu + WIFI_HDR_LEN;
    *body_len = blen;
    return true;
}
