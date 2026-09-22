#ifndef WINJECT_PACKET_H_
#define WINJECT_PACKET_H_

#include "pdu_types.h"

#include <stddef.h>
#include <stdint.h>
#include <atomic>

#include "bfc-esp32/function.hpp"

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

    // EMAC input delivers a malloc'd frame; payload points inside it. Frees
    // heap_owner on reset (see upstream_tx L2 hijack — no pool memcpy on that path).
    static packet adopt_eth_frame(uint8_t* heap_owner, const uint8_t* payload,
                                  size_t len);

private:
    friend class packet_allocator;
    friend class wifi_tx;
    friend class wifi_rx;
    friend class upstream_rx_endpoint;
    packet() = default;
    packet(packet_allocator& alloc, uint8_t* buf, size_t capacity);

    void steal_from(packet& other) noexcept;
    void clear() noexcept;

    packet_allocator* alloc = nullptr;
    uint8_t* buf = nullptr;
    uint8_t* heap_owner_ = nullptr;
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

    // backing must outlive the allocator; backing_count ≥ count.
    bool init(size_t count, uint8_t* backing, size_t backing_count);
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

    uint8_t* storage = nullptr;
    size_t storage_count = 0;
    std::atomic<uint8_t> refs[k_max_count]{};
    size_t count = 0;
    void* free = nullptr;  // QueueHandle_t
    bfc::light_function<void()> on_space;
};

#endif  // WINJECT_PACKET_H_
