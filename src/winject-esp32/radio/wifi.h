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
    uint32_t udp_tx_pkt;
    uint32_t udp_tx_failed;
    uint32_t udp_rx_pkt;
    uint32_t udp_rx_crc_err;
    uint32_t udp_rx_dropped;
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
    using tx_slot_s = wifi_tx::slot_s;

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

    void note_udp_tx_pkt();
    void note_udp_fwd_pkt();
    // Default wait=0: inject must not block holding upstream locks / starve
    // the console reactor when the TX pool is empty.
    tx_slot_s* take_tx(TickType_t wait = 0);
    bool post_tx(tx_slot_s* slot, TickType_t wait = 0);
    void release_tx(tx_slot_s* slot);
    bool inject(const uint8_t* frame, size_t len);
    bool set_cca_enabled(bool enabled);
    bool set_tx_power(int8_t dbm);

    bool pop_rx(uint8_t* out, size_t* len, size_t max_len, TickType_t wait = 0);
    bool set_allow_failed_crc(bool allow);

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
