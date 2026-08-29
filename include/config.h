#pragma once

#include <ETH.h>
#include <stdint.h>

#define DEVICE_HOSTNAME "winject-esp32"

// WT32-ETH01 LAN8720 wiring.
// If link never comes up, try ETH_CLOCK_GPIO17_OUT instead of ETH_CLOCK_GPIO0_IN.
#define ETH_PHY_TYPE   ETH_PHY_LAN8720
#define ETH_PHY_ADDR   1
#define ETH_PHY_MDC    23
#define ETH_PHY_MDIO   18
#define ETH_PHY_POWER  16
#define ETH_CLK_MODE   ETH_CLOCK_GPIO0_IN

// Locally administered MAC. Change the last byte for each device.
#define ETH_MAC_BYTES  0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC
