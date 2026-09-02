#ifndef WINJECT_PACKET_QUEUE_H_
#define WINJECT_PACKET_QUEUE_H_

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

template <typename Slot, size_t k_depth>
class packet_queue
{
    static_assert(k_depth >= 1, "packet_queue depth must be at least 1");

public:
    static constexpr size_t depth = k_depth;

    packet_queue() = default;
    packet_queue(const packet_queue&) = delete;
    packet_queue& operator=(const packet_queue&) = delete;

    bool init()
    {
        if (ready())
        {
            return true;
        }
        filled_ = xQueueCreate(k_depth, sizeof(Slot*));
        return filled_ != nullptr;
    }

    bool ready() const
    {
        return filled_ != nullptr;
    }

    bool post(Slot* slot, TickType_t wait = 0)
    {
        if (!ready() || slot == nullptr)
        {
            return false;
        }
        return xQueueSend(filled_, &slot, wait) == pdTRUE;
    }

    Slot* take(TickType_t wait = 0)
    {
        if (!ready())
        {
            return nullptr;
        }
        Slot* slot = nullptr;
        if (xQueueReceive(filled_, &slot, wait) != pdTRUE)
        {
            return nullptr;
        }
        return slot;
    }

    UBaseType_t size() const
    {
        if (!ready())
        {
            return 0;
        }
        return uxQueueMessagesWaiting(filled_);
    }

private:
    QueueHandle_t filled_ = nullptr;
};

#endif  // WINJECT_PACKET_QUEUE_H_
