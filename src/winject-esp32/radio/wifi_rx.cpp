#include "wifi_rx.h"

#include "channel_info_endpoint.h"
#include "config.h"
#include "frame.h"
#include "lc_rx.h"
#include "packet.h"
#include "wifi.h"

#include <atomic>
#include <string.h>
#include <utility>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "bfc-esp32/semaphore.hpp"

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

bool wifi_rx::set_domain(uint16_t domain)
{
    domain_.store(domain, std::memory_order_release);
    return true;
}

uint16_t wifi_rx::domain() const
{
    return domain_.load(std::memory_order_acquire);
}

bool wifi_rx::accept_mpdu(const uint8_t* mpdu, size_t len) const
{
    return frameAddr3Accept(mpdu, len,
                            domain_.load(std::memory_order_acquire));
}

void wifi_rx::note_air(const wifi_pkt_rx_ctrl_t& ctrl)
{
    const char* name = wifi::format_phy(
        static_cast<uint8_t>(ctrl.sig_mode), static_cast<uint8_t>(ctrl.rate),
        static_cast<uint8_t>(ctrl.mcs), ctrl.sgi != 0);
    modulation_.store(name, std::memory_order_relaxed);
    const int8_t rssi = clamp_i8(ctrl.rssi);
    const int8_t snr = clamp_i8(ctrl.rssi - ctrl.noise_floor);
    rssi_.store(rssi, std::memory_order_relaxed);
    snr_.store(snr, std::memory_order_relaxed);
    air_valid_.store(true, std::memory_order_relaxed);
    channel_info_endpoint::instance().on_rx_air_info(rssi, snr);
}

void wifi_rx::on_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (buf == nullptr || (type != WIFI_PKT_DATA && type != WIFI_PKT_MISC))
    {
        return;
    }

    const auto* pkt = static_cast<wifi_promiscuous_pkt_t*>(buf);
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
    if (len > static_cast<int>(WIFI_TX_PACKET_CAP))
    {
        return;
    }

    if (!accept_mpdu(pkt->payload, static_cast<size_t>(len)))
    {
        return;
    }

    udp_rx_pkt_.fetch_add(1, std::memory_order_relaxed);
    note_air(pkt->rx_ctrl);
    radio_.pulse_rx_led();

    const bool failed = type == WIFI_PKT_MISC || pkt->rx_ctrl.rx_state != 0;
    if (failed)
    {
        drop_crc_error_.fetch_add(1, std::memory_order_relaxed);
        if (!allow_failed_crc_.load(std::memory_order_relaxed))
        {
            return;
        }
    }

    packet p = packet_allocator::rx().allocate();
    if (!p.is_valid())
    {
        drop_rx_no_pkt_pool_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    p.set_packet_offset(0);
    memcpy(p.data(), pkt->payload, static_cast<size_t>(len));
    p.set_packet_size(static_cast<size_t>(len));
    if (rx_ == nullptr || !rx_->rx(std::move(p)))
    {
        drop_rx_queue_full_.fetch_add(1, std::memory_order_relaxed);
    }
}

void wifi_rx::promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    wifi::instance().rx_.on_promiscuous(buf, type);
}

bool wifi_rx::init(lc_rx& rx)
{
    rx_ = &rx;
    return true;
}

bool wifi_rx::apply_monitor()
{
    wifi_promiscuous_filter_t filter = {};
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
    status->drop_crc_error = drop_crc_error_.load(std::memory_order_relaxed);
    status->drop_rx_no_pkt_pool =
        drop_rx_no_pkt_pool_.load(std::memory_order_relaxed);
    status->drop_rx_queue_full =
        drop_rx_queue_full_.load(std::memory_order_relaxed);
    status->udp_fwd_pkt = udp_fwd_pkt_.load(std::memory_order_relaxed);
    status->rx_queue =
        rx_ != nullptr ? static_cast<uint16_t>(rx_->queue_size()) : 0;
    status->rx_air_valid = air_valid_.load(std::memory_order_relaxed);
    status->rx_modulation = modulation_.load(std::memory_order_relaxed);
    status->rx_rssi = rssi_.load(std::memory_order_relaxed);
    status->rx_snr = snr_.load(std::memory_order_relaxed);
}

bool wifi_rx::set_allow_failed_crc(bool allow)
{
    if (!radio_.ready())
    {
        return false;
    }
    allow_failed_crc_.store(allow, std::memory_order_relaxed);
    bfc::semaphore::lock lock(radio_.lock(), pdMS_TO_TICKS(1000));
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
