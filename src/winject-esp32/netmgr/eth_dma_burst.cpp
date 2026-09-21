#include "eth_dma_burst.h"

#include "nvs.h"
#include "nvs_flash.h"

static const char* k_ns = "winject";
static const char* k_key = "eth_dma_burst";

static bool beats_valid(uint8_t beats)
{
    return beats == 32 || beats == 16 || beats == 8 || beats == 4 || beats == 2 ||
           beats == 1;
}

static eth_mac_dma_burst_len_t beats_to_enum(uint8_t beats)
{
    switch (beats)
    {
        case 32:
            return ETH_DMA_BURST_LEN_32;
        case 16:
            return ETH_DMA_BURST_LEN_16;
        case 8:
            return ETH_DMA_BURST_LEN_8;
        case 4:
            return ETH_DMA_BURST_LEN_4;
        case 2:
            return ETH_DMA_BURST_LEN_2;
        case 1:
            return ETH_DMA_BURST_LEN_1;
        default:
            return ETH_DMA_BURST_LEN_32;
    }
}

uint8_t eth_dma_burst_beats_stored()
{
    nvs_handle_t handle = 0;
    if (nvs_open(k_ns, NVS_READONLY, &handle) != ESP_OK)
    {
        return 32;
    }
    uint8_t beats = 32;
    if (nvs_get_u8(handle, k_key, &beats) != ESP_OK || !beats_valid(beats))
    {
        beats = 32;
    }
    nvs_close(handle);
    return beats;
}

eth_mac_dma_burst_len_t eth_dma_burst_len_for_init()
{
    return beats_to_enum(eth_dma_burst_beats_stored());
}

bool eth_dma_burst_set_beats(uint8_t beats)
{
    if (!beats_valid(beats))
    {
        return false;
    }
    nvs_handle_t handle = 0;
    if (nvs_open(k_ns, NVS_READWRITE, &handle) != ESP_OK)
    {
        return false;
    }
    const bool ok =
        nvs_set_u8(handle, k_key, beats) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}
