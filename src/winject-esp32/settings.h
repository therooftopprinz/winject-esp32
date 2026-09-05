#ifndef WINJECT_SETTINGS_H_
#define WINJECT_SETTINGS_H_

#include "config.h"
#include "frame.h"
#include "upstream_rx.h"
#include "upstream_tx.h"

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
    bool save(uint8_t slot);
    bool use(uint8_t slot);
    bool load_current();
    bool apply_live();

private:
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
        bool dhcp_server_enabled;
        uint8_t rx_count;
        upstream_bind_s rx[WIFI_AIRPORT_MAX];
        uint8_t tx_count;
        upstream_dest_s tx[WIFI_AIRPORT_MAX];
    };

    static constexpr uint8_t k_blob_version = 2;
    static constexpr uint8_t k_blob_version_min = 1;
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

    upstream_rx& rx_;
    upstream_tx& tx_;
    manager& netmgr_;
    uint8_t current_slot_ = 0;
    bool has_loaded_ = false;
    // Too large for the reactor task stack (6144). Console save/use run there.
    snapshot_s loaded_{};
    snapshot_s scratch_{};
    uint8_t blob_[k_blob_max]{};
};

#endif  // WINJECT_SETTINGS_H_
