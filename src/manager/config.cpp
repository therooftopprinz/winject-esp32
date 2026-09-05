#include "config.h"

#include <bfc/configuration_parser.hpp>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "log.h"
#include "net_util.h"

static std::string trim(const std::string& s)
{
    const auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
    {
        return "";
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static bool parse_mode(const std::string& text, upstream_mode_e* mode)
{
    if (text == "UDP_GENERIC_FORWARDING")
    {
        *mode = upstream_mode_e::udp_generic;
        return true;
    }
    if (text == "UDP_CLIENT_FORWARDING")
    {
        *mode = upstream_mode_e::udp_client;
        return true;
    }
    if (text == "UDP_SERVER_FORWARDING")
    {
        *mode = upstream_mode_e::udp_server;
        return true;
    }
    if (text == "TCP_CLIENT_FORWARDING")
    {
        *mode = upstream_mode_e::tcp_client;
        return true;
    }
    if (text == "TCP_SERVER_FORWARDING")
    {
        *mode = upstream_mode_e::tcp_server;
        return true;
    }
    return false;
}

static std::string key_of(size_t i, const char* field)
{
    std::ostringstream os;
    os << "upstream-" << i << "." << field;
    return os.str();
}

static bool require_arg(const bfc::configuration_parser& p,
                        const std::string& key, std::string* out,
                        std::string* error)
{
    auto v = p.arg(key);
    if (!v || v->empty())
    {
        *error = "missing " + key;
        return false;
    }
    *out = *v;
    return true;
}

const char* config::radio_mode_name() const
{
    return radio_mode == radio_mode_e::bfc_tunnel_device ? "BFC_TUNNEL_DEVICE"
                                                    : "STANDALONE";
}

uint32_t config::phy_rate_kbps(const std::string& modulation)
{
    // Named rates from docs/winject.md (20 MHz MCS column).
    struct entry_s
    {
        const char* name;
        uint32_t kbps;
    };
    static constexpr entry_s k_table[] = {
        {"DSS_1M_L", 1000},       {"DSS_2M_S", 2000},
        {"DSS_2M_L", 2000},       {"CCK_5M_L", 5500},
        {"CCK_5M_S", 5500},       {"CCK_11M_L", 11000},
        {"CCK_11M_S", 11000},     {"OFDM_6M", 6000},
        {"OFDM_9M", 9000},        {"OFDM_12M", 12000},
        {"OFDM_18M", 18000},      {"OFDM_24M", 24000},
        {"OFDM_36M", 36000},      {"OFDM_48M", 48000},
        {"OFDM_54M", 54000},      {"OFDM_MCS0_LGI", 6500},
        {"OFDM_MCS1_LGI", 13000}, {"OFDM_MCS2_LGI", 19500},
        {"OFDM_MCS3_LGI", 26000}, {"OFDM_MCS4_LGI", 39000},
        {"OFDM_MCS5_LGI", 52000}, {"OFDM_MCS6_LGI", 58500},
        {"OFDM_MCS7_LGI", 65000}, {"OFDM_MCS0_SGI", 7200},
        {"OFDM_MCS1_SGI", 14400}, {"OFDM_MCS2_SGI", 21700},
        {"OFDM_MCS3_SGI", 28900}, {"OFDM_MCS4_SGI", 43300},
        {"OFDM_MCS5_SGI", 57800}, {"OFDM_MCS6_SGI", 65000},
        {"OFDM_MCS7_SGI", 72200},
    };
    std::string upper;
    upper.reserve(modulation.size());
    for (char c : modulation)
    {
        upper.push_back(
            static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    for (const auto& e : k_table)
    {
        if (upper == e.name)
        {
            return e.kbps;
        }
    }
    return 0;
}

uint32_t config::derive_max_rate_kbps(const std::string& modulation)
{
    const uint32_t phy = phy_rate_kbps(modulation);
    if (phy == 0)
    {
        return 10000;  // fallback if modulation string is unknown
    }
    // Match tools/bw_test.py auto_offer for a full MPDU. Scheduler/ingest
    // ceiling is ~70% of that UDP estimate (~10 Mbps for OFDM_24M).
    constexpr size_t k_payload = 1400;
    const double preamble_us = phy <= 11000 ? 200.0 : 40.0;
    const double mac_us = phy <= 11000 ? 400.0 : 150.0;
    const double mpdu_bits = (24.0 + static_cast<double>(k_payload)) * 8.0;
    const double air_us =
        preamble_us + (mpdu_bits / static_cast<double>(phy)) * 1000.0 + mac_us;
    const double udp_kbps =
        (static_cast<double>(k_payload) * 8.0) / (air_us / 1000.0) * 0.85;
    const double tcp_kbps = udp_kbps * 0.70;
    uint32_t out = static_cast<uint32_t>(tcp_kbps + 0.5);
    if (out < 64)
    {
        out = 64;
    }
    return out;
}

bool config::load(const std::string& path, std::string* error)
{
    std::ifstream in(path);
    if (!in.is_open())
    {
        *error = "cannot open " + path;
        return false;
    }

    bfc::configuration_parser parser;
    std::string line;
    while (std::getline(in, line))
    {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#')
        {
            continue;
        }
        parser.load_line(t);
    }

    if (!require_arg(parser, "winject.device", &device, error))
    {
        return false;
    }
    auto console = parser.as<unsigned>("winject.console");
    if (!console || *console == 0 || *console > 65535)
    {
        *error = "invalid winject.console";
        return false;
    }
    console_port = static_cast<uint16_t>(*console);

    auto ch = parser.as<unsigned>("winject.channel");
    if (!ch || *ch < 1 || *ch > 13)
    {
        *error = "invalid winject.channel";
        return false;
    }
    channel = static_cast<uint8_t>(*ch);

    if (!require_arg(parser, "winject.modulation", &modulation, error))
    {
        return false;
    }
    auto pwr = parser.as<int>("winject.power");
    if (!pwr || *pwr < 2 || *pwr > 20)
    {
        *error = "invalid winject.power";
        return false;
    }
    power_dbm = static_cast<int8_t>(*pwr);

    std::string mode_s;
    if (!require_arg(parser, "winject.mode", &mode_s, error))
    {
        return false;
    }
    if (mode_s == "STANDALONE")
    {
        radio_mode = radio_mode_e::standalone;
    }
    else if (mode_s == "BFC_TUNNEL_DEVICE")
    {
        radio_mode = radio_mode_e::bfc_tunnel_device;
    }
    else
    {
        *error = "invalid winject.mode";
        return false;
    }

    auto rate = parser.as<unsigned>("winject.max_rate_kbps");
    if (rate && *rate > 0)
    {
        max_rate_kbps = *rate;
    }
    else
    {
        max_rate_kbps = 10000;
    }
    local_ip = parser.arg("winject.local_ip").value_or("");
    if (auto skip = parser.arg("winject.skip_console"))
    {
        skip_console = *skip == "1" || *skip == "true";
    }
    auto fwd_base = parser.as<unsigned>("winject.forward_base");
    if (fwd_base && *fwd_base > 0 && *fwd_base <= 65535)
    {
        forward_base = static_cast<uint16_t>(*fwd_base);
    }
    stats_sec = parser.as<unsigned>("winject.stats_sec").value_or(0);
    if (const char* env = std::getenv("WINJECT_STATS_SEC"))
    {
        const unsigned v =
            static_cast<unsigned>(std::strtoul(env, nullptr, 10));
        if (v > 0)
        {
            stats_sec = v;
        }
    }

    auto size = parser.as<unsigned>("upstream.size");
    if (!size || *size == 0)
    {
        *error = "invalid upstream.size";
        return false;
    }
    if (radio_mode == radio_mode_e::bfc_tunnel_device && *size != 1)
    {
        *error = "BFC_TUNNEL_DEVICE allows only one upstream";
        return false;
    }

    upstreams.clear();
    for (size_t i = 0; i < *size; i++)
    {
        upstream_config_s u;
        u.index = i;
        std::string umode;
        if (!require_arg(parser, key_of(i, "mode"), &umode, error))
        {
            return false;
        }
        if (!parse_mode(umode, &u.mode))
        {
            *error = "invalid " + key_of(i, "mode");
            return false;
        }
        std::string airport;
        if (!require_arg(parser, key_of(i, "airport"), &airport, error))
        {
            return false;
        }
        if (!parse_airport(airport, u.airport))
        {
            *error = "invalid " + key_of(i, "airport");
            return false;
        }
        if (radio_mode == radio_mode_e::bfc_tunnel_device &&
            !airport_is_zero(u.airport))
        {
            *error = "BFC_TUNNEL_DEVICE airport must be 0";
            return false;
        }
        auto budget = parser.as<unsigned>(key_of(i, "scheduler_budget"));
        if (!budget || *budget == 0)
        {
            *error = "invalid " + key_of(i, "scheduler_budget");
            return false;
        }
        u.scheduler_budget = *budget;
        u.rcv_buffer_size = static_cast<int>(
            parser.as<int>(key_of(i, "rcv_buffer_size")).value_or(0));
        u.snd_buffer_size = static_cast<int>(
            parser.as<int>(key_of(i, "snd_buffer_size")).value_or(0));
        u.rx = parser.arg(key_of(i, "rx")).value_or("");
        u.tx = parser.arg(key_of(i, "tx")).value_or("");
        u.bind_address = parser.arg(key_of(i, "bind_address")).value_or("");
        u.connect_address =
            parser.arg(key_of(i, "connect_address")).value_or("");

        const std::string fec_type =
            parser.arg(key_of(i, "fec.type")).value_or("");
        if (!fec_type.empty() && fec_type != "NONE")
        {
            if (fec_type != "RS_BLOCK_ERASURE")
            {
                *error = "invalid " + key_of(i, "fec.type");
                return false;
            }
            if (u.mode == upstream_mode_e::tcp_client ||
                u.mode == upstream_mode_e::tcp_server)
            {
                *error =
                    key_of(i, "fec.type") + " is only valid for UDP upstreams";
                return false;
            }
            auto fk = parser.as<unsigned>(key_of(i, "fec.k"));
            auto fn = parser.as<unsigned>(key_of(i, "fec.n"));
            if (!fk || !fn || *fk < 1 || *fn <= *fk || *fn > 255)
            {
                *error = "invalid " + key_of(i, "fec.k") + "/" +
                         key_of(i, "fec.n") + " (need 1 <= k < n <= 255)";
                return false;
            }
            u.fec_type = fec_type_e::rs_block_erasure;
            u.fec_k = static_cast<int>(*fk);
            u.fec_n = static_cast<int>(*fn);
            auto fto = parser.as<int>(key_of(i, "fec.timeout_ms"));
            if (fto)
            {
                if (*fto < 0)
                {
                    *error = "invalid " + key_of(i, "fec.timeout_ms");
                    return false;
                }
                u.fec_timeout_ms = *fto;
            }
        }

        if (u.mode == upstream_mode_e::udp_generic &&
            (u.rx.empty() || u.tx.empty()))
        {
            *error = "UDP_GENERIC_FORWARDING needs rx and tx";
            return false;
        }
        if (u.mode == upstream_mode_e::udp_client && u.connect_address.empty())
        {
            *error = "UDP_CLIENT_FORWARDING needs connect_address";
            return false;
        }
        if (u.mode == upstream_mode_e::udp_server && u.bind_address.empty())
        {
            *error = "UDP_SERVER_FORWARDING needs bind_address";
            return false;
        }
        if (u.mode == upstream_mode_e::tcp_client && u.connect_address.empty())
        {
            *error = "TCP_CLIENT_FORWARDING needs connect_address";
            return false;
        }
        if (u.mode == upstream_mode_e::tcp_server && u.bind_address.empty())
        {
            *error = "TCP_SERVER_FORWARDING needs bind_address";
            return false;
        }
        for (const auto& prev : upstreams)
        {
            if (memcmp(prev.airport, u.airport, 6) == 0)
            {
                *error = "duplicate airport on upstream-" + std::to_string(i);
                return false;
            }
        }
        upstreams.push_back(u);
    }
    return true;
}
