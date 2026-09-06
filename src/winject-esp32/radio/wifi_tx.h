#ifndef WINJECT_WIFI_TX_H_
#define WINJECT_WIFI_TX_H_

#include "config.h"
#include "packet.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"

class wifi;
class lc_tx;
struct wifi_status_s;

class wifi_tx
{
    friend class wifi;

public:
    wifi_tx(const wifi_tx&) = delete;
    wifi_tx& operator=(const wifi_tx&) = delete;

    bool init(lc_tx& tx);
    bool start(BaseType_t core = WIFI_RADIO_TASK_CORE,
               UBaseType_t prio = WIFI_RADIO_TASK_PRIO,
               uint32_t stack_bytes = 6144);

private:
    explicit wifi_tx(wifi& radio);

    void run();
    bool inject_retry(const uint8_t* frame, size_t len);
    void stamp(packet& out, const pdu_slot_t slots[WIFI_PDU_SLOTS]);

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

    wifi& radio_;
    lc_tx* tx_ = nullptr;
    std::atomic<uint16_t> domain_{0};
    bool cca_enabled_ = true;
    bool phy_cca_off_ = false;
    int8_t tx_power_dbm_ = WIFI_DEFAULT_TX_POWER_DBM;
    std::atomic<uint32_t> udp_tx_pkt_{0};
    std::atomic<uint32_t> drop_tx_nomem_{0};
    std::atomic<uint32_t> tx_retry_count_{0};
    std::atomic<uint32_t> tx_retry_nomem_{0};
    std::atomic<uint32_t> tx_retry_other_{0};
    std::atomic<uint32_t> inject_ok_{0};
    std::atomic<uint32_t> inject_fail_{0};
    static constexpr size_t kInjectWaitSamples = 4;
    std::atomic<uint32_t> inject_wait_us_[kInjectWaitSamples]{};
    std::atomic<uint32_t> inject_wait_count_{0};
    std::atomic<uint32_t> inject_wait_next_{0};
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
