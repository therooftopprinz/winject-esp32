#include "settings.h"

#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "manager.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "settings";
static const char* kNvsNs = "winject";
static const char* kCurrentKey = "cur";
static constexpr uint8_t kBlobVersion = 2;
static constexpr uint8_t kBlobVersionMin = 1;
static constexpr size_t kBlobMax = 2600;

static uint8_t g_currentSlot = 0;
static bool g_hasLoaded = false;
static SettingsSnapshot g_loaded = {};

static bool slotOk(uint8_t slot)
{
    return slot < SETTINGS_SLOT_COUNT;
}

static void slotKey(uint8_t slot, char* out, size_t outLen)
{
    snprintf(out, outLen, "s%u", slot);
}

static void snapshotDefaults(SettingsSnapshot* snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->mode = WINJECT_MODE_BFC_TUNNEL_DEVICE;
    snap->channel = WIFI_DEFAULT_CHANNEL;
    strncpy(snap->modulation, WIFI_DEFAULT_MODULATION,
            sizeof(snap->modulation) - 1);
    snap->ccaEnabled = true;
    snap->allowFailedCrc = false;
    snap->txPowerDbm = WIFI_DEFAULT_TX_POWER_DBM;
    snap->networkMode = NETMGR_MODE_AUTO;
    snap->dhcpServerEnabled = false;
}

static bool captureLive(SettingsSnapshot* snap, upstream_rx& rx,
                        upstream_tx& tx, manager& netmgr)
{
    if (snap == nullptr)
    {
        return false;
    }
    snapshotDefaults(snap);

    snap->mode = frameGetMode();

    wifi& w = wifi::instance();
    wifi_status_s radio = {};
    w.get_status(&radio);
    if (w.ready())
    {
        snap->channel = radio.channel;
        if (radio.modulation != nullptr)
        {
            strncpy(snap->modulation, radio.modulation,
                    sizeof(snap->modulation) - 1);
        }
        snap->ccaEnabled = radio.cca_enabled;
        snap->allowFailedCrc = radio.allow_failed_crc;
        snap->txPowerDbm = radio.tx_power_dbm;
    }

    uint32_t fallback = 0;
    if (netmgr.static_ipv4(&fallback))
    {
        snap->fallbackIp = fallback;
    }
    snap->networkMode = netmgr.network_mode();
    snap->dhcpServerEnabled = netmgr.dhcp_server_enabled();

    rx.fill_status(snap->rx, &snap->rxCount);
    tx.fill_status(snap->tx, &snap->txCount);
    for (uint8_t i = 0; i < snap->rxCount; i++)
    {
        snap->rx[i].socket_open = false;
    }
    return true;
}

static bool packBlob(const SettingsSnapshot& snap, uint8_t* buf, size_t* len)
{
    if (buf == nullptr || len == nullptr)
    {
        return false;
    }
    if (snap.rxCount > WIFI_AIRPORT_MAX || snap.txCount > WIFI_AIRPORT_MAX)
    {
        return false;
    }

    uint8_t* p = buf;
    auto put8 = [&p](uint8_t v)
    {
        *p++ = v;
    };
    auto put16 = [&p](uint16_t v)
    {
        memcpy(p, &v, sizeof(v));
        p += sizeof(v);
    };
    auto put32 = [&p](uint32_t v)
    {
        memcpy(p, &v, sizeof(v));
        p += sizeof(v);
    };

    put8(kBlobVersion);
    put8(static_cast<uint8_t>(snap.mode));
    put8(snap.channel);
    put8(snap.ccaEnabled ? 1 : 0);
    put8(snap.allowFailedCrc ? 1 : 0);
    put8(static_cast<uint8_t>(snap.txPowerDbm));
    memcpy(p, snap.modulation, SETTINGS_MODULATION_MAX);
    p += SETTINGS_MODULATION_MAX;
    put32(snap.fallbackIp);
    put8(static_cast<uint8_t>(snap.networkMode));
    put8(snap.dhcpServerEnabled ? 1 : 0);
    put8(snap.rxCount);
    for (uint8_t i = 0; i < snap.rxCount; i++)
    {
        memcpy(p, snap.rx[i].airport, 6);
        p += 6;
        put16(snap.rx[i].port);
    }
    put8(snap.txCount);
    for (uint8_t i = 0; i < snap.txCount; i++)
    {
        memcpy(p, snap.tx[i].airport, 6);
        p += 6;
        put32(snap.tx[i].host);
        put16(snap.tx[i].port);
    }

    *len = static_cast<size_t>(p - buf);
    return *len <= kBlobMax;
}

static bool unpackBlob(const uint8_t* buf, size_t len, SettingsSnapshot* snap)
{
    if (buf == nullptr || snap == nullptr || len < 11)
    {
        return false;
    }
    snapshotDefaults(snap);

    const uint8_t* p = buf;
    const uint8_t* end = buf + len;
    auto need = [&](size_t n) -> bool
    {
        return static_cast<size_t>(end - p) >= n;
    };
    auto get8 = [&](uint8_t* v) -> bool
    {
        if (!need(1))
        {
            return false;
        }
        *v = *p++;
        return true;
    };
    auto get16 = [&](uint16_t* v) -> bool
    {
        if (!need(sizeof(uint16_t)))
        {
            return false;
        }
        memcpy(v, p, sizeof(uint16_t));
        p += sizeof(uint16_t);
        return true;
    };
    auto get32 = [&](uint32_t* v) -> bool
    {
        if (!need(sizeof(uint32_t)))
        {
            return false;
        }
        memcpy(v, p, sizeof(uint32_t));
        p += sizeof(uint32_t);
        return true;
    };

    uint8_t version = 0;
    uint8_t mode = 0;
    uint8_t cca = 0;
    uint8_t allowCrc = 0;
    uint8_t txPower = 0;
    if (!get8(&version) || version < kBlobVersionMin || version > kBlobVersion)
    {
        return false;
    }
    if (!get8(&mode) || !get8(&snap->channel) || !get8(&cca) ||
        !get8(&allowCrc) || !get8(&txPower))
    {
        return false;
    }
    if (mode != WINJECT_MODE_BFC_TUNNEL_DEVICE &&
        mode != WINJECT_MODE_STANDALONE)
    {
        return false;
    }
    snap->mode = static_cast<WinjectMode>(mode);
    snap->ccaEnabled = cca != 0;
    snap->allowFailedCrc = allowCrc != 0;
    snap->txPowerDbm = static_cast<int8_t>(txPower);
    if (!need(SETTINGS_MODULATION_MAX))
    {
        return false;
    }
    memcpy(snap->modulation, p, SETTINGS_MODULATION_MAX);
    snap->modulation[SETTINGS_MODULATION_MAX - 1] = '\0';
    p += SETTINGS_MODULATION_MAX;
    if (!get32(&snap->fallbackIp))
    {
        return false;
    }
    if (version >= 2)
    {
        uint8_t networkMode = 0;
        uint8_t dhcpServer = 0;
        if (!get8(&networkMode) || !get8(&dhcpServer))
        {
            return false;
        }
        if (networkMode != NETMGR_MODE_STATIC &&
            networkMode != NETMGR_MODE_AUTO)
        {
            return false;
        }
        snap->networkMode = static_cast<NetmgrMode>(networkMode);
        snap->dhcpServerEnabled = dhcpServer != 0;
    }
    else
    {
        snap->networkMode = NETMGR_MODE_AUTO;
        snap->dhcpServerEnabled = false;
    }
    if (!get8(&snap->rxCount))
    {
        return false;
    }
    if (snap->rxCount > WIFI_AIRPORT_MAX)
    {
        return false;
    }
    for (uint8_t i = 0; i < snap->rxCount; i++)
    {
        if (!need(6))
        {
            return false;
        }
        memcpy(snap->rx[i].airport, p, 6);
        p += 6;
        if (!get16(&snap->rx[i].port))
        {
            return false;
        }
        snap->rx[i].socket_open = false;
    }
    if (!get8(&snap->txCount) || snap->txCount > WIFI_AIRPORT_MAX)
    {
        return false;
    }
    for (uint8_t i = 0; i < snap->txCount; i++)
    {
        if (!need(6))
        {
            return false;
        }
        memcpy(snap->tx[i].airport, p, 6);
        p += 6;
        if (!get32(&snap->tx[i].host) || !get16(&snap->tx[i].port))
        {
            return false;
        }
    }
    return p == end;
}

static nvs_handle_t openNvs(bool write)
{
    nvs_handle_t handle = 0;
    const nvs_open_mode_t mode = write ? NVS_READWRITE : NVS_READONLY;
    if (nvs_open(kNvsNs, mode, &handle) != ESP_OK)
    {
        if (!write)
        {
            return 0;
        }
        if (nvs_open(kNvsNs, NVS_READWRITE, &handle) != ESP_OK)
        {
            return 0;
        }
    }
    return handle;
}

static bool readBlob(uint8_t slot, SettingsSnapshot* snap)
{
    char key[8];
    slotKey(slot, key, sizeof(key));
    nvs_handle_t handle = openNvs(false);
    if (handle == 0)
    {
        return false;
    }
    uint8_t buf[kBlobMax];
    size_t len = sizeof(buf);
    const esp_err_t err = nvs_get_blob(handle, key, buf, &len);
    nvs_close(handle);
    if (err != ESP_OK)
    {
        return false;
    }
    return unpackBlob(buf, len, snap);
}

static bool writeBlob(uint8_t slot, const SettingsSnapshot& snap)
{
    uint8_t buf[kBlobMax];
    size_t len = 0;
    if (!packBlob(snap, buf, &len))
    {
        return false;
    }
    char key[8];
    slotKey(slot, key, sizeof(key));
    nvs_handle_t handle = openNvs(true);
    if (handle == 0)
    {
        ESP_LOGE(TAG, "nvs open failed");
        return false;
    }
    bool ok = nvs_set_blob(handle, key, buf, len) == ESP_OK;
    if (ok)
    {
        ok = nvs_set_u8(handle, kCurrentKey, slot) == ESP_OK;
    }
    if (ok)
    {
        ok = nvs_commit(handle) == ESP_OK;
    }
    nvs_close(handle);
    return ok;
}

static bool applySnapshot(const SettingsSnapshot& snap, upstream_rx& rx,
                          upstream_tx& tx, manager& netmgr)
{
    if (!frameSetMode(snap.mode))
    {
        return false;
    }
    wifi& radio = wifi::instance();
    if (radio.ready())
    {
        if (!radio.set_channel(snap.channel) ||
            !radio.set_modulation(snap.modulation) ||
            !radio.set_cca_enabled(snap.ccaEnabled) ||
            !radio.set_allow_failed_crc(snap.allowFailedCrc) ||
            !radio.set_tx_power(snap.txPowerDbm))
        {
            ESP_LOGE(TAG, "radio apply failed");
            return false;
        }
    }
    if (snap.fallbackIp != 0)
    {
        netmgr.set_ip(snap.fallbackIp);
    }
    netmgr.set_dhcp_server_enabled(snap.dhcpServerEnabled);
    if (!netmgr.set_network_mode(snap.networkMode))
    {
        ESP_LOGE(TAG, "network apply failed");
        return false;
    }
    if (!rx.load(snap.rx, snap.rxCount) || !tx.load(snap.tx, snap.txCount))
    {
        ESP_LOGE(TAG, "upstream apply failed");
        return false;
    }
    return true;
}

uint8_t settingsCurrentSlot()
{
    return g_currentSlot;
}

bool settingsSave(uint8_t slot, upstream_rx& rx, upstream_tx& tx,
                  manager& netmgr)
{
    if (!slotOk(slot))
    {
        return false;
    }
    SettingsSnapshot snap = {};
    if (!captureLive(&snap, rx, tx, netmgr))
    {
        return false;
    }
    if (!writeBlob(slot, snap))
    {
        ESP_LOGE(TAG, "save slot %u failed", slot);
        return false;
    }
    g_currentSlot = slot;
    g_loaded = snap;
    g_hasLoaded = true;
    ESP_LOGI(TAG, "saved slot %u", slot);
    return true;
}

bool settingsUse(uint8_t slot, upstream_rx& rx, upstream_tx& tx,
                 manager& netmgr)
{
    if (!slotOk(slot))
    {
        return false;
    }
    SettingsSnapshot snap = {};
    if (!readBlob(slot, &snap))
    {
        return false;
    }
    nvs_handle_t handle = openNvs(true);
    if (handle != 0)
    {
        nvs_set_u8(handle, kCurrentKey, slot);
        nvs_commit(handle);
        nvs_close(handle);
    }
    if (!applySnapshot(snap, rx, tx, netmgr))
    {
        return false;
    }
    g_currentSlot = slot;
    g_loaded = snap;
    g_hasLoaded = true;
    ESP_LOGI(TAG, "using slot %u", slot);
    return true;
}

bool settingsLoadCurrent(manager& netmgr)
{
    snapshotDefaults(&g_loaded);
    g_hasLoaded = false;
    g_currentSlot = 0;

    nvs_handle_t handle = openNvs(false);
    if (handle != 0)
    {
        uint8_t slot = 0;
        if (nvs_get_u8(handle, kCurrentKey, &slot) == ESP_OK && slotOk(slot))
        {
            g_currentSlot = slot;
        }
        nvs_close(handle);
    }

    if (readBlob(g_currentSlot, &g_loaded))
    {
        g_hasLoaded = true;
        if (g_loaded.fallbackIp != 0)
        {
            netmgr.set_ip(g_loaded.fallbackIp);
        }
        netmgr.set_dhcp_server_enabled(g_loaded.dhcpServerEnabled);
        netmgr.set_network_mode(g_loaded.networkMode);
        ESP_LOGI(TAG, "loaded slot %u", g_currentSlot);
    }
    else
    {
        ESP_LOGI(TAG, "slot %u empty, using defaults", g_currentSlot);
    }
    return true;
}

bool settingsApplyLive(upstream_rx& rx, upstream_tx& tx, manager& netmgr)
{
    if (!g_hasLoaded)
    {
        return true;
    }
    return applySnapshot(g_loaded, rx, tx, netmgr);
}
