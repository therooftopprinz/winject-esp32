#pragma once

#include <stdint.h>

struct UpstreamStatus
{
  bool rxBound;
  uint16_t rxPort;
  bool txSet;
  uint32_t txHost;
  uint16_t txPort;
  uint32_t udpRx;
  uint32_t udpTx;
  uint32_t udpRxDropped;
};

void upstreamBegin();
bool upstreamSetRxPort(uint16_t port);
bool upstreamSetTx(uint32_t host, uint16_t port);
void upstreamGetStatus(UpstreamStatus* status);
