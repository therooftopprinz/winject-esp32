#ifndef WINJECT_SETTINGS_H_
#define WINJECT_SETTINGS_H_

#include "config.h"
#include "frame.h"
#include "packet.h"

#include <stddef.h>
#include <stdint.h>

#include "manager.h"
#include "upstream_rx_endpoint.h"
#include "upstream_tx_endpoint.h"

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
    // Persisted: mode, radio, ethernet, domain, single sut/sur.
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
    };

    // v7 = single upstream_tx/rx (no channel_info). v6 includes ci subscribers.
    static constexpr uint8_t k_blob_version = 7;
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

    upstream_tx_endpoint& sut;
    upstream_rx_endpoint& sur;
    manager& netmgr;
    uint8_t current_slot_ = 0;
    bool has_loaded = false;
    snapshot_s loaded{};
    snapshot_s scratch{};
    uint8_t blob[k_blob_max]{};
};

#endif  // WINJECT_SETTINGS_H_
