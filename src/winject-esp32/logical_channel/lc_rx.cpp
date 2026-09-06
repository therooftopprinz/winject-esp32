#include "lc_rx.h"

#include "frame.h"
#include "lc_rx_endpoint.h"

#include <utility>

#include "esp_log.h"
#include "freertos/task.h"

static const char* TAG = "lc_rx";

lc_rx& lc_rx::instance()
{
    static lc_rx inst;
    return inst;
}

bool lc_rx::init()
{
    return q.init();
}

void lc_rx::task(void* arg)
{
    static_cast<lc_rx*>(arg)->run();
}

bool lc_rx::start(BaseType_t core, UBaseType_t prio, uint32_t stack_bytes)
{
    if (xTaskCreatePinnedToCore(task, "lc_rx", stack_bytes, this, prio, nullptr,
                                core) != pdPASS)
    {
        ESP_LOGE(TAG, "lc_rx task failed");
        return false;
    }
    return true;
}

void lc_rx::run()
{
    for (;;)
    {
        packet mpdu = pop(portMAX_DELAY);
        if (!mpdu.is_valid())
        {
            continue;
        }
        handle_mpdu(std::move(mpdu));
    }
}

void lc_rx::set_endpoint(lc_rx_endpoint& ep)
{
    this->ep = &ep;
}

bool lc_rx::rx(packet&& pkt)
{
    if (!q.ready() || !pkt.is_valid())
    {
        drop_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!q.try_push(std::optional<packet>(std::move(pkt))))
    {
        drop_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void lc_rx::handle_mpdu(packet&& mpdu)
{
    if (mpdu.size() < WIFI_HDR_LEN || mpdu.data() == nullptr)
    {
        bad_mpdu_count_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    pdu_slot_t slots[WIFI_PDU_SLOTS] = {};
    frameUnpackSlots(mpdu.data() + 4, mpdu.data() + 10, slots);

    const size_t body_len = mpdu.size() - WIFI_HDR_LEN;
    const size_t sum = frameSlotPayloadBytes(slots);
    if (sum != body_len)
    {
        bad_mpdu_count_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        if (slots[i].size > WIFI_PAYLOAD_MAX)
        {
            bad_mpdu_count_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    if (ep == nullptr)
    {
        return;
    }

    size_t off = WIFI_HDR_LEN;
    for (int i = 0; i < WIFI_PDU_SLOTS; i++)
    {
        if (slots[i].size == 0)
        {
            continue;
        }
        packet pdu = mpdu.share();
        pdu.set_packet_offset(off);
        pdu.set_packet_size(slots[i].size);
        ep->forward(slots[i].bus, std::move(pdu));
        off += slots[i].size;
    }
}

packet lc_rx::pop(TickType_t wait)
{
    packet out;
    std::optional<packet> slot;
    if (!q.pop(&slot, wait) || !slot.has_value())
    {
        return out;
    }
    return std::move(*slot);
}

uint8_t lc_rx::queue_size() const
{
    return q.size();
}

uint32_t lc_rx::drop_count() const
{
    return drop_count_.load(std::memory_order_relaxed);
}

uint32_t lc_rx::bad_mpdu_count() const
{
    return bad_mpdu_count_.load(std::memory_order_relaxed);
}
