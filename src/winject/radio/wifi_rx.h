#ifndef WINJECT_WIFI_RX_H_
#define WINJECT_WIFI_RX_H_

#include "config.h"
#include "packet_pool.h"
#include "packet_queue.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "esp_wifi_types.h"

class wifi;
struct wifi_status_s;

class wifi_rx
{
    friend class wifi;

public:
    using pool_t = packet_pool<WIFI_RADIO_MAX_FRAME, WIFI_RADIO_RX_QUEUE>;
    using slot_s = pool_t::slot_s;

    wifi_rx(const wifi_rx&) = delete;
    wifi_rx& operator=(const wifi_rx&) = delete;

private:
    explicit wifi_rx(wifi& radio);

    bool init();
    bool apply_monitor();
    void fill_status(wifi_status_s* status);
    bool pop(uint8_t* out, size_t* len, size_t max_len);
    bool set_allow_failed_crc(bool allow);

    static void promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type);
    static int8_t clamp_i8(int value);
    void note_air(const wifi_pkt_rx_ctrl_t& ctrl);
    void on_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type);

    wifi& radio_;
    pool_t pool_;
    packet_queue<slot_s, WIFI_RADIO_RX_QUEUE> queue_;
    std::atomic<bool> allow_failed_crc_{false};
    std::atomic<uint32_t> udp_rx_pkt_{0};
    std::atomic<uint32_t> udp_rx_crc_err_{0};
    std::atomic<uint32_t> udp_rx_dropped_{0};
    std::atomic<bool> air_valid_{false};
    std::atomic<const char*> modulation_{nullptr};
    std::atomic<int8_t> rssi_{0};
    std::atomic<int8_t> snr_{0};
};

#endif  // WINJECT_WIFI_RX_H_
