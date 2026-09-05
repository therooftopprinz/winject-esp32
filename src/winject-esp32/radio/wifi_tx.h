#ifndef WINJECT_WIFI_TX_H_
#define WINJECT_WIFI_TX_H_

#include "config.h"
#include "packet_pool.h"
#include "packet_queue.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "esp_wifi_types.h"

class wifi;
struct wifi_status_s;

class wifi_tx
{
    friend class wifi;

public:
    using pool_t = packet_pool<WIFI_RADIO_INJECT_MAX, WIFI_RADIO_TX_QUEUE>;
    using slot_s = pool_t::slot_s;

    wifi_tx(const wifi_tx&) = delete;
    wifi_tx& operator=(const wifi_tx&) = delete;

private:
    explicit wifi_tx(wifi& radio);

    bool init();
    bool apply_power();
    bool apply_cca();
    bool apply_tx_done_cb();
    bool start_task();
    void fill_status(wifi_status_s* status);
    int8_t power_dbm() const;

    void note_udp_tx_pkt();
    // wait=0: never block the caller (inject path). portMAX_DELAY only for
    // the wifi_tx task pulling from its own queue.
    slot_s* take(TickType_t wait = 0);
    bool post(slot_s* slot, TickType_t wait = 0);
    void release(slot_s* slot);
    bool inject(const uint8_t* frame, size_t len);
    bool set_cca_enabled(bool enabled);
    bool set_tx_power(int8_t dbm);

    static void task(void* arg);
    void run();
    static bool seq_of(const uint8_t* frame, size_t len, uint16_t* seq);
    void note_submit(uint16_t seq);
    void cancel_submit(uint16_t seq);
    void note_done(uint16_t seq);
    void record_latency(uint32_t us);
    static void on_tx_done(const esp_80211_tx_info_t* info);

    static constexpr size_t kPendingCap = 64;
    static constexpr size_t kLatencySamples = 4;

    wifi& radio_;
    pool_t pool_;
    packet_queue<slot_s, WIFI_RADIO_TX_QUEUE> queue_;
    bool cca_enabled_ = true;
    bool phy_cca_off_ = false;
    int8_t tx_power_dbm_ = WIFI_DEFAULT_TX_POWER_DBM;
    std::atomic<uint32_t> udp_tx_pkt_{0};
    std::atomic<uint32_t> udp_tx_failed_{0};
    std::atomic<uint16_t> pending_seq_[kPendingCap]{};
    std::atomic<uint64_t> pending_t0_us_[kPendingCap]{};
    std::atomic<uint8_t> pending_used_[kPendingCap]{};
    std::atomic<uint8_t> pending_hol_[kPendingCap]{};
    std::atomic<uint32_t> in_flight_{0};
    std::atomic<uint64_t> last_done_us_{0};
    std::atomic<uint32_t> latency_us_[kLatencySamples]{};
    std::atomic<uint32_t> latency_count_{0};
    std::atomic<uint32_t> latency_next_{0};
};

#endif  // WINJECT_WIFI_TX_H_
