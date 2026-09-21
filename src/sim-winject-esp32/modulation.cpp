#include "modulation.hpp"

#include <cmath>
#include <cstring>
#include <strings.h>

namespace
{

constexpr modulation_entry_s k_mods[] = {
    {"DSS_1M_L", 1.0, 192, true},
    {"DSS_2M_S", 2.0, 96, true},
    {"DSS_2M_L", 2.0, 192, true},
    {"CCK_5M_L", 5.5, 192, true},
    {"CCK_5M_S", 5.5, 96, true},
    {"CCK_11M_L", 11.0, 192, true},
    {"CCK_11M_S", 11.0, 96, true},
    {"OFDM_6M", 6.0, 20, false},
    {"OFDM_9M", 9.0, 20, false},
    {"OFDM_12M", 12.0, 20, false},
    {"OFDM_18M", 18.0, 20, false},
    {"OFDM_24M", 24.0, 20, false},
    {"OFDM_36M", 36.0, 20, false},
    {"OFDM_48M", 48.0, 20, false},
    {"OFDM_54M", 54.0, 20, false},
    {"OFDM_MCS0_LGI", 6.5, 32, false},
    {"OFDM_MCS1_LGI", 13.0, 32, false},
    {"OFDM_MCS2_LGI", 19.5, 32, false},
    {"OFDM_MCS3_LGI", 26.0, 32, false},
    {"OFDM_MCS4_LGI", 39.0, 32, false},
    {"OFDM_MCS5_LGI", 52.0, 32, false},
    {"OFDM_MCS6_LGI", 58.5, 32, false},
    {"OFDM_MCS7_LGI", 65.0, 32, false},
    {"OFDM_MCS0_SGI", 7.2, 28, false},
    {"OFDM_MCS1_SGI", 14.4, 28, false},
    {"OFDM_MCS2_SGI", 21.7, 28, false},
    {"OFDM_MCS3_SGI", 28.9, 28, false},
    {"OFDM_MCS4_SGI", 43.3, 28, false},
    {"OFDM_MCS5_SGI", 57.8, 28, false},
    {"OFDM_MCS6_SGI", 65.0, 28, false},
    {"OFDM_MCS7_SGI", 72.2, 28, false},
};

}  // namespace

const modulation_entry_s* modulation_find(const char* name)
{
    if (name == nullptr || name[0] == '\0')
    {
        return nullptr;
    }
    for (const auto& e : k_mods)
    {
        if (strcasecmp(e.name, name) == 0)
        {
            return &e;
        }
    }
    return nullptr;
}

const char* modulation_list()
{
    return "DSS_1M_L DSS_2M_S DSS_2M_L CCK_5M_L CCK_5M_S CCK_11M_L CCK_11M_S "
           "OFDM_6M OFDM_9M OFDM_12M OFDM_18M OFDM_24M OFDM_36M OFDM_48M "
           "OFDM_54M "
           "OFDM_MCS0_LGI OFDM_MCS1_LGI OFDM_MCS2_LGI OFDM_MCS3_LGI "
           "OFDM_MCS4_LGI OFDM_MCS5_LGI OFDM_MCS6_LGI OFDM_MCS7_LGI "
           "OFDM_MCS0_SGI OFDM_MCS1_SGI OFDM_MCS2_SGI OFDM_MCS3_SGI "
           "OFDM_MCS4_SGI OFDM_MCS5_SGI OFDM_MCS6_SGI OFDM_MCS7_SGI";
}

bool modulation_ok_for_channel(const char* name, uint8_t channel)
{
    const modulation_entry_s* e = modulation_find(name);
    if (e == nullptr)
    {
        return false;
    }
    if (channel == 14 && !e->is_11b)
    {
        return false;
    }
    return true;
}

uint64_t modulation_airtime_us(const char* name, size_t mpdu_len)
{
    const modulation_entry_s* e = modulation_find(name);
    if (e == nullptr || e->mbps <= 0.0)
    {
        return 0;
    }
    const double bits = static_cast<double>(mpdu_len) * 8.0;
    const double payload_us = (bits / e->mbps);  // us at Mbps
    const double total = static_cast<double>(e->preamble_us) + payload_us;
    if (total < 1.0)
    {
        return 1;
    }
    return static_cast<uint64_t>(std::llround(total));
}

