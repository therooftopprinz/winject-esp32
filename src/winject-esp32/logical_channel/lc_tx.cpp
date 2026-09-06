#include "lc_tx.h"

#include "channel_info_endpoint.h"

#include <utility>

lc_tx& lc_tx::instance()
{
    static lc_tx inst;
    return inst;
}

bool lc_tx::init()
{
    return q_.init();
}

void lc_tx::set_channel_info(channel_info_endpoint& ci)
{
    ci_ = &ci;
}

void lc_tx::publish_flow_ctrl()
{
    if (ci_ == nullptr)
    {
        return;
    }
    const uint8_t size = queue_size();
    if (size <= k_queue_cap / 2)
    {
        return;
    }
    ci_->on_flow_ctrl_info(size, k_queue_cap);
}

bool lc_tx::tx(bus_t bus, packet&& pkt)
{
    if (!q_.ready() || !pkt.is_valid())
    {
        drop_count_.fetch_add(1, std::memory_order_relaxed);
        publish_flow_ctrl();
        return false;
    }
    slot_s slot;
    slot.bus = bus;
    slot.pkt = std::move(pkt);
    if (!q_.try_push(std::move(slot)))
    {
        drop_count_.fetch_add(1, std::memory_order_relaxed);
        publish_flow_ctrl();
        return false;
    }
    publish_flow_ctrl();
    return true;
}

bool lc_tx::peek(bus_t* bus, uint16_t* payload_size) const
{
    if (bus == nullptr || payload_size == nullptr)
    {
        return false;
    }
    const slot_s* slot = q_.peek();
    if (slot == nullptr || !slot->pkt.has_value() || !slot->pkt->is_valid())
    {
        return false;
    }
    *bus = slot->bus;
    *payload_size = static_cast<uint16_t>(slot->pkt->size());
    return true;
}

packet lc_tx::pop(bus_t* bus, TickType_t wait)
{
    packet out;
    slot_s slot;
    if (!q_.pop(&slot, wait))
    {
        return out;
    }
    if (slot.pkt.has_value())
    {
        if (bus != nullptr)
        {
            *bus = slot.bus;
        }
        out = std::move(*slot.pkt);
    }
    publish_flow_ctrl();
    return out;
}

uint8_t lc_tx::queue_size() const
{
    return q_.size();
}

uint32_t lc_tx::drop_count() const
{
    return drop_count_.load(std::memory_order_relaxed);
}
