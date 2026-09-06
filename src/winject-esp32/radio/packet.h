#ifndef WINJECT_PACKET_H_
#define WINJECT_PACKET_H_

#include "winject-esp32/config.h"

#include <stddef.h>
#include <stdint.h>
#include <atomic>

#include "bfc-esp32/function.hpp"

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

class packet_allocator;

class packet
{
public:
    packet(packet&& other) noexcept;
    packet& operator=(packet&& other) noexcept;
    ~packet();

    packet(const packet&) = delete;
    packet& operator=(const packet&) = delete;

    packet share() const;

    void set_packet_offset(size_t offset);
    void set_packet_size(size_t size);
    void reset();

    bool is_valid() const;
    uint8_t* data();
    const uint8_t* data() const;
    size_t size() const;
    size_t offset() const;
    size_t capacity() const;

private:
    friend class packet_allocator;
    friend class lc_tx;
    friend class lc_rx;
    packet() = default;
    packet(packet_allocator& alloc, uint8_t* buf, size_t capacity);

    void steal_from(packet& other) noexcept;
    void clear() noexcept;

    packet_allocator* alloc = nullptr;
    uint8_t* buf = nullptr;
    size_t capacity_ = 0;
    size_t offset_ = 0;
    size_t size_ = 0;
};

class packet_allocator
{
public:
    static packet_allocator& tx();
    static packet_allocator& rx();

    static constexpr size_t k_buf_size = WIFI_TX_PACKET_CAP;
    static constexpr size_t k_max_count = WIFI_RADIO_TX_QUEUE > WIFI_RADIO_RX_QUEUE
                                              ? WIFI_RADIO_TX_QUEUE
                                              : WIFI_RADIO_RX_QUEUE;

    bool init(size_t count);
    packet allocate();
    size_t available() const;
    void set_on_space(bfc::light_function<void()> cb);

private:
    friend class packet;
    packet_allocator() = default;
    packet_allocator(const packet_allocator&) = delete;
    packet_allocator& operator=(const packet_allocator&) = delete;

    void add_ref(uint8_t* buf);
    void release(uint8_t* buf);
    bool index_of(const uint8_t* buf, uint8_t* idx) const;

    uint8_t storage[k_max_count * k_buf_size]{};
    std::atomic<uint8_t> refs[k_max_count]{};
    size_t count = 0;
    void* free = nullptr;  // QueueHandle_t
    bfc::light_function<void()> on_space;
};

#endif  // WINJECT_PACKET_H_
