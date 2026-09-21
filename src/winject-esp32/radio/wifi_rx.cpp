#include "wifi_rx.h"

#include "channel_info_endpoint.h"
#include "config.h"
#include "frame.h"
#include "lc_rx_endpoint.h"
#include "packet.h"
#include "wifi.h"

#include <atomic>
#include <string.h>
#include <utility>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bfc-esp32/semaphore.hpp"

static const char* TAG = "wifi_rx";

wifi_rx::wifi_rx(wifi& radio) : radio(radio) {}

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
    modulation.store(name, std::memory_order_relaxed);
    const int8_t rssi_dbm = clamp_i8(ctrl.rssi);
    const int8_t snr_db = clamp_i8(ctrl.rssi - ctrl.noise_floor);
    rssi.store(rssi_dbm, std::memory_order_relaxed);
    snr.store(snr_db, std::memory_order_relaxed);
    air_valid.store(true, std::memory_order_relaxed);
    channel_info_endpoint::instance().on_rx_air_info(rssi_dbm, snr_db);
}

void wifi_rx::stash_air_sample(const wifi_pkt_rx_ctrl_t& ctrl)
{
    stash_sig_mode_.store(static_cast<uint8_t>(ctrl.sig_mode),
                          std::memory_order_relaxed);
    stash_rate_.store(static_cast<uint8_t>(ctrl.rate),
                      std::memory_order_relaxed);
    stash_mcs_.store(static_cast<uint8_t>(ctrl.mcs), std::memory_order_relaxed);
    stash_sgi_.store(ctrl.sgi != 0 ? 1u : 0u, std::memory_order_relaxed);
    stash_rssi_.store(clamp_i8(ctrl.rssi), std::memory_order_relaxed);
    stash_noise_.store(clamp_i8(ctrl.noise_floor), std::memory_order_relaxed);
}

void wifi_rx::apply_stashed_air()
{
    wifi_pkt_rx_ctrl_t ctrl = {};
    ctrl.sig_mode = stash_sig_mode_.load(std::memory_order_relaxed);
    ctrl.rate = stash_rate_.load(std::memory_order_relaxed);
    ctrl.mcs = stash_mcs_.load(std::memory_order_relaxed);
    ctrl.sgi = stash_sgi_.load(std::memory_order_relaxed) != 0 ? 1 : 0;
    ctrl.rssi = stash_rssi_.load(std::memory_order_relaxed);
    ctrl.noise_floor = stash_noise_.load(std::memory_order_relaxed);
    note_air(ctrl);
}

bool wifi_rx::enqueue(packet&& pkt)
{
    if (!q.ready() || !pkt.is_valid())
    {
        return false;
    }
    return q.try_push(std::optional<packet>(std::move(pkt)));
}

void wifi_rx::reset_promisc_stats()
{
    promisc_data.store(0, std::memory_order_relaxed);
    promisc_misc.store(0, std::memory_order_relaxed);
    promisc_ctrl.store(0, std::memory_order_relaxed);
    promisc_skip_type.store(0, std::memory_order_relaxed);
    promisc_ht.store(0, std::memory_order_relaxed);
    promisc_legacy.store(0, std::memory_order_relaxed);
    promisc_other_sig.store(0, std::memory_order_relaxed);
    promisc_misc_nonempty.store(0, std::memory_order_relaxed);
    promisc_drop_ampdu.store(0, std::memory_order_relaxed);
    promisc_drop_len.store(0, std::memory_order_relaxed);
    promisc_drop_addr3.store(0, std::memory_order_relaxed);
    promisc_ht_addr3_ok.store(0, std::memory_order_relaxed);
    promisc_ht_prefix.store(0, std::memory_order_relaxed);
    promisc_legacy_prefix.store(0, std::memory_order_relaxed);
    promisc_legacy_addr3_ok.store(0, std::memory_order_relaxed);
    promisc_domain_word_ok.store(0, std::memory_order_relaxed);
}

void wifi_rx::on_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type)
{
    if (buf == nullptr)
    {
        return;
    }
    // HT MCS inject may be classified as CTRL/MISC on ESP32 promisc; Addr3 match
    // still uses the MPDU in payload when sig_len is valid.
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MISC && type != WIFI_PKT_CTRL)
    {
        promisc_skip_type.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto* pkt = static_cast<wifi_promiscuous_pkt_t*>(buf);
    if (type == WIFI_PKT_DATA)
    {
        promisc_data.fetch_add(1, std::memory_order_relaxed);
    }
    else if (type == WIFI_PKT_MISC)
    {
        promisc_misc.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        promisc_ctrl.fetch_add(1, std::memory_order_relaxed);
    }

    const wifi_pkt_rx_ctrl_t& rx = pkt->rx_ctrl;
    if (rx.sig_mode == 1)
    {
        promisc_ht.fetch_add(1, std::memory_order_relaxed);
    }
    else if (rx.sig_mode == 0)
    {
        promisc_legacy.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        promisc_other_sig.fetch_add(1, std::memory_order_relaxed);
    }
    if (type == WIFI_PKT_MISC && rx.sig_len > WIFI_HDR_LEN + 4)
    {
        promisc_misc_nonempty.fetch_add(1, std::memory_order_relaxed);
    }

    // Drop only true multi-subframe A-MPDUs. HT MCS often sets aggregation=1
    // for a single MPDU; ampdu_cnt alone is not a reliable drop signal.
    if (rx.aggregation != 0 && rx.ampdu_cnt > 1)
    {
        promisc_drop_ampdu.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    int len = rx.sig_len;
    if (len <= WIFI_HDR_LEN + 4)
    {
        promisc_drop_len.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    len -= 4;
    if (len > static_cast<int>(WIFI_TX_PACKET_CAP))
    {
        return;
    }

    const size_t mpdu_len = static_cast<size_t>(len);
    if (!frameAddr3PrefixMatch(pkt->payload, mpdu_len))
    {
        promisc_drop_addr3.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool prefix = true;
    const uint16_t want_domain = domain_.load(std::memory_order_acquire);
    if (prefix && mpdu_len >= 22 && want_domain != 0)
    {
        const uint16_t got = static_cast<uint16_t>(
            (pkt->payload[20] << 8) | pkt->payload[21]);
        if (got == want_domain)
        {
            promisc_domain_word_ok.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (rx.sig_mode == 1)
    {
        if (prefix)
        {
            promisc_ht_prefix.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else if (rx.sig_mode == 0 && prefix)
    {
        promisc_legacy_prefix.fetch_add(1, std::memory_order_relaxed);
    }

    if (!accept_mpdu(pkt->payload, mpdu_len))
    {
        promisc_drop_addr3.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (rx.sig_mode == 1)
    {
        promisc_ht_addr3_ok.fetch_add(1, std::memory_order_relaxed);
    }
    else if (rx.sig_mode == 0)
    {
        promisc_legacy_addr3_ok.fetch_add(1, std::memory_order_relaxed);
    }

    // FCS without computing CRC: do not drop on
    // WIFI_PKT_MISC or nonzero rx_state — raw-inject peers were rejected
    // 100% that way while USB monitor FCS was clean (false positive).
    // Count hardware 0x41 for telemetry only (should stay ~0 with FCSFAIL off).
    constexpr uint8_t k_rx_state_fcs_fail = 0x41;
    if (pkt->rx_ctrl.rx_state == k_rx_state_fcs_fail)
    {
        drop_crc_error.fetch_add(1, std::memory_order_relaxed);
    }

    stash_air_sample(pkt->rx_ctrl);

    packet p = packet_allocator::rx().allocate();
    if (!p.is_valid())
    {
        drop_rx_no_pkt_pool.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    udp_rx_pkt.fetch_add(1, std::memory_order_relaxed);
    p.set_packet_offset(0);
    memcpy(p.data(), pkt->payload, static_cast<size_t>(len));
    p.set_packet_size(static_cast<size_t>(len));
    if (!enqueue(std::move(p)))
    {
        drop_rx_queue_full.fetch_add(1, std::memory_order_relaxed);
    }
}

void wifi_rx::promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type)
{
    wifi::instance().rx().on_promiscuous(buf, type);
}

bool wifi_rx::init()
{
    return q.init();
}

void wifi_rx::set_endpoint(lc_rx_endpoint& ep_ref)
{
    ep = &ep_ref;
}

void wifi_rx::task(void* arg)
{
    static_cast<wifi_rx*>(arg)->run();
}

bool wifi_rx::start(BaseType_t core, UBaseType_t prio, uint32_t stack_bytes)
{
    if (!q.ready())
    {
        ESP_LOGE(TAG, "wifi_rx start without queue");
        return false;
    }
    if (xTaskCreatePinnedToCore(task, "wifi_rx", stack_bytes, this, prio,
                                nullptr, core) != pdPASS)
    {
        ESP_LOGE(TAG, "wifi_rx task failed");
        return false;
    }
    return true;
}

void wifi_rx::handle_mpdu(packet&& mpdu)
{
    if (mpdu.size() < WIFI_RADIO_INJECT_MIN || !mpdu.is_valid())
    {
        bad_mpdu_count_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    apply_stashed_air();
    radio.pulse_rx_led();
    if (ep == nullptr)
    {
        return;
    }
    ep->forward(std::move(mpdu));
}

packet wifi_rx::pop(TickType_t wait)
{
    packet out;
    std::optional<packet> slot;
    if (!q.pop(&slot, wait) || !slot.has_value())
    {
        return out;
    }
    out = std::move(*slot);
    return out;
}

void wifi_rx::run()
{
    for (;;)
    {
        packet mpdu = pop(portMAX_DELAY);
        if (mpdu.is_valid())
        {
            handle_mpdu(std::move(mpdu));
        }
    }
}

uint8_t wifi_rx::queue_size() const
{
    return q.size();
}

bool wifi_rx::apply_monitor()
{
    wifi_promiscuous_filter_t filter = {};
    // Raw-inject peers are delivered as DATA/MISC with spurious rx_state==0x41;
    // do not drop on that in on_promiscuous (see comment there). FCSFAIL must
    // stay enabled on ESP32 or Addr3-matched inject frames never reach the CB.
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA |
                         WIFI_PROMIS_FILTER_MASK_DATA_MPDU |
                         WIFI_PROMIS_FILTER_MASK_DATA_AMPDU |
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
    (void)radio.apply_sta_decode_protocol();
    return true;
}

void wifi_rx::fill_status(wifi_status_s* status)
{
    if (status == nullptr)
    {
        return;
    }
    status->allow_failed_crc =
        allow_failed_crc.load(std::memory_order_relaxed);
    status->udp_rx_pkt = udp_rx_pkt.load(std::memory_order_relaxed);
    status->drop_crc_error = drop_crc_error.load(std::memory_order_relaxed);
    status->drop_rx_no_pkt_pool =
        drop_rx_no_pkt_pool.load(std::memory_order_relaxed);
    status->drop_rx_queue_full =
        drop_rx_queue_full.load(std::memory_order_relaxed);
    status->udp_fwd_pkt = udp_fwd_pkt.load(std::memory_order_relaxed);
    status->rx_queue = queue_size();
    status->rx_air_valid = air_valid.load(std::memory_order_relaxed);
    status->rx_modulation = modulation.load(std::memory_order_relaxed);
    status->rx_rssi = rssi.load(std::memory_order_relaxed);
    status->rx_snr = snr.load(std::memory_order_relaxed);
    status->promisc_data = promisc_data.load(std::memory_order_relaxed);
    status->promisc_misc = promisc_misc.load(std::memory_order_relaxed);
    status->promisc_ctrl = promisc_ctrl.load(std::memory_order_relaxed);
    status->promisc_skip_type = promisc_skip_type.load(std::memory_order_relaxed);
    status->promisc_ht = promisc_ht.load(std::memory_order_relaxed);
    status->promisc_legacy = promisc_legacy.load(std::memory_order_relaxed);
    status->promisc_other_sig = promisc_other_sig.load(std::memory_order_relaxed);
    status->promisc_misc_nonempty =
        promisc_misc_nonempty.load(std::memory_order_relaxed);
    status->promisc_drop_ampdu =
        promisc_drop_ampdu.load(std::memory_order_relaxed);
    status->promisc_drop_len = promisc_drop_len.load(std::memory_order_relaxed);
    status->promisc_drop_addr3 =
        promisc_drop_addr3.load(std::memory_order_relaxed);
    status->promisc_ht_addr3_ok =
        promisc_ht_addr3_ok.load(std::memory_order_relaxed);
    status->promisc_ht_prefix =
        promisc_ht_prefix.load(std::memory_order_relaxed);
    status->promisc_legacy_prefix =
        promisc_legacy_prefix.load(std::memory_order_relaxed);
    status->promisc_legacy_addr3_ok =
        promisc_legacy_addr3_ok.load(std::memory_order_relaxed);
    status->promisc_domain_word_ok =
        promisc_domain_word_ok.load(std::memory_order_relaxed);
}

bool wifi_rx::set_allow_failed_crc(bool allow)
{
    if (!radio.ready())
    {
        return false;
    }
    allow_failed_crc.store(allow, std::memory_order_relaxed);
    bfc::semaphore::lock lock(radio.lock, pdMS_TO_TICKS(1000));
    if (!lock)
    {
        return false;
    }
    return apply_monitor();
}

void wifi_rx::note_udp_fwd_pkt()
{
    udp_fwd_pkt.fetch_add(1, std::memory_order_relaxed);
}
