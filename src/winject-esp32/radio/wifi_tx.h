#ifndef WINJECT_WIFI_TX_H_
#define WINJECT_WIFI_TX_H_

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
class channel_info_endpoint;
struct wifi_status_s;

class wifi_tx
{
    friend class wifi;

public:
    static constexpr uint8_t k_queue_cap = WIFI_RADIO_TX_QUEUE;

    wifi_tx(const wifi_tx&) = delete;
    wifi_tx& operator=(const wifi_tx&) = delete;

    bool init();
    bool start(BaseType_t core = WIFI_RADIO_TASK_CORE,
               UBaseType_t prio = WIFI_RADIO_TASK_PRIO,
               uint32_t stack_bytes = 6144);

    void set_channel_info(channel_info_endpoint& ci);
    // lc_tx staging depth for combined FLOW_CTRL (host inject backpressure).
    void report_inject_staging(uint8_t staging_count);
    // true while lc_tx must not pull from EMAC (WiFi DMA busy or staging backlogged).
    void set_lc_tx_yield(bool blocked);
    uint32_t tx_in_flight_count() const;
    // lc_tx flush: false while WiFi DMA pressure would starve EMAC RX.
    bool may_feed_from_lc_tx() const;
    bool enqueue(packet&& pkt);

    uint8_t queue_size() const;
    uint8_t queue_hwm() const
    {
        return tx_q_hwm.load(std::memory_order_relaxed);
    }
    void note_queue_sample(uint8_t size);
    void reset_queue_hwm();
    void set_dry_run(bool enabled);
    bool dry_run() const;
    uint32_t max_in_flight_cap() const;
    bool set_max_in_flight(uint32_t n);
    uint8_t compute_tx_slots() const;

    uint8_t queue_capacity() const
    {
        return k_queue_cap;
    }

private:
    explicit wifi_tx(wifi& radio);

    void run();
    packet pop(TickType_t wait);
    void publish_flow_ctrl();
    bool inject_retry(const uint8_t* frame, size_t len);

    bool set_domain(uint16_t domain);
    uint16_t domain() const;

    bool apply_power();
    bool apply_cca();
    bool apply_tx_done_cb();
    void fill_status(wifi_status_s* status);
    int8_t power_dbm() const;
    void note_udp_tx_pkt();
    bool set_cca_enabled(bool enabled);
    bool set_tx_power(int8_t dbm);

    static void task(void* arg);
    static bool seq_of(const uint8_t* frame, size_t len, uint16_t* seq);
    void note_submit(uint16_t seq);
    void cancel_submit(uint16_t seq);
    void note_done(uint16_t seq);
    void record_latency(uint32_t us);
    static void on_tx_done(const esp_80211_tx_info_t* info);

    static constexpr size_t kPendingCap = 64;
    static constexpr size_t kLatencySamples = 4;

    wifi& radio;
    bfc::wait_free_queue<std::optional<packet>, k_queue_cap> q;
    channel_info_endpoint* ci = nullptr;
    std::atomic<uint16_t> domain_{0};
    bool cca_enabled = true;
    bool phy_cca_off = false;
    int8_t tx_power_dbm = WIFI_DEFAULT_TX_POWER_DBM;
    std::atomic<uint32_t> udp_tx_pkt{0};
    std::atomic<bool> dry_run_{false};
    std::atomic<uint64_t> flow_ctrl_last_us{0};
    std::atomic<uint32_t> flow_ctrl_last_inject_{0};
    std::atomic<uint8_t> lc_staging_{0};
    std::atomic<bool> lc_tx_yield_{false};
    std::atomic<uint32_t> drop_tx_nomem{0};
    std::atomic<uint8_t> tx_q_hwm{0};
    std::atomic<uint32_t> tx_retry_count{0};
    std::atomic<uint32_t> tx_retry_nomem{0};
    std::atomic<uint32_t> tx_retry_other{0};
    std::atomic<uint32_t> inject_ok{0};
    std::atomic<uint32_t> inject_fail{0};
    static constexpr size_t kInjectWaitSamples = 4;
    std::atomic<uint32_t> inject_wait_us[kInjectWaitSamples]{};
    std::atomic<uint32_t> inject_wait_count{0};
    std::atomic<uint32_t> inject_wait_next{0};
    std::atomic<uint16_t> pending_seq[kPendingCap]{};
    std::atomic<uint64_t> pending_t0_us[kPendingCap]{};
    std::atomic<uint8_t> pending_used[kPendingCap]{};
    std::atomic<uint8_t> pending_hol[kPendingCap]{};
    std::atomic<uint32_t> in_flight{0};
    std::atomic<uint32_t> max_in_flight_cap_{WIFI_RADIO_MAX_IN_FLIGHT};
    std::atomic<uint64_t> last_done_us{0};
    std::atomic<uint32_t> latency_us[kLatencySamples]{};
    std::atomic<uint32_t> latency_count{0};
    std::atomic<uint32_t> latency_next{0};
};

#endif  // WINJECT_WIFI_TX_H_
