#ifndef WINJECT_CONFIG_H_
#define WINJECT_CONFIG_H_

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

// WT32-ETH01 UART2 LEDs, active-low (LED on when GPIO is 0).
// LED4 / silk TXD = IO17 = WiFi TX. LED3 / silk RXD = IO5 = WiFi RX.
// GPIO17 is also the optional RMII clock-out pin; TX LED is disabled then.
#define WIFI_RX_LED_GPIO 5
#if ETH_CLK_MODE == ETH_CLK_GPIO17_OUT
#define WIFI_TX_LED_GPIO -1
#else
#define WIFI_TX_LED_GPIO 17
#endif
#define WIFI_LED_ON 0
#define WIFI_LED_OFF 1
// Stretch must exceed indicator_led poll (10 ms) or TX/RX pulses are invisible.
#define WIFI_LED_STRETCH_US 1 * 1000u

// TCP control-plane console.
#define CONTROL_CONSOLE_PORT 2323

// AUTO: if the DHCP client has no lease by this time, apply the static
// Ethernet address (no DHCP server). DHCP server is STATIC-only and off
// until set_enable_dhcp_server. Pool is host .1–.64 except the device host
// if it lands in that range. .65–.254 are for static/external use.
#define DHCP_FALLBACK_MS 5000
#define ETH_FALLBACK_ADDR 192, 168, 32, 1
#define ETH_FALLBACK_DHCP_HOST_MIN 1
#define ETH_FALLBACK_DHCP_HOST_MAX 64

// HTTP firmware update. GET / form, POST /update blob.
#define OTA_HTTP_PORT 80

// Core split: WiFi driver + inject task on core 0; Ethernet/lwIP/console/OTA
// on core 1 (see sdkconfig.defaults).
#define WIFI_RADIO_TASK_CORE 0
#define APP_TASK_CORE 1
#define WIFI_RADIO_TASK_PRIO 20
#define UPSTREAM_TASK_PRIO 18
#define NETMGR_TASK_PRIO 6

// Raw 802.11 radio.
#define WIFI_RADIO_MAX_FRAME 1600
#define WIFI_RADIO_INJECT_MIN 24
#define WIFI_RADIO_INJECT_MAX 1500
// 64×1600 BSS (~100 KB) leaves <6 KB heap on WT32-ETH01 and kills UDP/lwIP.
#define WIFI_RADIO_RX_QUEUE 16
#define WIFI_RADIO_TX_QUEUE 16
#define WIFI_RADIO_INJECT_RETRIES 8
#define WIFI_DEFAULT_CHANNEL 1
#define WIFI_DEFAULT_MODULATION "DSS_1M_L"
#define WIFI_DEFAULT_TX_POWER_DBM 20
#define WIFI_TX_POWER_DBM_MIN 2
#define WIFI_TX_POWER_DBM_MAX 20

#define WIFI_HDR_LEN 24
#define WIFI_QOS_HDR_LEN 26
#define WIFI_PAYLOAD_MAX (WIFI_RADIO_INJECT_MAX - WIFI_HDR_LEN)
#define WIFI_AIRPORT_MAX 128
#define WIFI_BSSID_TUNNEL 0xBA, 0xDD, 0xCA, 0xFE, 0xBA, 0xBE
#define WIFI_BSSID_STANDALONE 0xDE, 0xAD, 0xCA, 0xFE, 0xBA, 0xBE

#define SETTINGS_SLOT_COUNT 10
#define SETTINGS_MODULATION_MAX 16

#endif  // WINJECT_CONFIG_H_
