#include "frames/air_seq.h"

#include <arpa/inet.h>
#include <string.h>

namespace
{
void store_be16(uint8_t* p, uint16_t v)
{
    const uint16_t n = htons(v);
    memcpy(p, &n, sizeof(n));
}

uint16_t load_be16(const uint8_t* p)
{
    uint16_t n;
    memcpy(&n, p, sizeof(n));
    return ntohs(n);
}
}  // namespace

bool air_seq::stamp(uint8_t* out, size_t max, const uint8_t* data, size_t len,
                     size_t* out_len)
{
    if (out == nullptr || out_len == nullptr)
    {
        return false;
    }
    if (len > 0 && data == nullptr)
    {
        return false;
    }
    if (len > max || max - len < k_len)
    {
        return false;
    }
    store_be16(out, tx_);
    if (len > 0)
    {
        memcpy(out + k_len, data, len);
    }
    tx_ = static_cast<uint16_t>(tx_ + 1);
    *out_len = len + k_len;
    return true;
}

bool air_seq::accept(const uint8_t* data, size_t len, const uint8_t** payload,
                     size_t* plen)
{
    if (data == nullptr || payload == nullptr || plen == nullptr ||
        len < k_len)
    {
        return false;
    }
    note_rx(load_be16(data));
    *payload = data + k_len;
    *plen = len - k_len;
    return true;
}

uint64_t air_seq::take_lost()
{
    const uint64_t n = lost_;
    lost_ = 0;
    return n;
}

void air_seq::note_rx(uint16_t seq)
{
    if (!have_rx_)
    {
        rx_ = seq;
        have_rx_ = true;
        return;
    }
    if (seq == rx_)
    {
        return;
    }
    const uint16_t expected = static_cast<uint16_t>(rx_ + 1);
    const uint16_t ahead = static_cast<uint16_t>(seq - expected);
    // ahead < 0x8000: seq is forward (possibly with gaps). Else old/reorder.
    if (ahead < 0x8000)
    {
        lost_ += ahead;
        rx_ = seq;
    }
}
