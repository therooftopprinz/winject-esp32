#ifndef WINJECT_ETH_DMA_BURST_H_
#define WINJECT_ETH_DMA_BURST_H_

#include <stdint.h>

#include "hal/eth_types.h"

// NVS-persisted EMAC programmed DMA burst (beats). Applied at ETH driver init.
uint8_t eth_dma_burst_beats_stored();
eth_mac_dma_burst_len_t eth_dma_burst_len_for_init();
bool eth_dma_burst_set_beats(uint8_t beats);

#endif  // WINJECT_ETH_DMA_BURST_H_
