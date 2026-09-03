#include "wifi_rx.h"

#include "config.h"
#include "frame.h"
#include "wifi.h"

#include <atomic>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "wifi_rx";

wifi_rx::wifi_rx(wifi& radio) : radio_(radio) {}

int8_t wifi_rx::clamp_i8(int value)
{
    if (value > 127)
    {
        return 127;
    }
    if (value < -128)
    {
        return -128;
    }
    return static_cast<int8_t>(value);
}

void wifi_rx::note_air(const wifi_pkt_rx_ctrl_t& ctrl)
{
    const char* name = wifi::format_phy(
        static_cast<uint8_t>(ctrl.sig_mode), static_cast<uint8_t>(ctrl.rate),
        static_cast<uint8_t>(ctrl.mcs), ctrl.sgi != 0);
    modulation_.store(name, std::memory_order_relaxed);
    rssi_.store(clamp_i8(ctrl.rssi), std::memory_order_relaxed);
    snr_.store(clamp_i8(ctrl.rssi - ctrl.noise_floor),
               std::memory_order_relaxed);
    air_valid_.store(true, std::memory_order_relaxed);
}

void wifi_rx::on_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (buf == nullptr || (type != WIFI_PKT_DATA && type != WIFI_PKT_MISC))
    {
        return;
    }

    const auto* pkt = static_cast<wifi_promiscuous_pkt_t*>(buf);
    // True A-MPDUs can advertise a sig_len larger than this DMA fragment.
    // HT MCS frames often set aggregation=1 even for a single MPDU.
    if (pkt->rx_ctrl.ampdu_cnt > 1)
    {
        return;
    }

    int len = pkt->rx_ctrl.sig_len;
    if (len <= WIFI_HDR_LEN + 4)
    {
        return;
    }
    len -= 4;
    if (len > WIFI_RADIO_MAX_FRAME)
    {
        len = WIFI_RADIO_MAX_FRAME;
    }

    const bool failed = type == WIFI_PKT_MISC || pkt->rx_ctrl.rx_state != 0;
    if (!frameHeard(pkt->payload, static_cast<size_t>(len)))
    {
        return;
    }
    if (frameClassify(pkt->payload, static_cast<size_t>(len)) != FRAME_RX_MATCH)
    {
        return;
    }

    udp_rx_pkt_.fetch_add(1, std::memory_order_relaxed);
    note_air(pkt->rx_ctrl);
    if (failed)
    {
        udp_rx_crc_err_.fetch_add(1, std::memory_order_relaxed);
        if (!allow_failed_crc_.load(std::memory_order_relaxed))
        {
            return;
        }
    }

    slot_s* slot = pool_.take(0);
    if (slot == nullptr)
    {
        udp_rx_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    slot->len = static_cast<uint16_t>(len);
    memcpy(slot->data, pkt->payload, static_cast<size_t>(len));
    if (!queue_.post(slot, 0))
    {
        pool_.release(slot);
        udp_rx_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

void wifi_rx::promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    wifi::instance().rx_.on_promiscuous(buf, type);
}

bool wifi_rx::init()
{
    return pool_.init() && queue_.init();
}

bool wifi_rx::apply_monitor()
{
    wifi_promiscuous_filter_t filter = {};
    // FCSFAIL/MISC are required or CRC-failed MPDUs never reach the callback.
    // Count matched CRC errors even when forwarding is off; drop them after
    // classify so a busy channel does not fill the RX queue.
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA |
                         WIFI_PROMIS_FILTER_MASK_MISC |
                         WIFI_PROMIS_FILTER_MASK_FCSFAIL;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promiscuous_cb);

    const esp_err_t err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "promiscuous failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void wifi_rx::fill_status(wifi_status_s* status)
{
    if (status == nullptr)
    {
        return;
    }
    status->allow_failed_crc =
        allow_failed_crc_.load(std::memory_order_relaxed);
    status->udp_rx_pkt = udp_rx_pkt_.load(std::memory_order_relaxed);
    status->udp_rx_crc_err = udp_rx_crc_err_.load(std::memory_order_relaxed);
    status->udp_rx_dropped = udp_rx_dropped_.load(std::memory_order_relaxed);
    status->udp_fwd_pkt = udp_fwd_pkt_.load(std::memory_order_relaxed);
    status->rx_queue = static_cast<uint16_t>(queue_.size());
    status->rx_air_valid = air_valid_.load(std::memory_order_relaxed);
    status->rx_modulation = modulation_.load(std::memory_order_relaxed);
    status->rx_rssi = rssi_.load(std::memory_order_relaxed);
    status->rx_snr = snr_.load(std::memory_order_relaxed);
}

bool wifi_rx::pop(uint8_t* out, size_t* len, size_t max_len, TickType_t wait)
{
    if (out == nullptr || len == nullptr)
    {
        return false;
    }
    slot_s* slot = queue_.take(wait);
    if (slot == nullptr)
    {
        return false;
    }
    const size_t copy = slot->len < max_len ? slot->len : max_len;
    memcpy(out, slot->data, copy);
    *len = copy;
    pool_.release(slot);
    radio_.pulse_rx_led();
    return true;
}

bool wifi_rx::set_allow_failed_crc(bool allow)
{
    if (!radio_.ready())
    {
        return false;
    }
    allow_failed_crc_.store(allow, std::memory_order_relaxed);
    semaphore::lock lock(radio_.lock(), pdMS_TO_TICKS(1000));
    if (!lock)
    {
        return false;
    }
    return apply_monitor();
}

void wifi_rx::note_udp_fwd_pkt()
{
    udp_fwd_pkt_.fetch_add(1, std::memory_order_relaxed);
}
