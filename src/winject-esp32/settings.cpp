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
    : sut(upstream_tx_endpoint::instance()),
      sur(upstream_rx_endpoint::instance()),
      netmgr(manager::instance())
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
    snap->domain = 0;
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
        snap->domain = radio.domain;
    }
    else
    {
        snap->domain = w.domain();
    }

    uint32_t fallback = 0;
    if (netmgr.static_ipv4(&fallback))
    {
        snap->fallback_ip = fallback;
    }
    snap->network_mode = netmgr.network_mode();

    upstream_tx_bind_s tx = {};
    if (sut.get_status(&tx))
    {
        snap->has_sut = true;
        snap->sut_port = tx.udp_port;
    }
    upstream_rx_bind_s rx = {};
    if (sur.get_status(&rx))
    {
        snap->has_sur = true;
        snap->sur_dest = rx.dest;
    }
    return true;
}

bool settings::pack_blob(const snapshot_s& snap, uint8_t* buf, size_t* len)
{
    if (buf == nullptr || len == nullptr)
    {
        return false;
    }
    if (snap.has_sut && snap.sut_port == 0)
    {
        return false;
    }
    if (snap.has_sur &&
        (snap.sur_dest.port == 0 || snap.sur_dest.host == 0 ||
         snap.sur_dest.host == 0xFFFFFFFFu))
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
    put8(0);  // legacy dhcp_server byte; always off
    put16(snap.domain);

    put8(snap.has_sut ? 1 : 0);
    if (snap.has_sut)
    {
        put16(snap.sut_port);
    }
    put8(snap.has_sur ? 1 : 0);
    if (snap.has_sur)
    {
        put32(snap.sur_dest.host);
        put16(snap.sur_dest.port);
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
        mode != WINJECT_MODE_STANDALONE && mode != WINJECT_MODE_OTA)
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
    uint8_t network_mode = 0;
    uint8_t dhcp_server_legacy = 0;
    if (!get8(&network_mode) || !get8(&dhcp_server_legacy))
    {
        return false;
    }
    (void)dhcp_server_legacy;  // ignored; radio never runs a DHCP server
    if (network_mode != NETMGR_MODE_STATIC && network_mode != NETMGR_MODE_AUTO)
    {
        return false;
    }
    snap->network_mode = static_cast<NetmgrMode>(network_mode);
    if (!get16(&snap->domain))
    {
        return false;
    }

    // v4: network/radio only (upstreams were runtime-only).
    if (version == 4)
    {
        return p == end;
    }

    // Legacy multi-bind tables: consume and clear (manager reprograms).
    if (version == 3 || version == 5)
    {
        uint8_t sut_count = 0;
        uint8_t sur_count = 0;
        if (!get8(&sut_count) || sut_count > WIFI_AIRPORT_MAX)
        {
            return false;
        }
        for (uint8_t i = 0; i < sut_count; i++)
        {
            uint8_t bus = 0;
            uint16_t port = 0;
            if (!get8(&bus) || !get16(&port))
            {
                return false;
            }
            (void)bus;
            (void)port;
        }
        if (!get8(&sur_count) || sur_count > WIFI_AIRPORT_MAX)
        {
            return false;
        }
        for (uint8_t i = 0; i < sur_count; i++)
        {
            uint8_t bus = 0;
            uint32_t host = 0;
            uint16_t port = 0;
            if (!get8(&bus) || !get32(&host) || !get16(&port))
            {
                return false;
            }
            (void)bus;
            (void)host;
            (void)port;
        }
        uint8_t ci_count = 0;
        if (!get8(&ci_count) || ci_count > WIFI_AIRPORT_MAX)
        {
            return false;
        }
        for (uint8_t i = 0; i < ci_count; i++)
        {
            uint32_t host = 0;
            uint16_t port = 0;
            if (!get32(&host) || !get16(&port))
            {
                return false;
            }
        }
        snap->has_sut = false;
        snap->sut_port = 0;
        snap->has_sur = false;
        snap->sur_dest = {};
        return p == end;
    }

    // v6: single upstream_tx / upstream_rx.
    uint8_t has_sut = 0;
    uint8_t has_sur = 0;
    if (!get8(&has_sut))
    {
        return false;
    }
    snap->has_sut = has_sut != 0;
    if (snap->has_sut)
    {
        if (!get16(&snap->sut_port) || snap->sut_port == 0)
        {
            return false;
        }
    }
    if (!get8(&has_sur))
    {
        return false;
    }
    snap->has_sur = has_sur != 0;
    if (snap->has_sur)
    {
        if (!get32(&snap->sur_dest.host) || !get16(&snap->sur_dest.port) ||
            snap->sur_dest.port == 0 || snap->sur_dest.host == 0 ||
            snap->sur_dest.host == 0xFFFFFFFFu)
        {
            return false;
        }
    }
    if (version == 6)
    {
        uint8_t ci_count = 0;
        if (!get8(&ci_count) || ci_count > WIFI_AIRPORT_MAX)
        {
            return false;
        }
        for (uint8_t i = 0; i < ci_count; i++)
        {
            uint32_t host = 0;
            uint16_t port = 0;
            if (!get32(&host) || !get16(&port))
            {
                return false;
            }
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
    size_t len = sizeof(blob);
    const esp_err_t err = nvs_get_blob(handle, key, blob, &len);
    nvs_close(handle);
    if (err != ESP_OK)
    {
        return false;
    }
    return unpack_blob(blob, len, snap);
}

bool settings::write_blob(uint8_t slot, const snapshot_s& snap)
{
    size_t len = 0;
    if (!pack_blob(snap, blob, &len))
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
    bool ok = nvs_set_blob(handle, key, blob, len) == ESP_OK;
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
    if (snap.fallback_ip != 0)
    {
        netmgr.set_ip(snap.fallback_ip);
    }
    if (!netmgr.set_network_mode(snap.network_mode))
    {
        ESP_LOGE(TAG, "network apply failed");
        return false;
    }
    if (snap.mode == WINJECT_MODE_OTA)
    {
        return true;
    }

    wifi& radio = wifi::instance();
    radio.set_domain(snap.domain);
    if (radio.ready())
    {
        // Channel 14 is 802.11b-only: apply DSSS/CCK before switching to 14,
        // and leave 14 before applying OFDM.
        bool radio_ok;
        if (snap.channel == 14)
        {
            radio_ok = radio.set_modulation(snap.modulation) &&
                       radio.set_channel(snap.channel);
        }
        else
        {
            radio_ok = radio.set_channel(snap.channel) &&
                       radio.set_modulation(snap.modulation);
        }
        if (!radio_ok || !radio.set_cca_enabled(snap.cca_enabled) ||
            !radio.set_allow_failed_crc(snap.allow_failed_crc) ||
            !radio.set_tx_power(snap.tx_power_dbm))
        {
            ESP_LOGE(TAG, "radio apply failed");
            return false;
        }
    }

    if (!sut.clear() || !sur.clear())
    {
        ESP_LOGE(TAG, "endpoint apply failed");
        return false;
    }
    if (snap.has_sut && !sut.set_endpoint(snap.sut_port))
    {
        ESP_LOGE(TAG, "endpoint apply failed");
        return false;
    }
    if (snap.has_sur && !sur.set_endpoint(snap.sur_dest))
    {
        ESP_LOGE(TAG, "endpoint apply failed");
        return false;
    }
    return true;
}

uint8_t settings::current_slot() const
{
    return current_slot_;
}

WinjectMode settings::configured_mode() const
{
    return loaded.mode;
}

bool settings::persist_mode(WinjectMode mode)
{
    if (mode != WINJECT_MODE_BFC_TUNNEL_DEVICE &&
        mode != WINJECT_MODE_STANDALONE && mode != WINJECT_MODE_OTA)
    {
        return false;
    }
    if (configured_mode() != WINJECT_MODE_OTA && wifi::instance().ready())
    {
        // Snapshot live radio/ethernet/upstreams before flipping into OTA.
        if (!capture_live(&scratch))
        {
            return false;
        }
        loaded = scratch;
    }
    if (!frameSetMode(mode))
    {
        return false;
    }
    loaded.mode = mode;
    if (!write_blob(current_slot_, loaded))
    {
        ESP_LOGE(TAG, "persist mode failed");
        return false;
    }
    has_loaded = true;
    ESP_LOGI(TAG, "persisted mode %s slot %u", frameModeName(mode),
             current_slot_);
    return true;
}

bool settings::save(uint8_t slot)
{
    if (!slot_ok(slot))
    {
        return false;
    }
    if (configured_mode() == WINJECT_MODE_OTA)
    {
        // Keep radio/endpoint tables from the last full-mode snapshot.
        scratch = loaded;
        scratch.mode = WINJECT_MODE_OTA;
        uint32_t fallback = 0;
        if (netmgr.static_ipv4(&fallback))
        {
            scratch.fallback_ip = fallback;
        }
        scratch.network_mode = netmgr.network_mode();
    }
    else if (!capture_live(&scratch))
    {
        return false;
    }
    if (!write_blob(slot, scratch))
    {
        ESP_LOGE(TAG, "save slot %u failed", slot);
        return false;
    }
    current_slot_ = slot;
    loaded = scratch;
    has_loaded = true;
    ESP_LOGI(TAG, "saved slot %u", slot);
    return true;
}

bool settings::use(uint8_t slot)
{
    if (!slot_ok(slot))
    {
        return false;
    }
    if (!read_blob(slot, &scratch))
    {
        return false;
    }
    const WinjectMode from = configured_mode();
    const WinjectMode to = scratch.mode;
    nvs_handle_t handle = open_nvs(true);
    if (handle != 0)
    {
        nvs_set_u8(handle, k_current_key, slot);
        nvs_commit(handle);
        nvs_close(handle);
    }
    current_slot_ = slot;
    loaded = scratch;
    has_loaded = true;
    if (from == WINJECT_MODE_OTA || to == WINJECT_MODE_OTA)
    {
        // Entering/leaving OTA needs a reboot to (un)load WiFi and LCs.
        ESP_LOGI(TAG, "using slot %u mode %s (reboot)", slot,
                 frameModeName(to));
        return true;
    }
    if (!apply_snapshot(scratch))
    {
        return false;
    }
    ESP_LOGI(TAG, "using slot %u", slot);
    return true;
}

bool settings::load_current()
{
    snapshot_defaults(&loaded);
    has_loaded = false;
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

    if (read_blob(current_slot_, &loaded))
    {
        has_loaded = true;
        if (loaded.fallback_ip != 0)
        {
            netmgr.set_ip(loaded.fallback_ip);
        }
        netmgr.set_network_mode(loaded.network_mode);
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
    if (!has_loaded)
    {
        return true;
    }
    return apply_snapshot(loaded);
}
