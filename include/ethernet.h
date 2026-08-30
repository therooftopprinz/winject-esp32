#pragma once

#include <stddef.h>
#include <stdint.h>

void ethernetBegin();
bool ethernetConnected();
bool ethernetLocalIpv4(uint32_t* out);
bool ethernetMac(uint8_t mac[6]);
bool ethernetLinkSpeedMbps(uint32_t* mbps);
