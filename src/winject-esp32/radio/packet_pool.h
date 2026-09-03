#ifndef WINJECT_PACKET_POOL_H_
#define WINJECT_PACKET_POOL_H_

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

template <size_t k_data_bytes, size_t k_slot_count>
class packet_pool
{
    static_assert(k_data_bytes >= 1 && k_data_bytes <= 65535,
                  "packet_pool data size must fit in uint16_t");
    static_assert(k_slot_count >= 1 && k_slot_count <= 255,
                  "packet_pool uses uint8_t slot indices");

public:
    static constexpr size_t data_bytes = k_data_bytes;
    static constexpr size_t slot_count = k_slot_count;

    struct slot_s
    {
        uint16_t len;
        uint8_t data[k_data_bytes];
    };

    packet_pool() = default;
    packet_pool(const packet_pool&) = delete;
    packet_pool& operator=(const packet_pool&) = delete;

    bool init()
    {
        if (ready())
        {
            return true;
        }
        free_ = xQueueCreate(k_slot_count, sizeof(uint8_t));
        if (free_ == nullptr)
        {
            return false;
        }
        for (uint8_t i = 0; i < k_slot_count; i++)
        {
            xQueueSend(free_, &i, 0);
        }
        return true;
    }

    bool ready() const
    {
        return free_ != nullptr;
    }

    slot_s* take(TickType_t wait = 0)
    {
        if (!ready())
        {
            return nullptr;
        }
        uint8_t idx = 0;
        if (xQueueReceive(free_, &idx, wait) != pdTRUE)
        {
            return nullptr;
        }
        if (idx >= k_slot_count)
        {
            return nullptr;
        }
        slots_[idx].len = 0;
        return &slots_[idx];
    }

    void release(slot_s* slot)
    {
        uint8_t idx = 0;
        if (!ready() || !index_of(slot, &idx))
        {
            return;
        }
        xQueueSend(free_, &idx, 0);
    }

private:
    bool index_of(const slot_s* slot, uint8_t* idx) const
    {
        if (slot == nullptr || idx == nullptr)
        {
            return false;
        }
        const ptrdiff_t i = slot - slots_;
        if (i < 0 || i >= static_cast<ptrdiff_t>(k_slot_count))
        {
            return false;
        }
        *idx = static_cast<uint8_t>(i);
        return true;
    }

    slot_s slots_[k_slot_count];
    QueueHandle_t free_ = nullptr;
};

#endif  // WINJECT_PACKET_POOL_H_
