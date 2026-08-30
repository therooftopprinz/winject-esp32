#pragma once

#include <stddef.h>
#include <stdint.h>

struct WifiRadioStatus
{
  uint8_t channel;
  const char* modulation;
  bool ccaEnabled;
  uint32_t wifiRx;
  uint32_t wifiTx;
  uint32_t wifiRxDropped;
  uint32_t wifiTxFail;
};

bool wifiRadioBegin();
bool wifiRadioReady();
bool wifiRadioStartApFallback();
bool wifiRadioApActive();
const char* wifiRadioApSsid();
bool wifiRadioApIpv4(uint32_t* out);
bool wifiRadioSetChannel(uint8_t channel);
bool wifiRadioSetModulation(const char* name);
bool wifiRadioSetCcaEnabled(bool enabled);
bool wifiRadioInject(const uint8_t* frame, size_t len);
bool wifiRadioPopRx(uint8_t* out, size_t* len, size_t maxLen);
void wifiRadioGetStatus(WifiRadioStatus* status);
const char* wifiRadioModulationList();
