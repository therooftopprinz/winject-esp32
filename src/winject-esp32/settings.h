#ifndef WINJECT_SETTINGS_H_
#define WINJECT_SETTINGS_H_

#include "channel_info_endpoint.h"
#include "config.h"
#include "frame.h"
#include "lc_rx_endpoint.h"
#include "lc_tx_endpoint.h"
#include "packet.h"

#include <stddef.h>
#include <stdint.h>

#include "manager.h"

class settings
{
public:
    static settings& instance();
    settings(const settings&) = delete;
    settings& operator=(const settings&) = delete;

    uint8_t current_slot() const;
    WinjectMode configured_mode() const;
    // Update loaded mode and rewrite the current NVS slot (rest unchanged).
    bool persist_mode(WinjectMode mode);
    bool save(uint8_t slot);
    bool use(uint8_t slot);
    bool load_current();
    bool apply_live();

private:
    // Persisted: mode, radio, ethernet, domain, single sut/sur, ci.
    struct snapshot_s
    {
        WinjectMode mode;
        uint8_t channel;
        char modulation[SETTINGS_MODULATION_MAX];
        bool cca_enabled;
        bool allow_failed_crc;
        int8_t tx_power_dbm;
        uint32_t fallback_ip;
        NetmgrMode network_mode;
        uint16_t domain;
        bool has_sut;
        uint16_t sut_port;
        bool has_sur;
        ip_port_t sur_dest;
        uint8_t ci_count;
        ip_port_t ci[WIFI_AIRPORT_MAX];
    };

    // v6 = single upstream_tx/rx (+ ci). v3/v5 multi-bind tables load with
    // upstreams cleared. v4 has empty upstreams.
    static constexpr uint8_t k_blob_version = 6;
    static constexpr uint8_t k_blob_version_min = 3;
    static constexpr size_t k_blob_max = 2600;

    settings();

    static bool slot_ok(uint8_t slot);
    static void slot_key(uint8_t slot, char* out, size_t out_len);
    static void snapshot_defaults(snapshot_s* snap);
    static bool pack_blob(const snapshot_s& snap, uint8_t* buf, size_t* len);
    static bool unpack_blob(const uint8_t* buf, size_t len, snapshot_s* snap);
    bool capture_live(snapshot_s* snap);
    bool apply_snapshot(const snapshot_s& snap);

    bool read_blob(uint8_t slot, snapshot_s* snap);
    bool write_blob(uint8_t slot, const snapshot_s& snap);

    lc_tx_endpoint& sut;
    lc_rx_endpoint& sur;
    channel_info_endpoint& ci;
    manager& netmgr;
    uint8_t current_slot_ = 0;
    bool has_loaded = false;
    snapshot_s loaded{};
    snapshot_s scratch{};
    uint8_t blob[k_blob_max]{};
};

#endif  // WINJECT_SETTINGS_H_
