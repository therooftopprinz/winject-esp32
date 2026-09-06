#include "packet.h"

#include <atomic>
#include <cstddef>
#include <string.h>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

packet::packet(packet_allocator& alloc, uint8_t* buf, size_t capacity)
    : alloc_(&alloc), buf_(buf), capacity_(capacity)
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
    alloc_ = other.alloc_;
    buf_ = other.buf_;
    capacity_ = other.capacity_;
    offset_ = other.offset_;
    size_ = other.size_;
    other.clear();
}

void packet::clear() noexcept
{
    alloc_ = nullptr;
    buf_ = nullptr;
    capacity_ = 0;
    offset_ = 0;
    size_ = 0;
}

packet packet::share() const
{
    packet out;
    if (buf_ == nullptr || alloc_ == nullptr)
    {
        return out;
    }
    alloc_->add_ref(buf_);
    out.alloc_ = alloc_;
    out.buf_ = buf_;
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
    if (alloc_ != nullptr && buf_ != nullptr)
    {
        alloc_->release(buf_);
    }
    clear();
}

bool packet::is_valid() const
{
    return buf_ != nullptr && offset_ + size_ <= capacity_;
}

uint8_t* packet::data()
{
    return buf_ == nullptr ? nullptr : buf_ + offset_;
}

const uint8_t* packet::data() const
{
    return buf_ == nullptr ? nullptr : buf_ + offset_;
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
    return buf_ == nullptr ? 0 : capacity_;
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
    if (free_ != nullptr)
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
    count_ = count;
    for (size_t i = 0; i < k_max_count; i++)
    {
        refs_[i].store(0, std::memory_order_relaxed);
    }
    for (uint8_t i = 0; i < count_; i++)
    {
        if (xQueueSend(q, &i, 0) != pdTRUE)
        {
            vQueueDelete(q);
            free_ = nullptr;
            count_ = 0;
            return false;
        }
    }
    free_ = q;
    return true;
}

packet packet_allocator::allocate()
{
    packet out;
    if (free_ == nullptr)
    {
        return out;
    }
    uint8_t idx = 0;
    if (xQueueReceive(static_cast<QueueHandle_t>(free_), &idx, 0) != pdTRUE)
    {
        return out;
    }
    if (idx >= count_)
    {
        return out;
    }
    refs_[idx].store(1, std::memory_order_relaxed);
    return packet(*this, storage_ + static_cast<size_t>(idx) * k_buf_size,
                  k_buf_size);
}

size_t packet_allocator::available() const
{
    if (free_ == nullptr)
    {
        return 0;
    }
    return uxQueueMessagesWaiting(static_cast<QueueHandle_t>(free_));
}

void packet_allocator::set_on_space(bfc::light_function<void()> cb)
{
    on_space_ = std::move(cb);
}

void packet_allocator::add_ref(uint8_t* buf)
{
    uint8_t idx = 0;
    if (!index_of(buf, &idx))
    {
        return;
    }
    refs_[idx].fetch_add(1, std::memory_order_relaxed);
}

void packet_allocator::release(uint8_t* buf)
{
    uint8_t idx = 0;
    if (!index_of(buf, &idx))
    {
        return;
    }
    const uint8_t prev = refs_[idx].fetch_sub(1, std::memory_order_acq_rel);
    if (prev != 1)
    {
        return;
    }
    if (free_ != nullptr)
    {
        xQueueSend(static_cast<QueueHandle_t>(free_), &idx, 0);
    }
    if (on_space_)
    {
        on_space_();
    }
}

bool packet_allocator::index_of(const uint8_t* buf, uint8_t* idx) const
{
    if (buf == nullptr || idx == nullptr || count_ == 0)
    {
        return false;
    }
    const ptrdiff_t off = buf - storage_;
    if (off < 0 || (off % static_cast<ptrdiff_t>(k_buf_size)) != 0)
    {
        return false;
    }
    const ptrdiff_t i = off / static_cast<ptrdiff_t>(k_buf_size);
    if (i < 0 || i >= static_cast<ptrdiff_t>(count_))
    {
        return false;
    }
    *idx = static_cast<uint8_t>(i);
    return true;
}
