#pragma once

#include <stdint.h>

#define DEVICE_HOSTNAME "winject-esp32"

// WT32-ETH01 LAN8720 wiring.
// If link never comes up, set ETH_CLK_GPIO17_OUT instead of ETH_CLK_GPIO0_IN.
#define ETH_PHY_ADDR 1
#define ETH_PHY_MDC 23
#define ETH_PHY_MDIO 18
#define ETH_PHY_POWER 16
#define ETH_CLK_GPIO0_IN 0
#define ETH_CLK_GPIO17_OUT 1
#define ETH_CLK_MODE ETH_CLK_GPIO0_IN

// TCP control-plane console (also available on UART0).
#define CONTROL_CONSOLE_PORT 2323

// If Ethernet DHCP has not assigned an address by this time, start an open
// WiFi AP named winject-<STA MAC>. Monitor/inject stays up (APSTA).
#define DHCP_FALLBACK_MS 5000
#define WIFI_AP_MAX_CLIENTS 4

// HTTP firmware update. GET / form, POST /update blob.
#define OTA_HTTP_PORT 80

// Core split: WiFi driver + inject task on core 0; Ethernet/lwIP/console/OTA
// on core 1 (see sdkconfig.defaults).
#define WIFI_RADIO_TASK_CORE 0
#define APP_TASK_CORE 1
#define WIFI_RADIO_TASK_PRIO 20
#define UPSTREAM_TASK_PRIO 18
#define CONSOLE_TASK_PRIO 5

// Raw 802.11 radio.
#define WIFI_RADIO_MAX_FRAME 1600
#define WIFI_RADIO_INJECT_MIN 24
#define WIFI_RADIO_INJECT_MAX 1500
#define WIFI_RADIO_RX_QUEUE 16
#define WIFI_RADIO_TX_QUEUE 16
#define WIFI_RADIO_INJECT_RETRIES 8
#define WIFI_DEFAULT_CHANNEL 1
#define WIFI_DEFAULT_MODULATION "DSS_1M_L"
#define WIFI_AP_CHANNEL WIFI_DEFAULT_CHANNEL

#define WIFI_HDR_LEN 24
#define WIFI_QOS_HDR_LEN 26
#define WIFI_PAYLOAD_MAX (WIFI_RADIO_INJECT_MAX - WIFI_HDR_LEN)
#define WIFI_BSSID 0xBA, 0xDD, 0xCA, 0xFE, 0xBA, 0xBE
