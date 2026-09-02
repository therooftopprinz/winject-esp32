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
#include "winject/semaphore.h"

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

static uint8_t protocol_for_rate(wifi_phy_rate_t rate)
{
    if (rate >= WIFI_PHY_RATE_MCS0_LGI)
    {
        return WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
    }

    if (rate >= WIFI_PHY_RATE_48M)
    {
        return WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G;
    }

    return WIFI_PROTOCOL_11B;
}

wifi& wifi::instance()
{
    static wifi inst;
    return inst;
}

wifi::wifi()
    : tx_(*this),
      rx_(*this),
      rx_led_(WIFI_RX_LED_GPIO),
      tx_led_(WIFI_TX_LED_GPIO)
{
}

semaphore& wifi::lock()
{
    return lock_;
}

void wifi::pulse_tx_led()
{
    tx_led_.pulse();
}

void wifi::pulse_rx_led()
{
    rx_led_.pulse();
}

void wifi::init_activity_leds()
{
    const bool rx = rx_led_.init();
    const bool tx = tx_led_.init();
    if (!rx && !tx)
    {
        if (rx_led_.enabled() || tx_led_.enabled())
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
                 rx_led_.gpio(), tx_led_.gpio(), WIFI_LED_STRETCH_US);
    }
    else if (rx)
    {
        ESP_LOGI(TAG, "activity LED RX=IO%d; TX LED disabled", rx_led_.gpio());
    }
    else
    {
        ESP_LOGI(TAG, "activity LED TX=IO%d; RX LED disabled", tx_led_.gpio());
    }
}

esp_err_t wifi::apply_country()
{
    wifi_country_t country = {};
    memcpy(country.cc, "01", 2);
    country.schan = 1;
    country.nchan = 13;
    country.max_tx_power = static_cast<int8_t>(tx_.power_dbm() * 4);
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;
    return esp_wifi_set_country(&country);
}

bool wifi::apply_channel()
{
    const esp_err_t err = esp_wifi_set_channel(channel_, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_channel %u failed: %s", channel_,
                 esp_err_to_name(err));
        return false;
    }

    return true;
}

bool wifi::apply_modulation()
{
    const uint8_t proto = protocol_for_rate(modulation_rate_);
    esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, proto);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "set_protocol failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, modulation_rate_);
    if (err == ESP_OK)
    {
        return true;
    }

    err = esp_wifi_stop();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "stop for rate failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, modulation_rate_);
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
    return true;
}

bool wifi::initialize()
{
    const modulation_entry_s* def = find_modulation(WIFI_DEFAULT_MODULATION);
    if (def != nullptr)
    {
        modulation_name_ = def->name;
        modulation_rate_ = def->rate;
    }

    init_activity_leds();

    if (!lock_.init() || !tx_.init() || !rx_.init())
    {
        ESP_LOGE(TAG, "radio queue alloc failed");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.ampdu_rx_enable = 0;
    cfg.ampdu_tx_enable = 0;
    cfg.amsdu_tx_enable = 0;
    cfg.rx_ba_win = 0;
    if (esp_netif_create_default_wifi_sta() == nullptr)
    {
        ESP_LOGE(TAG, "STA netif alloc failed");
        return false;
    }

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(apply_country());
    ESP_ERROR_CHECK(esp_wifi_start());
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

    if (!tx_.start_task())
    {
        return false;
    }

    ready_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "monitor/inject on channel %u rate %s (core %d) AMPDU off",
             channel_, modulation_name_, WIFI_RADIO_TASK_CORE);
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

    if (channel < 1 || channel > 13)
    {
        return false;
    }

    semaphore::lock guard(lock_, pdMS_TO_TICKS(1000));
    if (!guard)
    {
        return false;
    }

    const uint8_t previous = channel_;
    channel_ = channel;
    const bool ok = apply_channel();
    if (!ok)
    {
        channel_ = previous;
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

    semaphore::lock guard(lock_, pdMS_TO_TICKS(2000));
    if (!guard)
    {
        return false;
    }

    const char* previous_name = modulation_name_;
    const wifi_phy_rate_t previous_rate = modulation_rate_;
    modulation_name_ = entry->name;
    modulation_rate_ = entry->rate;
    bool ok = apply_modulation() && apply_channel() && rx_.apply_monitor();
    if (!ok)
    {
        modulation_name_ = previous_name;
        modulation_rate_ = previous_rate;
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
        semaphore::lock guard(lock_, pdMS_TO_TICKS(50));
        status->channel = channel_;
        status->modulation = modulation_name_;
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

wifi::tx_slot_s* wifi::take_tx()
{
    return tx_.take();
}

bool wifi::post_tx(tx_slot_s* slot)
{
    return tx_.post(slot);
}

void wifi::release_tx(tx_slot_s* slot)
{
    tx_.release(slot);
}

bool wifi::inject(const uint8_t* frame, size_t len)
{
    return tx_.inject(frame, len);
}

bool wifi::set_cca_enabled(bool enabled)
{
    return tx_.set_cca_enabled(enabled);
}

bool wifi::set_tx_power(int8_t dbm)
{
    return tx_.set_tx_power(dbm);
}

bool wifi::pop_rx(uint8_t* out, size_t* len, size_t max_len)
{
    return rx_.pop(out, len, max_len);
}

bool wifi::set_allow_failed_crc(bool allow)
{
    return rx_.set_allow_failed_crc(allow);
}
