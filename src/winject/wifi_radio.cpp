#include "wifi_radio.h"

#include "config.h"
#include "frame.h"

#include <atomic>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "wifi";

extern "C" int ieee80211_raw_frame_sanity_check(int32_t, int32_t, uint32_t,
                                                uint32_t)
{
  return 0;
}

extern "C"
{
  int hal_mac_tx_set_cca(int enable);
  void esp_rom_phy_disable_cca(void) __attribute__((weak));
  void phy_disable_cca(void) __attribute__((weak));
  void phy_enable_cca(void) __attribute__((weak));
}

struct ModulationEntry
{
  const char* name;
  wifi_phy_rate_t rate;
};

static const ModulationEntry kModulations[] = {
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

struct RxSlot
{
  uint16_t len;
  uint8_t data[WIFI_RADIO_MAX_FRAME];
};

struct TxSlot
{
  uint16_t len;
  uint8_t data[WIFI_RADIO_INJECT_MAX];
};

static RxSlot g_rxPool[WIFI_RADIO_RX_QUEUE];
static TxSlot g_txPool[WIFI_RADIO_TX_QUEUE];
static QueueHandle_t g_rxFree = nullptr;
static QueueHandle_t g_rxFilled = nullptr;
static QueueHandle_t g_txFree = nullptr;
static QueueHandle_t g_txFilled = nullptr;
static SemaphoreHandle_t g_radioLock = nullptr;
static uint8_t g_channel = WIFI_DEFAULT_CHANNEL;
static const char* g_modulationName = WIFI_DEFAULT_MODULATION;
static wifi_phy_rate_t g_modulationRate = WIFI_PHY_RATE_1M_L;
static bool g_ccaEnabled = true;
static bool g_radioReady = false;
static bool g_apActive = false;
static char g_apSsid[32] = {};
static esp_netif_t* g_apNetif = nullptr;
static std::atomic<uint32_t> g_wifiRx{0};
static std::atomic<uint32_t> g_wifiRxDropped{0};
static uint32_t g_wifiTx = 0;
static uint32_t g_wifiTxFail = 0;

static const ModulationEntry* findModulation(const char* name)
{
  if (name == nullptr)
  {
    return nullptr;
  }
  for (const auto& entry : kModulations)
  {
    if (strcasecmp(entry.name, name) == 0)
    {
      return &entry;
    }
  }
  return nullptr;
}

static uint8_t protocolForRate(wifi_phy_rate_t rate)
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

static void applyCountry()
{
  wifi_country_t country = {};
  memcpy(country.cc, "01", 2);
  country.schan = 1;
  country.nchan = 13;
  country.max_tx_power = 20;
  country.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_wifi_set_country(&country);
}

static void formatApSsid(char* out, size_t outLen)
{
  uint8_t mac[6] = {};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  snprintf(out, outLen, "winject-%02X%02X%02X%02X%02X%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

static bool applyChannel()
{
  const esp_err_t err = esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "set_channel %u failed: %s", g_channel, esp_err_to_name(err));
    return false;
  }
  return true;
}

static bool applyModulation()
{
  const uint8_t proto = protocolForRate(g_modulationRate);
  esp_err_t err = esp_wifi_set_protocol(WIFI_IF_STA, proto);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "set_protocol failed: %s", esp_err_to_name(err));
    return false;
  }
  if (g_apActive)
  {
    esp_wifi_set_protocol(WIFI_IF_AP, proto);
  }

  err = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, g_modulationRate);
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
  err = esp_wifi_config_80211_tx_rate(WIFI_IF_STA, g_modulationRate);
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
  return true;
}

static bool applyMonitor()
{
  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(
      [](void* buf, wifi_promiscuous_pkt_type_t type)
      {
        if (type != WIFI_PKT_DATA || buf == nullptr || g_rxFree == nullptr)
        {
          return;
        }

        const auto* pkt = static_cast<wifi_promiscuous_pkt_t*>(buf);
        int len = pkt->rx_ctrl.sig_len;
        if (len <= 4)
        {
          return;
        }
        len -= 4;
        if (len > WIFI_RADIO_MAX_FRAME)
        {
          len = WIFI_RADIO_MAX_FRAME;
        }
        if (!frameMatch(pkt->payload, static_cast<size_t>(len)))
        {
          return;
        }

        uint8_t idx = 0;
        if (xQueueReceive(g_rxFree, &idx, 0) != pdTRUE)
        {
          g_wifiRxDropped.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        g_rxPool[idx].len = static_cast<uint16_t>(len);
        memcpy(g_rxPool[idx].data, pkt->payload, g_rxPool[idx].len);
        if (xQueueSend(g_rxFilled, &idx, 0) != pdTRUE)
        {
          xQueueSend(g_rxFree, &idx, 0);
          g_wifiRxDropped.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        g_wifiRx.fetch_add(1, std::memory_order_relaxed);
      });

  const esp_err_t err = esp_wifi_set_promiscuous(true);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "promiscuous failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

static bool applyCca()
{
  const int err = hal_mac_tx_set_cca(g_ccaEnabled ? 1 : 0);
  if (err != 0)
  {
    ESP_LOGE(TAG, "hal_mac_tx_set_cca failed: %d", err);
    return false;
  }

  if (g_ccaEnabled)
  {
    if (phy_enable_cca)
    {
      phy_enable_cca();
    }
    return true;
  }

  if (esp_rom_phy_disable_cca)
  {
    esp_rom_phy_disable_cca();
  }
  else if (phy_disable_cca)
  {
    phy_disable_cca();
  }
  return true;
}

static bool lockRadio(TickType_t ticks)
{
  return g_radioLock != nullptr && xSemaphoreTake(g_radioLock, ticks) == pdTRUE;
}

static void unlockRadio()
{
  if (g_radioLock != nullptr)
  {
    xSemaphoreGive(g_radioLock);
  }
}

static void radioTxTask(void* arg)
{
  (void)arg;
  for (;;)
  {
    uint8_t idx = 0;
    if (xQueueReceive(g_txFilled, &idx, portMAX_DELAY) != pdTRUE)
    {
      continue;
    }
    if (!lockRadio(pdMS_TO_TICKS(50)))
    {
      g_wifiTxFail++;
      xQueueSend(g_txFree, &idx, 0);
      continue;
    }

    bool ok = false;
    for (int attempt = 0; attempt < WIFI_RADIO_INJECT_RETRIES; attempt++)
    {
      const esp_err_t err =
          esp_wifi_80211_tx(WIFI_IF_STA, g_txPool[idx].data,
                            static_cast<int>(g_txPool[idx].len), false);
      if (err == ESP_OK)
      {
        ok = true;
        break;
      }
      vTaskDelay(1);
    }
    unlockRadio();
    if (ok)
    {
      g_wifiTx++;
    }
    else
    {
      g_wifiTxFail++;
    }
    xQueueSend(g_txFree, &idx, 0);
  }
}

bool wifiRadioBegin()
{
  const ModulationEntry* def = findModulation(WIFI_DEFAULT_MODULATION);
  if (def != nullptr)
  {
    g_modulationName = def->name;
    g_modulationRate = def->rate;
  }

  g_radioLock = xSemaphoreCreateMutex();
  g_rxFree = xQueueCreate(WIFI_RADIO_RX_QUEUE, sizeof(uint8_t));
  g_rxFilled = xQueueCreate(WIFI_RADIO_RX_QUEUE, sizeof(uint8_t));
  g_txFree = xQueueCreate(WIFI_RADIO_TX_QUEUE, sizeof(uint8_t));
  g_txFilled = xQueueCreate(WIFI_RADIO_TX_QUEUE, sizeof(uint8_t));
  if (g_radioLock == nullptr || g_rxFree == nullptr || g_rxFilled == nullptr ||
      g_txFree == nullptr || g_txFilled == nullptr)
  {
    ESP_LOGE(TAG, "radio queue alloc failed");
    return false;
  }
  for (uint8_t i = 0; i < WIFI_RADIO_RX_QUEUE; i++)
  {
    xQueueSend(g_rxFree, &i, 0);
  }
  for (uint8_t i = 0; i < WIFI_RADIO_TX_QUEUE; i++)
  {
    xQueueSend(g_txFree, &i, 0);
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_netif_create_default_wifi_sta() == nullptr)
  {
    ESP_LOGE(TAG, "STA netif alloc failed");
    return false;
  }
  g_apNetif = esp_netif_create_default_wifi_ap();
  if (g_apNetif == nullptr)
  {
    ESP_LOGE(TAG, "AP netif alloc failed");
    return false;
  }

  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  applyCountry();
  ESP_ERROR_CHECK(esp_wifi_start());
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(50));

  if (!applyModulation() || !applyChannel() || !applyMonitor())
  {
    return false;
  }

  if (xTaskCreatePinnedToCore(radioTxTask, "wifi_tx", 4096, nullptr,
                              WIFI_RADIO_TASK_PRIO, nullptr,
                              WIFI_RADIO_TASK_CORE) != pdPASS)
  {
    ESP_LOGE(TAG, "wifi_tx task failed");
    return false;
  }

  g_radioReady = true;
  ESP_LOGI(TAG, "monitor/inject on channel %u rate %s (core %d)", g_channel,
           g_modulationName, WIFI_RADIO_TASK_CORE);
  return true;
}

bool wifiRadioReady()
{
  return g_radioReady;
}

bool wifiRadioStartApFallback()
{
  if (g_apActive)
  {
    return true;
  }

  formatApSsid(g_apSsid, sizeof(g_apSsid));
  const uint8_t apChannel = g_radioReady ? g_channel : WIFI_AP_CHANNEL;

  wifi_config_t ap = {};
  const size_t ssidLen = strlen(g_apSsid);
  if (ssidLen >= sizeof(ap.ap.ssid))
  {
    return false;
  }
  memcpy(ap.ap.ssid, g_apSsid, ssidLen);
  ap.ap.ssid_len = static_cast<uint8_t>(ssidLen);
  ap.ap.channel = apChannel;
  ap.ap.authmode = WIFI_AUTH_OPEN;
  ap.ap.max_connection = WIFI_AP_MAX_CLIENTS;
  ap.ap.beacon_interval = 100;

  if (!g_radioReady)
  {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  }
  else
  {
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  }
  const esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "AP fallback failed: %s", esp_err_to_name(err));
    g_apSsid[0] = '\0';
    return false;
  }

  if (g_radioReady)
  {
    if (lockRadio(pdMS_TO_TICKS(1000)))
    {
      applyModulation();
      applyChannel();
      applyMonitor();
      unlockRadio();
    }
  }

  g_apActive = true;
  uint32_t ip = 0;
  wifiRadioApIpv4(&ip);
  ESP_LOGI(TAG, "AP fallback SSID %s  IP " IPSTR "  (open)%s", g_apSsid,
           IP2STR((esp_ip4_addr_t*)&ip),
           g_radioReady ? "  radio still up" : "");
  return true;
}

bool wifiRadioApActive()
{
  return g_apActive;
}

const char* wifiRadioApSsid()
{
  return g_apSsid;
}

bool wifiRadioApIpv4(uint32_t* out)
{
  if (out == nullptr || !g_apActive || g_apNetif == nullptr)
  {
    return false;
  }
  esp_netif_ip_info_t info = {};
  if (esp_netif_get_ip_info(g_apNetif, &info) != ESP_OK || info.ip.addr == 0)
  {
    return false;
  }
  *out = info.ip.addr;
  return true;
}

bool wifiRadioSetChannel(uint8_t channel)
{
  if (!g_radioReady)
  {
    return false;
  }
  if (channel < 1 || channel > 13)
  {
    return false;
  }
  if (!lockRadio(pdMS_TO_TICKS(1000)))
  {
    return false;
  }
  const uint8_t previous = g_channel;
  g_channel = channel;
  const bool ok = applyChannel();
  if (!ok)
  {
    g_channel = previous;
  }
  unlockRadio();
  return ok;
}

bool wifiRadioSetModulation(const char* name)
{
  if (!g_radioReady)
  {
    return false;
  }
  const ModulationEntry* entry = findModulation(name);
  if (entry == nullptr)
  {
    return false;
  }
  if (!lockRadio(pdMS_TO_TICKS(2000)))
  {
    return false;
  }
  const char* previousName = g_modulationName;
  const wifi_phy_rate_t previousRate = g_modulationRate;
  g_modulationName = entry->name;
  g_modulationRate = entry->rate;
  bool ok = applyModulation() && applyChannel() && applyMonitor();
  if (!ok)
  {
    g_modulationName = previousName;
    g_modulationRate = previousRate;
    applyModulation();
    applyChannel();
    applyMonitor();
  }
  unlockRadio();
  return ok;
}

bool wifiRadioSetCcaEnabled(bool enabled)
{
  if (!g_radioReady)
  {
    return false;
  }
  if (g_ccaEnabled == enabled)
  {
    return true;
  }
  if (!lockRadio(pdMS_TO_TICKS(1000)))
  {
    return false;
  }
  const bool previous = g_ccaEnabled;
  g_ccaEnabled = enabled;
  const bool ok = applyCca();
  if (!ok)
  {
    g_ccaEnabled = previous;
  }
  unlockRadio();
  return ok;
}

bool wifiRadioInject(const uint8_t* frame, size_t len)
{
  if (!g_radioReady || frame == nullptr || len < WIFI_RADIO_INJECT_MIN ||
      len > WIFI_RADIO_INJECT_MAX || g_txFree == nullptr)
  {
    g_wifiTxFail++;
    return false;
  }

  uint8_t idx = 0;
  if (xQueueReceive(g_txFree, &idx, 0) != pdTRUE)
  {
    g_wifiTxFail++;
    return false;
  }
  g_txPool[idx].len = static_cast<uint16_t>(len);
  memcpy(g_txPool[idx].data, frame, len);
  if (xQueueSend(g_txFilled, &idx, 0) != pdTRUE)
  {
    xQueueSend(g_txFree, &idx, 0);
    g_wifiTxFail++;
    return false;
  }
  return true;
}

bool wifiRadioPopRx(uint8_t* out, size_t* len, size_t maxLen)
{
  if (out == nullptr || len == nullptr || g_rxFilled == nullptr)
  {
    return false;
  }

  uint8_t idx = 0;
  if (xQueueReceive(g_rxFilled, &idx, 0) != pdTRUE)
  {
    return false;
  }

  const size_t copy = g_rxPool[idx].len < maxLen ? g_rxPool[idx].len : maxLen;
  memcpy(out, g_rxPool[idx].data, copy);
  *len = copy;
  xQueueSend(g_rxFree, &idx, 0);
  return true;
}

void wifiRadioGetStatus(WifiRadioStatus* status)
{
  if (status == nullptr)
  {
    return;
  }
  status->channel = g_channel;
  status->modulation = g_modulationName;
  status->ccaEnabled = g_ccaEnabled;
  status->wifiRx = g_wifiRx.load(std::memory_order_relaxed);
  status->wifiTx = g_wifiTx;
  status->wifiRxDropped = g_wifiRxDropped.load(std::memory_order_relaxed);
  status->wifiTxFail = g_wifiTxFail;
}

const char* wifiRadioModulationList()
{
  return "DSS_1M_L DSS_2M_S DSS_2M_L CCK_5M_L CCK_5M_S CCK_11M_L CCK_11M_S "
         "OFDM_6M OFDM_9M OFDM_12M OFDM_18M OFDM_24M OFDM_36M OFDM_48M "
         "OFDM_54M "
         "OFDM_MCS0_LGI OFDM_MCS1_LGI OFDM_MCS2_LGI OFDM_MCS3_LGI "
         "OFDM_MCS4_LGI OFDM_MCS5_LGI OFDM_MCS6_LGI OFDM_MCS7_LGI "
         "OFDM_MCS0_SGI OFDM_MCS1_SGI OFDM_MCS2_SGI OFDM_MCS3_SGI "
         "OFDM_MCS4_SGI OFDM_MCS5_SGI OFDM_MCS6_SGI OFDM_MCS7_SGI";
}
