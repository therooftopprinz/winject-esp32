#ifndef WINJECT_WIFI_H_
#define WINJECT_WIFI_H_

#include "config.h"
#include "indicator_led.h"
#include "wifi_rx.h"
#include "wifi_tx.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "bfc-esp32/semaphore.hpp"

struct wifi_status_s
{
    uint8_t channel;
    const char* modulation;
    bool cca_enabled;
    bool allow_failed_crc;
    int8_t tx_power_dbm;
    uint16_t domain;
    uint32_t udp_tx_pkt;
    uint32_t drop_tx_nomem;
    uint32_t tx_retry_count;
    uint32_t tx_retry_nomem;
    uint32_t tx_retry_other;
    uint32_t inject_ok;
    uint32_t inject_fail;
    uint16_t tx_in_flight;
    bool inject_wait_valid;
    uint32_t inject_wait_us;
    uint32_t udp_rx_pkt;
    uint32_t drop_crc_error;
    uint32_t drop_rx_no_pkt_pool;
    uint32_t drop_rx_queue_full;
    uint32_t udp_fwd_pkt;
    uint16_t tx_queue;
    uint16_t rx_queue;
    bool rx_air_valid;
    const char* rx_modulation;
    int8_t rx_rssi;
    int8_t rx_snr;
    bool tx_latency_valid;
    uint32_t tx_latency_us;
};

class wifi
{
    friend class wifi_tx;
    friend class wifi_rx;

public:
    static wifi& instance();
    wifi(const wifi&) = delete;
    wifi& operator=(const wifi&) = delete;

    bool initialize();
    bool ready() const;
    bool set_channel(uint8_t channel);
    bool set_modulation(const char* name);
    void get_status(wifi_status_s* status);
    static const char* modulation_list();
    static const char* format_phy(uint8_t sig_mode, uint8_t rate, uint8_t mcs,
                                  bool sgi);

    bool set_domain(uint16_t domain);
    uint16_t domain() const;

    void note_udp_tx_pkt();
    void note_udp_fwd_pkt();
    bool set_cca_enabled(bool enabled);
    bool set_tx_power(int8_t dbm);
    bool set_allow_failed_crc(bool allow);

    wifi_tx& tx();
    wifi_rx& rx();

private:
    wifi();

    bfc::semaphore& lock();
    void pulse_tx_led();
    void pulse_rx_led();
    esp_err_t apply_country();
    void init_activity_leds();
    bool apply_channel();
    bool apply_modulation();

    wifi_tx tx_;
    wifi_rx rx_;
    bfc::semaphore lock_;
    uint8_t channel_ = WIFI_DEFAULT_CHANNEL;
    const char* modulation_name_ = WIFI_DEFAULT_MODULATION;
    wifi_phy_rate_t modulation_rate_ = WIFI_PHY_RATE_1M_L;
    std::atomic<bool> ready_{false};
    indicator_led rx_led_;
    indicator_led tx_led_;
};

#endif  // WINJECT_WIFI_H_
