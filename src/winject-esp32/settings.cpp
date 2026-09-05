#include "settings.h"

#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "settings";
static const char* k_nvs_ns = "winject";
static const char* k_current_key = "cur";

settings& settings::instance()
{
    static settings inst;
    return inst;
}

settings::settings()
    : rx_(upstream_rx::instance()),
      tx_(upstream_tx::instance()),
      netmgr_(manager::instance())
{
}

bool settings::slot_ok(uint8_t slot)
{
    return slot < SETTINGS_SLOT_COUNT;
}

void settings::slot_key(uint8_t slot, char* out, size_t out_len)
{
    snprintf(out, out_len, "s%u", slot);
}

void settings::snapshot_defaults(snapshot_s* snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->mode = WINJECT_MODE_BFC_TUNNEL_DEVICE;
    snap->channel = WIFI_DEFAULT_CHANNEL;
    strncpy(snap->modulation, WIFI_DEFAULT_MODULATION,
            sizeof(snap->modulation) - 1);
    snap->cca_enabled = true;
    snap->allow_failed_crc = false;
    snap->tx_power_dbm = WIFI_DEFAULT_TX_POWER_DBM;
    snap->network_mode = NETMGR_MODE_AUTO;
    snap->dhcp_server_enabled = false;
}

bool settings::capture_live(snapshot_s* snap)
{
    if (snap == nullptr)
    {
        return false;
    }
    snapshot_defaults(snap);

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
        snap->cca_enabled = radio.cca_enabled;
        snap->allow_failed_crc = radio.allow_failed_crc;
        snap->tx_power_dbm = radio.tx_power_dbm;
    }

    uint32_t fallback = 0;
    if (netmgr_.static_ipv4(&fallback))
    {
        snap->fallback_ip = fallback;
    }
    snap->network_mode = netmgr_.network_mode();
    snap->dhcp_server_enabled = netmgr_.dhcp_server_enabled();

    rx_.fill_status(snap->rx, &snap->rx_count);
    tx_.fill_status(snap->tx, &snap->tx_count);
    for (uint8_t i = 0; i < snap->rx_count; i++)
    {
        snap->rx[i].socket_open = false;
    }
    return true;
}

bool settings::pack_blob(const snapshot_s& snap, uint8_t* buf, size_t* len)
{
    if (buf == nullptr || len == nullptr)
    {
        return false;
    }
    if (snap.rx_count > WIFI_AIRPORT_MAX || snap.tx_count > WIFI_AIRPORT_MAX)
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

    put8(k_blob_version);
    put8(static_cast<uint8_t>(snap.mode));
    put8(snap.channel);
    put8(snap.cca_enabled ? 1 : 0);
    put8(snap.allow_failed_crc ? 1 : 0);
    put8(static_cast<uint8_t>(snap.tx_power_dbm));
    memcpy(p, snap.modulation, SETTINGS_MODULATION_MAX);
    p += SETTINGS_MODULATION_MAX;
    put32(snap.fallback_ip);
    put8(static_cast<uint8_t>(snap.network_mode));
    put8(snap.dhcp_server_enabled ? 1 : 0);
    put8(snap.rx_count);
    for (uint8_t i = 0; i < snap.rx_count; i++)
    {
        memcpy(p, snap.rx[i].airport, 6);
        p += 6;
        put16(snap.rx[i].port);
    }
    put8(snap.tx_count);
    for (uint8_t i = 0; i < snap.tx_count; i++)
    {
        memcpy(p, snap.tx[i].airport, 6);
        p += 6;
        put32(snap.tx[i].host);
        put16(snap.tx[i].port);
    }

    *len = static_cast<size_t>(p - buf);
    return *len <= k_blob_max;
}

bool settings::unpack_blob(const uint8_t* buf, size_t len, snapshot_s* snap)
{
    if (buf == nullptr || snap == nullptr || len < 11)
    {
        return false;
    }
    snapshot_defaults(snap);

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
    uint8_t allow_crc = 0;
    uint8_t tx_power = 0;
    if (!get8(&version) || version < k_blob_version_min ||
        version > k_blob_version)
    {
        return false;
    }
    if (!get8(&mode) || !get8(&snap->channel) || !get8(&cca) ||
        !get8(&allow_crc) || !get8(&tx_power))
    {
        return false;
    }
    if (mode != WINJECT_MODE_BFC_TUNNEL_DEVICE &&
        mode != WINJECT_MODE_STANDALONE)
    {
        return false;
    }
    snap->mode = static_cast<WinjectMode>(mode);
    snap->cca_enabled = cca != 0;
    snap->allow_failed_crc = allow_crc != 0;
    snap->tx_power_dbm = static_cast<int8_t>(tx_power);
    if (!need(SETTINGS_MODULATION_MAX))
    {
        return false;
    }
    memcpy(snap->modulation, p, SETTINGS_MODULATION_MAX);
    snap->modulation[SETTINGS_MODULATION_MAX - 1] = '\0';
    p += SETTINGS_MODULATION_MAX;
    if (!get32(&snap->fallback_ip))
    {
        return false;
    }
    if (version >= 2)
    {
        uint8_t network_mode = 0;
        uint8_t dhcp_server = 0;
        if (!get8(&network_mode) || !get8(&dhcp_server))
        {
            return false;
        }
        if (network_mode != NETMGR_MODE_STATIC &&
            network_mode != NETMGR_MODE_AUTO)
        {
            return false;
        }
        snap->network_mode = static_cast<NetmgrMode>(network_mode);
        snap->dhcp_server_enabled = dhcp_server != 0;
    }
    else
    {
        snap->network_mode = NETMGR_MODE_AUTO;
        snap->dhcp_server_enabled = false;
    }
    if (!get8(&snap->rx_count))
    {
        return false;
    }
    if (snap->rx_count > WIFI_AIRPORT_MAX)
    {
        return false;
    }
    for (uint8_t i = 0; i < snap->rx_count; i++)
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
    if (!get8(&snap->tx_count) || snap->tx_count > WIFI_AIRPORT_MAX)
    {
        return false;
    }
    for (uint8_t i = 0; i < snap->tx_count; i++)
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

static nvs_handle_t open_nvs(bool write)
{
    nvs_handle_t handle = 0;
    const nvs_open_mode_t mode = write ? NVS_READWRITE : NVS_READONLY;
    if (nvs_open(k_nvs_ns, mode, &handle) != ESP_OK)
    {
        if (!write)
        {
            return 0;
        }
        if (nvs_open(k_nvs_ns, NVS_READWRITE, &handle) != ESP_OK)
        {
            return 0;
        }
    }
    return handle;
}

bool settings::read_blob(uint8_t slot, snapshot_s* snap)
{
    char key[8];
    slot_key(slot, key, sizeof(key));
    nvs_handle_t handle = open_nvs(false);
    if (handle == 0)
    {
        return false;
    }
    size_t len = sizeof(blob_);
    const esp_err_t err = nvs_get_blob(handle, key, blob_, &len);
    nvs_close(handle);
    if (err != ESP_OK)
    {
        return false;
    }
    return unpack_blob(blob_, len, snap);
}

bool settings::write_blob(uint8_t slot, const snapshot_s& snap)
{
    size_t len = 0;
    if (!pack_blob(snap, blob_, &len))
    {
        return false;
    }
    char key[8];
    slot_key(slot, key, sizeof(key));
    nvs_handle_t handle = open_nvs(true);
    if (handle == 0)
    {
        ESP_LOGE(TAG, "nvs open failed");
        return false;
    }
    bool ok = nvs_set_blob(handle, key, blob_, len) == ESP_OK;
    if (ok)
    {
        ok = nvs_set_u8(handle, k_current_key, slot) == ESP_OK;
    }
    if (ok)
    {
        ok = nvs_commit(handle) == ESP_OK;
    }
    nvs_close(handle);
    return ok;
}

bool settings::apply_snapshot(const snapshot_s& snap)
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
            !radio.set_cca_enabled(snap.cca_enabled) ||
            !radio.set_allow_failed_crc(snap.allow_failed_crc) ||
            !radio.set_tx_power(snap.tx_power_dbm))
        {
            ESP_LOGE(TAG, "radio apply failed");
            return false;
        }
    }
    if (snap.fallback_ip != 0)
    {
        netmgr_.set_ip(snap.fallback_ip);
    }
    netmgr_.set_dhcp_server_enabled(snap.dhcp_server_enabled);
    if (!netmgr_.set_network_mode(snap.network_mode))
    {
        ESP_LOGE(TAG, "network apply failed");
        return false;
    }
    if (!rx_.load(snap.rx, snap.rx_count) || !tx_.load(snap.tx, snap.tx_count))
    {
        ESP_LOGE(TAG, "upstream apply failed");
        return false;
    }
    return true;
}

uint8_t settings::current_slot() const
{
    return current_slot_;
}

bool settings::save(uint8_t slot)
{
    if (!slot_ok(slot))
    {
        return false;
    }
    if (!capture_live(&scratch_))
    {
        return false;
    }
    if (!write_blob(slot, scratch_))
    {
        ESP_LOGE(TAG, "save slot %u failed", slot);
        return false;
    }
    current_slot_ = slot;
    loaded_ = scratch_;
    has_loaded_ = true;
    ESP_LOGI(TAG, "saved slot %u", slot);
    return true;
}

bool settings::use(uint8_t slot)
{
    if (!slot_ok(slot))
    {
        return false;
    }
    if (!read_blob(slot, &scratch_))
    {
        return false;
    }
    nvs_handle_t handle = open_nvs(true);
    if (handle != 0)
    {
        nvs_set_u8(handle, k_current_key, slot);
        nvs_commit(handle);
        nvs_close(handle);
    }
    if (!apply_snapshot(scratch_))
    {
        return false;
    }
    current_slot_ = slot;
    loaded_ = scratch_;
    has_loaded_ = true;
    ESP_LOGI(TAG, "using slot %u", slot);
    return true;
}

bool settings::load_current()
{
    snapshot_defaults(&loaded_);
    has_loaded_ = false;
    current_slot_ = 0;

    nvs_handle_t handle = open_nvs(false);
    if (handle != 0)
    {
        uint8_t slot = 0;
        if (nvs_get_u8(handle, k_current_key, &slot) == ESP_OK && slot_ok(slot))
        {
            current_slot_ = slot;
        }
        nvs_close(handle);
    }

    if (read_blob(current_slot_, &loaded_))
    {
        has_loaded_ = true;
        if (loaded_.fallback_ip != 0)
        {
            netmgr_.set_ip(loaded_.fallback_ip);
        }
        netmgr_.set_dhcp_server_enabled(loaded_.dhcp_server_enabled);
        netmgr_.set_network_mode(loaded_.network_mode);
        ESP_LOGI(TAG, "loaded slot %u", current_slot_);
    }
    else
    {
        ESP_LOGI(TAG, "slot %u empty, using defaults", current_slot_);
    }
    return true;
}

bool settings::apply_live()
{
    if (!has_loaded_)
    {
        return true;
    }
    return apply_snapshot(loaded_);
}
