#include "wifi.h"

#include "config.h"

#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bfc-esp32/semaphore.hpp"

static const char* TAG = "wifi";

struct modulation_entry_s
{
    const char* name;
    wifi_phy_rate_t rate;
};

static const modulation_entry_s k_modulations[] = {
    {"DSS_1M_L", WIFI_PHY_RATE_1M_L},
    {"DSS_2M_S", WIFI_PHY_RATE_2M_S},
    {"DSS_2M_L", WIFI_PHY_RATE_2M_L},
    {"CCK_5M_L", WIFI_PHY_RATE_5M_L},
    {"CCK_5M_S", WIFI_PHY_RATE_5M_S},
    {"CCK_11M_L", WIFI_PHY_RATE_11M_L},
    {"CCK_11M_S", WIFI_PHY_RATE_11M_S},
    {"OFDM_6M", WIFI_PHY_RATE_6M},
    {"OFDM_9M", WIFI_PHY_RATE_9M},
    {"OFDM_12M", WIFI_PHY_RATE_12M},
    {"OFDM_18M", WIFI_PHY_RATE_18M},
    {"OFDM_24M", WIFI_PHY_RATE_24M},
    {"OFDM_36M", WIFI_PHY_RATE_36M},
    {"OFDM_48M", WIFI_PHY_RATE_48M},
    {"OFDM_54M", WIFI_PHY_RATE_54M},
    {"OFDM_MCS0_LGI", WIFI_PHY_RATE_MCS0_LGI},
    {"OFDM_MCS1_LGI", WIFI_PHY_RATE_MCS1_LGI},
    {"OFDM_MCS2_LGI", WIFI_PHY_RATE_MCS2_LGI},
    {"OFDM_MCS3_LGI", WIFI_PHY_RATE_MCS3_LGI},
    {"OFDM_MCS4_LGI", WIFI_PHY_RATE_MCS4_LGI},
    {"OFDM_MCS5_LGI", WIFI_PHY_RATE_MCS5_LGI},
    {"OFDM_MCS6_LGI", WIFI_PHY_RATE_MCS6_LGI},
    {"OFDM_MCS7_LGI", WIFI_PHY_RATE_MCS7_LGI},
    {"OFDM_MCS0_SGI", WIFI_PHY_RATE_MCS0_SGI},
    {"OFDM_MCS1_SGI", WIFI_PHY_RATE_MCS1_SGI},
    {"OFDM_MCS2_SGI", WIFI_PHY_RATE_MCS2_SGI},
    {"OFDM_MCS3_SGI", WIFI_PHY_RATE_MCS3_SGI},
    {"OFDM_MCS4_SGI", WIFI_PHY_RATE_MCS4_SGI},
    {"OFDM_MCS5_SGI", WIFI_PHY_RATE_MCS5_SGI},
    {"OFDM_MCS6_SGI", WIFI_PHY_RATE_MCS6_SGI},
    {"OFDM_MCS7_SGI", WIFI_PHY_RATE_MCS7_SGI},
};

static const modulation_entry_s* find_modulation(const char* name)
{
    if (name == nullptr)
    {
        return nullptr;
    }

    for (const auto& entry : k_modulations)
    {
        if (strcasecmp(entry.name, name) == 0)
        {
            return &entry;
        }
    }

    return nullptr;
}

static bool is_11b_rate(wifi_phy_rate_t rate)
{
    switch (rate)
    {
        case WIFI_PHY_RATE_1M_L:
        case WIFI_PHY_RATE_2M_L:
        case WIFI_PHY_RATE_5M_L:
        case WIFI_PHY_RATE_11M_L:
        case WIFI_PHY_RATE_2M_S:
        case WIFI_PHY_RATE_5M_S:
        case WIFI_PHY_RATE_11M_S:
            return true;
        default:
            return false;
    }
}

static bool channel_in_range(uint8_t channel)
{
    return channel >= WIFI_CHANNEL_MIN && channel <= WIFI_CHANNEL_MAX;
}

static bool modulation_ok_for_channel(wifi_phy_rate_t rate, uint8_t channel)
{
    return channel != 14 || is_11b_rate(rate);
}

static uint8_t protocol_for_rate(wifi_phy_rate_t rate, uint8_t channel)
{
    if (channel == 14)
    {
        return WIFI_PROTOCOL_11B;
    }

    // Promisc peer RX for legacy 64-QAM (48M/54M) needs 11n in the STA protocol
    // mask. esp_wifi_config_80211_tx_rate() can clamp phymode to 11b|11g unless
    // we re-apply the mask after the rate (see apply_modulation).
    (void)rate;
    return WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
}

wifi& wifi::instance()
{
    static wifi inst;
    return inst;
}

wifi::wifi()
    : tx_(*this),
      rx_(*this),
      rx_led(WIFI_RX_LED_GPIO),
      tx_led(WIFI_TX_LED_GPIO)
{
}

void wifi::pulse_tx_led()
{
    tx_led.pulse();
}

void wifi::pulse_rx_led()
{
    rx_led.pulse();
}

void wifi::init_activity_leds()
{
    const bool rx = rx_led.init();
    const bool tx = tx_led.init();
    if (!rx && !tx)
    {
        if (rx_led.enabled() || tx_led.enabled())
        {
            ESP_LOGW(TAG, "activity LED gpio_config failed");
        }
        return;
    }

    if (!indicator_led::start_poll())
    {
        ESP_LOGW(TAG, "activity LED poll timer failed");
    }

    if (rx && tx)
    {
        ESP_LOGI(TAG, "activity LEDs RX=IO%d TX=IO%d (active-low, %u us)",
                 rx_led.gpio(), tx_led.gpio(), WIFI_LED_STRETCH_US);
    }
    else if (rx)
    {
        ESP_LOGI(TAG, "activity LED RX=IO%d; TX LED disabled", rx_led.gpio());
    }
    else
    {
        ESP_LOGI(TAG, "activity LED TX=IO%d; RX LED disabled", tx_led.gpio());
    }
}

esp_err_t wifi::apply_country()
{
    wifi_country_t country = {};
    country.max_tx_power = static_cast<int8_t>(tx_.power_dbm() * 4);
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;

    // JP PHY includes 2484 MHz (channel 14). Fall back to world-safe "01"
    // (channels 1–13) if the radio rejects JP — never abort boot on this.
    memcpy(country.cc, "JP", 2);
    country.schan = WIFI_CHANNEL_MIN;
    country.nchan = WIFI_CHANNEL_MAX - WIFI_CHANNEL_MIN + 1;
    esp_err_t err = esp_wifi_set_country(&country);
    if (err == ESP_OK)
    {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "country JP failed (%s), falling back to 01",
             esp_err_to_name(err));
    memcpy(country.cc, "01", 2);
    country.schan = 1;
    country.nchan = 13;
    return esp_wifi_set_country(&country);
}

bool wifi::apply_sta_decode_protocol()
{
    const uint8_t proto = protocol_for_rate(modulation_rate, channel);
    const esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, proto);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_protocol failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool wifi::apply_channel()
{
    const esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_channel %u failed: %s", channel,
                 esp_err_to_name(err));
        return false;
    }

    return true;
}

bool wifi::apply_modulation()
{
    if (!modulation_ok_for_channel(modulation_rate, channel))
    {
        ESP_LOGE(TAG, "channel %u is 802.11b only", channel);
        return false;
    }

    if (!apply_sta_decode_protocol())
    {
        return false;
    }

    esp_err_t err = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, modulation_rate);
    if (err == ESP_OK)
    {
        (void)esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
        (void)apply_sta_decode_protocol();
        (void)tx_.apply_power();
        return true;
    }

    err = esp_wifi_stop();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "stop for rate failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, modulation_rate);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "config_80211_tx_rate failed: %s", esp_err_to_name(err));
        esp_wifi_start();
        return false;
    }

    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "start after rate failed: %s", esp_err_to_name(err));
        return false;
    }

    tx_.apply_power();
    tx_.apply_cca();
    tx_.apply_tx_done_cb();
    (void)esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
    (void)apply_sta_decode_protocol();
    (void)tx_.apply_power();
    return true;
}

bool wifi::initialize()
{
    const modulation_entry_s* def = find_modulation(WIFI_DEFAULT_MODULATION);
    if (def != nullptr)
    {
        modulation_name = def->name;
        modulation_rate = def->rate;
    }

    init_activity_leds();

    if (!lock.init())
    {
        ESP_LOGE(TAG, "radio lock alloc failed");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // HT MCS inject is still one MPDU per datagram (TX AMPDU off). RX AMPDU/BA
    // must stay on or promisc never sees MCS4/7 peer frames (legacy OFDM OK).
    cfg.ampdu_rx_enable = 1;
    cfg.ampdu_tx_enable = 0;
    cfg.amsdu_tx_enable = 0;
    if (esp_netif_create_default_wifi_sta() == nullptr)
    {
        ESP_LOGE(TAG, "STA netif alloc failed");
        return false;
    }

    if (esp_wifi_init(&cfg) != ESP_OK ||
        esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
        esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi init/mode failed");
        return false;
    }
    if (apply_country() != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi country failed");
        return false;
    }
    if (esp_wifi_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "wifi start failed");
        return false;
    }
    esp_wifi_disconnect();
    if (!tx_.apply_power())
    {
        ESP_LOGW(TAG, "default tx power %d dBm not applied", tx_.power_dbm());
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    if (!apply_modulation() || !apply_channel() || !rx_.apply_monitor() ||
        !tx_.apply_cca())
    {
        return false;
    }
    if (!tx_.apply_tx_done_cb())
    {
        ESP_LOGW(TAG, "tx_latency unavailable (tx done callback not registered)");
    }

    ready_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "monitor/inject ch %u rate %s (core %d) AMPDU rx on tx off",
             channel, modulation_name, WIFI_RADIO_TASK_CORE);
    return true;
}

bool wifi::ready() const
{
    return ready_.load(std::memory_order_acquire);
}

bool wifi::set_channel(uint8_t channel)
{
    if (!ready_.load(std::memory_order_acquire))
    {
        return false;
    }

    if (!channel_in_range(channel))
    {
        return false;
    }

    bfc::semaphore::lock guard(lock, pdMS_TO_TICKS(1000));
    if (!guard)
    {
        return false;
    }

    if (!modulation_ok_for_channel(modulation_rate, channel))
    {
        ESP_LOGW(TAG, "channel 14 requires DSSS/CCK");
        return false;
    }

    const uint8_t previous = this->channel;
    this->channel = channel;
    bool ok = true;
    if (channel == 14)
    {
        ok = apply_modulation();
    }
    if (ok)
    {
        ok = apply_channel();
    }
    if (!ok)
    {
        this->channel = previous;
        apply_modulation();
        apply_channel();
    }

    return ok;
}

bool wifi::set_modulation(const char* name)
{
    if (!ready_.load(std::memory_order_acquire))
    {
        return false;
    }

    const modulation_entry_s* entry = find_modulation(name);
    if (entry == nullptr)
    {
        return false;
    }

    bfc::semaphore::lock guard(lock, pdMS_TO_TICKS(2000));
    if (!guard)
    {
        return false;
    }

    if (!modulation_ok_for_channel(entry->rate, channel))
    {
        ESP_LOGW(TAG, "channel 14 rejects OFDM");
        return false;
    }

    const char* previous_name = modulation_name;
    const wifi_phy_rate_t previous_rate = modulation_rate;
    modulation_name = entry->name;
    modulation_rate = entry->rate;
    bool ok = apply_modulation() && apply_channel() && rx_.apply_monitor();
    if (!ok)
    {
        modulation_name = previous_name;
        modulation_rate = previous_rate;
        apply_modulation();
        apply_channel();
        rx_.apply_monitor();
    }

    return ok;
}

void wifi::get_status(wifi_status_s* status)
{
    if (status == nullptr)
    {
        return;
    }

    {
        bfc::semaphore::lock guard(lock, pdMS_TO_TICKS(50));
        status->channel = channel;
        status->modulation = modulation_name;
        tx_.fill_status(status);
    }

    rx_.fill_status(status);
}

const char* wifi::modulation_list()
{
    return "DSS_1M_L DSS_2M_S DSS_2M_L CCK_5M_L CCK_5M_S CCK_11M_L CCK_11M_S "
           "OFDM_6M OFDM_9M OFDM_12M OFDM_18M OFDM_24M OFDM_36M OFDM_48M "
           "OFDM_54M "
           "OFDM_MCS0_LGI OFDM_MCS1_LGI OFDM_MCS2_LGI OFDM_MCS3_LGI "
           "OFDM_MCS4_LGI OFDM_MCS5_LGI OFDM_MCS6_LGI OFDM_MCS7_LGI "
           "OFDM_MCS0_SGI OFDM_MCS1_SGI OFDM_MCS2_SGI OFDM_MCS3_SGI "
           "OFDM_MCS4_SGI OFDM_MCS5_SGI OFDM_MCS6_SGI OFDM_MCS7_SGI";
}

const char* wifi::format_phy(uint8_t sig_mode, uint8_t rate, uint8_t mcs,
                             bool sgi)
{
    wifi_phy_rate_t phy;
    if (sig_mode == 1)
    {
        if (mcs > 7)
        {
            return "UNKNOWN";
        }
        phy = static_cast<wifi_phy_rate_t>(
            (sgi ? WIFI_PHY_RATE_MCS0_SGI : WIFI_PHY_RATE_MCS0_LGI) + mcs);
    }
    else
    {
        phy = static_cast<wifi_phy_rate_t>(rate);
    }

    for (const auto& entry : k_modulations)
    {
        if (entry.rate == phy)
        {
            return entry.name;
        }
    }

    return "UNKNOWN";
}

void wifi::note_udp_tx_pkt()
{
    tx_.note_udp_tx_pkt();
}

void wifi::note_udp_fwd_pkt()
{
    rx_.note_udp_fwd_pkt();
}

bool wifi::set_domain(uint16_t domain)
{
    return tx_.set_domain(domain) && rx_.set_domain(domain);
}

uint16_t wifi::domain() const
{
    return tx_.domain();
}

wifi_tx& wifi::tx()
{
    return tx_;
}

wifi_rx& wifi::rx()
{
    return rx_;
}

bool wifi::set_cca_enabled(bool enabled)
{
    return tx_.set_cca_enabled(enabled);
}

bool wifi::set_tx_power(int8_t dbm)
{
    return tx_.set_tx_power(dbm);
}

bool wifi::set_allow_failed_crc(bool allow)
{
    return rx_.set_allow_failed_crc(allow);
}

void wifi::reset_promisc_stats()
{
    rx_.reset_promisc_stats();
}

void wifi::set_tx_dry_run(bool enabled)
{
    tx_.set_dry_run(enabled);
}

bool wifi::tx_dry_run() const
{
    return tx_.dry_run();
}
