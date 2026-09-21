#ifndef WINJECT_WIFI_RX_H_
#define WINJECT_WIFI_RX_H_

#include "config.h"
#include "packet.h"

#include <atomic>
#include <optional>
#include <stddef.h>
#include <stdint.h>

#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "bfc-esp32/wait_free_queue.hpp"

class wifi;
class lc_rx_endpoint;
struct wifi_status_s;

class wifi_rx
{
    friend class wifi;

public:
    static constexpr uint8_t k_queue_cap = WIFI_RADIO_RX_QUEUE;

    wifi_rx(const wifi_rx&) = delete;
    wifi_rx& operator=(const wifi_rx&) = delete;

    bool init();
    bool start(BaseType_t core = APP_TASK_CORE,
               UBaseType_t prio = UPSTREAM_TASK_PRIO,
               uint32_t stack_bytes = 6144);

    void set_endpoint(lc_rx_endpoint& ep);

    uint8_t queue_size() const;
    uint8_t queue_capacity() const
    {
        return k_queue_cap;
    }

private:
    explicit wifi_rx(wifi& radio);

    bool apply_monitor();
    void fill_status(wifi_status_s* status);
    bool set_allow_failed_crc(bool allow);
    void note_udp_fwd_pkt();
    void reset_promisc_stats();

    bool set_domain(uint16_t domain);
    uint16_t domain() const;

    static void promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type);
    static int8_t clamp_i8(int value);
    void note_air(const wifi_pkt_rx_ctrl_t& ctrl);
    void stash_air_sample(const wifi_pkt_rx_ctrl_t& ctrl);
    void apply_stashed_air();
    void on_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type);
    bool accept_mpdu(const uint8_t* mpdu, size_t len) const;

    // Wait-free. From promiscuous CB only: enqueue and return.
    // Do not allocate, take mutexes, or call wake_up here — that runs on
    // the Wi-Fi task, which also completes esp_wifi_80211_tx.
    bool enqueue(packet&& pkt);

    static void task(void* arg);
    void run();
    void handle_mpdu(packet&& mpdu);
    packet pop(TickType_t wait);

    wifi& radio;
    bfc::wait_free_queue<std::optional<packet>, k_queue_cap> q;
    lc_rx_endpoint* ep = nullptr;
    std::atomic<uint16_t> domain_{0};
    std::atomic<bool> allow_failed_crc{false};
    std::atomic<uint32_t> udp_rx_pkt{0};
    std::atomic<uint32_t> drop_crc_error{0};
    std::atomic<uint32_t> drop_rx_no_pkt_pool{0};
    std::atomic<uint32_t> drop_rx_queue_full{0};
    std::atomic<uint32_t> udp_fwd_pkt{0};
    std::atomic<uint32_t> bad_mpdu_count_{0};
    std::atomic<bool> air_valid{false};
    std::atomic<const char*> modulation{nullptr};
    std::atomic<int8_t> rssi{0};
    std::atomic<int8_t> snr{0};
    std::atomic<uint8_t> stash_sig_mode_{0};
    std::atomic<uint8_t> stash_rate_{0};
    std::atomic<uint8_t> stash_mcs_{0};
    std::atomic<uint8_t> stash_sgi_{0};
    std::atomic<int8_t> stash_rssi_{0};
    std::atomic<int8_t> stash_noise_{0};

    // Promiscuous-path diagnostics (any on-air source, pre-Addr3).
    std::atomic<uint32_t> promisc_data{0};
    std::atomic<uint32_t> promisc_misc{0};
    std::atomic<uint32_t> promisc_ctrl{0};
    std::atomic<uint32_t> promisc_skip_type{0};
    std::atomic<uint32_t> promisc_ht{0};
    std::atomic<uint32_t> promisc_legacy{0};
    std::atomic<uint32_t> promisc_other_sig{0};
    std::atomic<uint32_t> promisc_misc_nonempty{0};
    std::atomic<uint32_t> promisc_drop_ampdu{0};
    std::atomic<uint32_t> promisc_drop_len{0};
    std::atomic<uint32_t> promisc_drop_addr3{0};
    std::atomic<uint32_t> promisc_ht_addr3_ok{0};
    std::atomic<uint32_t> promisc_ht_prefix{0};
    std::atomic<uint32_t> promisc_legacy_prefix{0};
    std::atomic<uint32_t> promisc_legacy_addr3_ok{0};
    std::atomic<uint32_t> promisc_domain_word_ok{0};
};

#endif  // WINJECT_WIFI_RX_H_
