#include "packet.h"

#include <atomic>
#include <cstddef>
#include <string.h>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

packet::packet(packet_allocator& alloc, uint8_t* buf, size_t capacity)
    : alloc(&alloc), buf(buf), capacity_(capacity)
{
}

packet::packet(packet&& other) noexcept
{
    steal_from(other);
}

packet& packet::operator=(packet&& other) noexcept
{
    if (this != &other)
    {
        reset();
        steal_from(other);
    }
    return *this;
}

packet::~packet()
{
    reset();
}

void packet::steal_from(packet& other) noexcept
{
    alloc = other.alloc;
    buf = other.buf;
    capacity_ = other.capacity_;
    offset_ = other.offset_;
    size_ = other.size_;
    other.clear();
}

void packet::clear() noexcept
{
    alloc = nullptr;
    buf = nullptr;
    capacity_ = 0;
    offset_ = 0;
    size_ = 0;
}

packet packet::share() const
{
    packet out;
    if (buf == nullptr || alloc == nullptr)
    {
        return out;
    }
    alloc->add_ref(buf);
    out.alloc = alloc;
    out.buf = buf;
    out.capacity_ = capacity_;
    out.offset_ = offset_;
    out.size_ = size_;
    return out;
}

void packet::set_packet_offset(size_t offset)
{
    offset_ = offset;
}

void packet::set_packet_size(size_t size)
{
    size_ = size;
}

void packet::reset()
{
    if (alloc != nullptr && buf != nullptr)
    {
        alloc->release(buf);
    }
    clear();
}

bool packet::is_valid() const
{
    return buf != nullptr && offset_ + size_ <= capacity_;
}

uint8_t* packet::data()
{
    return buf == nullptr ? nullptr : buf + offset_;
}

const uint8_t* packet::data() const
{
    return buf == nullptr ? nullptr : buf + offset_;
}

size_t packet::size() const
{
    return size_;
}

size_t packet::offset() const
{
    return offset_;
}

size_t packet::capacity() const
{
    return buf == nullptr ? 0 : capacity_;
}

packet_allocator& packet_allocator::tx()
{
    static packet_allocator inst;
    return inst;
}

packet_allocator& packet_allocator::rx()
{
    static packet_allocator inst;
    return inst;
}

bool packet_allocator::init(size_t count)
{
    if (free != nullptr)
    {
        return true;
    }
    if (count == 0 || count > k_max_count)
    {
        return false;
    }
    QueueHandle_t q = xQueueCreate(static_cast<UBaseType_t>(count),
                                   sizeof(uint8_t));
    if (q == nullptr)
    {
        return false;
    }
    this->count = count;
    for (size_t i = 0; i < k_max_count; i++)
    {
        refs[i].store(0, std::memory_order_relaxed);
    }
    for (uint8_t i = 0; i < count; i++)
    {
        if (xQueueSend(q, &i, 0) != pdTRUE)
        {
            vQueueDelete(q);
            free = nullptr;
            count = 0;
            return false;
        }
    }
    free = q;
    return true;
}

packet packet_allocator::allocate()
{
    packet out;
    if (free == nullptr)
    {
        return out;
    }
    uint8_t idx = 0;
    if (xQueueReceive(static_cast<QueueHandle_t>(free), &idx, 0) != pdTRUE)
    {
        return out;
    }
    if (idx >= count)
    {
        return out;
    }
    refs[idx].store(1, std::memory_order_relaxed);
    return packet(*this, storage + static_cast<size_t>(idx) * k_buf_size,
                  k_buf_size);
}

size_t packet_allocator::available() const
{
    if (free == nullptr)
    {
        return 0;
    }
    return uxQueueMessagesWaiting(static_cast<QueueHandle_t>(free));
}

void packet_allocator::set_on_space(bfc::light_function<void()> cb)
{
    on_space = std::move(cb);
}

void packet_allocator::add_ref(uint8_t* buf)
{
    uint8_t idx = 0;
    if (!index_of(buf, &idx))
    {
        return;
    }
    refs[idx].fetch_add(1, std::memory_order_relaxed);
}

void packet_allocator::release(uint8_t* buf)
{
    uint8_t idx = 0;
    if (!index_of(buf, &idx))
    {
        return;
    }
    const uint8_t prev = refs[idx].fetch_sub(1, std::memory_order_acq_rel);
    if (prev != 1)
    {
        return;
    }
    if (free != nullptr)
    {
        xQueueSend(static_cast<QueueHandle_t>(free), &idx, 0);
    }
    if (on_space)
    {
        on_space();
    }
}

bool packet_allocator::index_of(const uint8_t* buf, uint8_t* idx) const
{
    if (buf == nullptr || idx == nullptr || count == 0)
    {
        return false;
    }
    const ptrdiff_t off = buf - storage;
    if (off < 0 || (off % static_cast<ptrdiff_t>(k_buf_size)) != 0)
    {
        return false;
    }
    const ptrdiff_t i = off / static_cast<ptrdiff_t>(k_buf_size);
    if (i < 0 || i >= static_cast<ptrdiff_t>(count))
    {
        return false;
    }
    *idx = static_cast<uint8_t>(i);
    return true;
}
