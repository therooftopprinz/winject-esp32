#ifndef WINJECT_PDU_TYPES_H_
#define WINJECT_PDU_TYPES_H_

#include "winject-esp32/config.h"

#include <stddef.h>
#include <stdint.h>

using bus_t = uint8_t;  // lcid; 0 = broadcast

inline bool bus_is_broadcast(bus_t b)
{
    return b == 0;
}

struct pdu_slot_t
{
    bus_t bus;      // lcid on the air
    uint16_t size;  // 11-bit value; 0 = absent
};

// host = IPv4, network byte order
struct ip_port_t
{
    uint32_t host;
    uint16_t port;
};

inline bool ip_port_eq(const ip_port_t& a, const ip_port_t& b)
{
    return a.host == b.host && a.port == b.port;
}

#endif  // WINJECT_PDU_TYPES_H_
