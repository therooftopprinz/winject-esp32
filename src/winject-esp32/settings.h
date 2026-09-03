#ifndef WINJECT_SETTINGS_H_
#define WINJECT_SETTINGS_H_

#include "config.h"
#include "frame.h"
#include "upstream_rx.h"
#include "upstream_tx.h"

#include <stdint.h>

#include "manager.h"

struct SettingsSnapshot
{
    WinjectMode mode;
    uint8_t channel;
    char modulation[SETTINGS_MODULATION_MAX];
    bool ccaEnabled;
    bool allowFailedCrc;
    int8_t txPowerDbm;
    uint32_t fallbackIp;
    NetmgrMode networkMode;
    bool dhcpServerEnabled;
    uint8_t rxCount;
    upstream_bind_s rx[WIFI_AIRPORT_MAX];
    uint8_t txCount;
    upstream_dest_s tx[WIFI_AIRPORT_MAX];
};

uint8_t settingsCurrentSlot();
bool settingsSave(uint8_t slot, upstream_rx& rx, upstream_tx& tx,
                  manager& netmgr);
bool settingsUse(uint8_t slot, upstream_rx& rx, upstream_tx& tx,
                 manager& netmgr);
bool settingsLoadCurrent(manager& netmgr);
bool settingsApplyLive(upstream_rx& rx, upstream_tx& tx, manager& netmgr);

#endif  // WINJECT_SETTINGS_H_
