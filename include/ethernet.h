#pragma once

#include <stdint.h>

void ethernetBegin();
bool ethernetConnected();
bool ethernetLocalIpv4(uint32_t *out);
