#ifndef WINJECT_MANAGER_AIR_SEQ_H_
#define WINJECT_MANAGER_AIR_SEQ_H_

#include <stddef.h>
#include <stdint.h>

#include "net_util.h"

// Per-TX uint16 sequence on every inject datagram so the peer RX can detect
// gaps (lost air packets). Big-endian prefix; wrapping arithmetic.
class air_seq
{
public:
    static constexpr size_t k_len = k_air_seq_len;

    // Prefix seq and increment. Fails if out is too small; seq is unchanged.
    bool stamp(uint8_t* out, size_t max, const uint8_t* data, size_t len,
               size_t* out_len);
    // Strip prefix, count forward gaps as lost. False if shorter than k_len.
    bool accept(const uint8_t* data, size_t len, const uint8_t** payload,
                size_t* plen);

    uint16_t tx() const
    {
        return tx_;
    }
    uint64_t lost() const
    {
        return lost_;
    }
    uint64_t take_lost();

private:
    void note_rx(uint16_t seq);

    uint16_t tx_ = 0;
    uint16_t rx_ = 0;
    bool have_rx_ = false;
    uint64_t lost_ = 0;
};

#endif  // WINJECT_MANAGER_AIR_SEQ_H_
