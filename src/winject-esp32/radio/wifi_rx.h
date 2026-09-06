#ifndef WINJECT_WIFI_RX_H_
#define WINJECT_WIFI_RX_H_

#include "config.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "esp_wifi_types.h"

class wifi;
class lc_rx;
struct wifi_status_s;

class wifi_rx
{
    friend class wifi;

public:
    wifi_rx(const wifi_rx&) = delete;
    wifi_rx& operator=(const wifi_rx&) = delete;

    bool init(lc_rx& rx);

private:
    explicit wifi_rx(wifi& radio);

    bool apply_monitor();
    void fill_status(wifi_status_s* status);
    bool set_allow_failed_crc(bool allow);
    void note_udp_fwd_pkt();

    bool set_domain(uint16_t domain);
    uint16_t domain() const;

    static void promiscuous_cb(void* buf, wifi_promiscuous_pkt_type_t type);
    static int8_t clamp_i8(int value);
    void note_air(const wifi_pkt_rx_ctrl_t& ctrl);
    void on_promiscuous(void* buf, wifi_promiscuous_pkt_type_t type);
    bool accept_mpdu(const uint8_t* mpdu, size_t len) const;

    wifi& radio_;
    lc_rx* rx_ = nullptr;
    std::atomic<uint16_t> domain_{0};
    std::atomic<bool> allow_failed_crc_{false};
    std::atomic<uint32_t> udp_rx_pkt_{0};
    std::atomic<uint32_t> drop_crc_error_{0};
    std::atomic<uint32_t> drop_rx_no_pkt_pool_{0};
    std::atomic<uint32_t> drop_rx_queue_full_{0};
    std::atomic<uint32_t> udp_fwd_pkt_{0};
    std::atomic<bool> air_valid_{false};
    std::atomic<const char*> modulation_{nullptr};
    std::atomic<int8_t> rssi_{0};
    std::atomic<int8_t> snr_{0};
};

#endif  // WINJECT_WIFI_RX_H_
