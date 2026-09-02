#ifndef WINJECT_WIFI_TX_H_
#define WINJECT_WIFI_TX_H_

#include "config.h"
#include "packet_pool.h"
#include "packet_queue.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

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
    bool start_task();
    void fill_status(wifi_status_s* status);
    int8_t power_dbm() const;

    void note_udp_tx_pkt();
    slot_s* take();
    bool post(slot_s* slot);
    void release(slot_s* slot);
    bool inject(const uint8_t* frame, size_t len);
    bool set_cca_enabled(bool enabled);
    bool set_tx_power(int8_t dbm);

    static void task(void* arg);
    void run();

    wifi& radio_;
    pool_t pool_;
    packet_queue<slot_s, WIFI_RADIO_TX_QUEUE> queue_;
    bool cca_enabled_ = true;
    bool phy_cca_off_ = false;
    int8_t tx_power_dbm_ = WIFI_DEFAULT_TX_POWER_DBM;
    std::atomic<uint32_t> udp_tx_pkt_{0};
    std::atomic<uint32_t> udp_tx_dropped_{0};
    std::atomic<uint32_t> udp_tx_failed_{0};
};

#endif  // WINJECT_WIFI_TX_H_
