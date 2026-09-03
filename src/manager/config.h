#ifndef WINJECT_MANAGER_CONFIG_H_
#define WINJECT_MANAGER_CONFIG_H_

#include <netinet/in.h>
#include <stdint.h>
#include <string>
#include <vector>

enum class UpstreamMode
{
    UdpGeneric,
    UdpClient,
    UdpServer,
    TcpClient,
    TcpServer,
};

enum class RadioMode
{
    Standalone,
    BfcTunnelDevice,
};

enum class FecType
{
    None,
    RsBlockErasure,
};

struct UpstreamConfig
{
    size_t index = 0;
    UpstreamMode mode = UpstreamMode::UdpGeneric;
    uint8_t airport[6]{};
    size_t scheduler_budget = 256;
    int rcv_buffer_size = 0;
    int snd_buffer_size = 0;
    FecType fec_type = FecType::None;
    int fec_k = 0;
    int fec_n = 0;
    int fec_timeout_ms = 20;
    std::string rx;
    std::string tx;
    std::string bind_address;
    std::string connect_address;
};

struct Config
{
    std::string device;
    uint16_t console_port = 2323;
    uint8_t channel = 1;
    std::string modulation = "DSS_1M_L";
    int8_t power_dbm = 20;
    RadioMode radio_mode = RadioMode::Standalone;
    uint32_t max_rate_kbps = 10000;
    std::string local_ip;
    uint16_t forward_base = 9210;
    bool skip_console = false;
    std::vector<UpstreamConfig> upstreams;

    bool load(const std::string& path, std::string* error);
    const char* radio_mode_name() const;

    // PHY air rate (kbps) for a modulation name, or 0 if unknown.
    static uint32_t phy_rate_kbps(const std::string& modulation);
    static uint32_t derive_max_rate_kbps(const std::string& modulation);

    // Periodic stats to stderr (0 = disabled). Also WINJECT_STATS_SEC env.
    unsigned stats_sec = 0;
};

#endif  // WINJECT_MANAGER_CONFIG_H_
