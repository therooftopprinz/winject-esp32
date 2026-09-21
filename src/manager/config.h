#ifndef WINJECT_MANAGER_CONFIG_H_
#define WINJECT_MANAGER_CONFIG_H_

#include <netinet/in.h>
#include <stdint.h>
#include <string>
#include <vector>

enum class upstream_mode_e
{
    udp_generic,
    udp_client,
    udp_server,
    tcp_client,
    tcp_server,
};

enum class radio_mode_e
{
    standalone,
    bfc_tunnel_device,
};

enum class fec_type_e
{
    none,
    rs_block_erasure,
};

struct upstream_config_s
{
    size_t index = 0;
    upstream_mode_e mode = upstream_mode_e::udp_generic;
    // Host TX / RX buses (0 = unset). Manager stamps/demuxes these into MPDUs.
    // UDP may set only one direction; TCP ARQ needs both.
    uint8_t bus_tx = 0;
    uint8_t bus_rx = 0;
    size_t scheduler_budget = 256;
    int rcv_buffer_size = 0;
    int snd_buffer_size = 0;
    fec_type_e fec_type = fec_type_e::none;
    int fec_k = 0;
    int fec_n = 0;
    int fec_timeout_ms = 20;
    std::string rx;
    std::string tx;
    std::string bind_address;
    std::string connect_address;
};

struct config
{
    std::string device;
    uint16_t console_port = 22;
    uint8_t channel = 1;
    std::string modulation = "DSS_1M_L";
    int8_t power_dbm = 20;
    radio_mode_e radio_mode = radio_mode_e::standalone;
    // Shared air domain (Addr3); 0 = unset / invalid.
    uint16_t domain = 0;
    uint32_t max_rate_kbps = 10000;
    // Max DATA MPDUs emitted per 250 us scheduler tick (1–32).
    size_t max_data_per_tick = 4;
    std::string local_ip;
    uint16_t inject_port = 9000;
    uint16_t forward_port = 9210;
    uint16_t forward_base = 9210;  // legacy alias for forward_port
    bool skip_console = false;
    // Console tx_grant before inject batches (cd-protocol); 0 = no grant gate.
    bool ci_pace_inject = true;
    // Local UDP management console. Empty = disabled. Both required together.
    // console_in = bind (recv commands); console_out = dest (send replies).
    std::string manager_console_in;
    std::string manager_console_out;
    std::vector<upstream_config_s> upstreams;

    bool load(const std::string& path, std::string* error);
    const char* radio_mode_name() const;

    // PHY air rate (kbps) for a modulation name, or 0 if unknown.
    static uint32_t phy_rate_kbps(const std::string& modulation);
    static uint32_t derive_max_rate_kbps(const std::string& modulation);
    // Canonical firmware name (e.g. OFDM_24M), or empty if unknown.
    static std::string canonical_modulation(const std::string& modulation);
    // Channel 14 is DSSS/CCK only. Unknown names are not ok.
    static bool modulation_ok_for_channel(const std::string& modulation,
                                          uint8_t channel);

    // Periodic stats to stderr (0 = disabled). Also WINJECT_STATS_SEC env.
    unsigned stats_sec = 0;
};

#endif  // WINJECT_MANAGER_CONFIG_H_
