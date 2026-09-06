#ifndef BFC_WAIT_FREE_QUEUE_HPP_
#define BFC_WAIT_FREE_QUEUE_HPP_

#include <stdint.h>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace bfc
{

// Fixed-capacity SPSC-style queue: non-blocking try_push, optional blocking pop.
// Producer path never waits (timeout 0). Storage is an internal slot array plus
// FreeRTOS free/filled index queues.
template <typename T, uint8_t Cap>
class wait_free_queue
{
public:
    static_assert(Cap > 0, "wait_free_queue capacity must be > 0");

    wait_free_queue() = default;

    ~wait_free_queue()
    {
        if (free != nullptr)
        {
            vQueueDelete(free);
            free = nullptr;
        }
        if (filled != nullptr)
        {
            vQueueDelete(filled);
            filled = nullptr;
        }
    }

    wait_free_queue(const wait_free_queue&) = delete;
    wait_free_queue& operator=(const wait_free_queue&) = delete;

    bool init()
    {
        if (free != nullptr && filled != nullptr)
        {
            return true;
        }
        QueueHandle_t free_q = xQueueCreate(Cap, sizeof(uint8_t));
        QueueHandle_t filled_q = xQueueCreate(Cap, sizeof(uint8_t));
        if (free_q == nullptr || filled_q == nullptr)
        {
            if (free_q != nullptr)
            {
                vQueueDelete(free_q);
            }
            if (filled_q != nullptr)
            {
                vQueueDelete(filled_q);
            }
            return false;
        }
        for (uint8_t i = 0; i < Cap; i++)
        {
            if (xQueueSend(free_q, &i, 0) != pdTRUE)
            {
                vQueueDelete(free_q);
                vQueueDelete(filled_q);
                return false;
            }
        }
        free = free_q;
        filled = filled_q;
        return true;
    }

    bool ready() const
    {
        return free != nullptr && filled != nullptr;
    }

    // Non-blocking. Moves into a free slot only after one is claimed.
    // false = full / not ready (item not moved), or filled-enqueue failed
    // after move (slot cleared; item already consumed).
    bool try_push(T&& item)
    {
        if (!ready())
        {
            return false;
        }
        uint8_t idx = 0;
        if (xQueueReceive(free, &idx, 0) != pdTRUE)
        {
            return false;
        }
        if (idx >= Cap)
        {
            return false;
        }
        slots[idx] = std::move(item);
        if (xQueueSend(filled, &idx, 0) != pdTRUE)
        {
            slots[idx] = T{};
            xQueueSend(free, &idx, 0);
            return false;
        }
        return true;
    }

    // Non-blocking view of the front slot. nullptr if empty / not ready.
    const T* peek() const
    {
        if (!ready())
        {
            return nullptr;
        }
        uint8_t idx = 0;
        if (xQueuePeek(filled, &idx, 0) != pdTRUE || idx >= Cap)
        {
            return nullptr;
        }
        return &slots[idx];
    }

    T* peek()
    {
        return const_cast<T*>(
            static_cast<const wait_free_queue*>(this)->peek());
    }

    // Moves front into *out. wait=0 is non-blocking. false on timeout /
    // not ready / null out.
    bool pop(T* out, TickType_t wait)
    {
        if (!ready() || out == nullptr)
        {
            return false;
        }
        uint8_t idx = 0;
        if (xQueueReceive(filled, &idx, wait) != pdTRUE)
        {
            return false;
        }
        if (idx < Cap)
        {
            *out = std::move(slots[idx]);
            slots[idx] = T{};
        }
        xQueueSend(free, &idx, 0);
        return true;
    }

    bool try_pop(T* out)
    {
        return pop(out, 0);
    }

    uint8_t size() const
    {
        if (filled == nullptr)
        {
            return 0;
        }
        return static_cast<uint8_t>(uxQueueMessagesWaiting(filled));
    }

    static constexpr uint8_t capacity()
    {
        return Cap;
    }

private:
    T slots[Cap]{};
    mutable QueueHandle_t free = nullptr;
    mutable QueueHandle_t filled = nullptr;
};

}  // namespace bfc

#endif  // BFC_WAIT_FREE_QUEUE_HPP_
