#ifndef SIM_WINJECT_MODULATION_HPP_
#define SIM_WINJECT_MODULATION_HPP_

#include <cstddef>
#include <cstdint>

// Named PHY rates matching firmware wifi::modulation_list() / docs table.
// Airtime model: preamble_us + (len_bytes * 8) / bitrate.

struct modulation_entry_s
{
    const char* name;
    double mbps;
    uint32_t preamble_us;
    bool is_11b;  // DSSS/CCK — only rates allowed on channel 14
};

const modulation_entry_s* modulation_find(const char* name);
const char* modulation_list();
bool modulation_ok_for_channel(const char* name, uint8_t channel);
uint64_t modulation_airtime_us(const char* name, size_t mpdu_len);

#endif  // SIM_WINJECT_MODULATION_HPP_
