#ifndef WINJECT_CONFIG_H_
#define WINJECT_CONFIG_H_

#include <stdint.h>

#define DEVICE_HOSTNAME "winject-esp32"

// LAN8720 RMII wiring (PHY addr / MDC / MDIO / power shared).
// RMII REF_CLK is selected by PlatformIO env (see platformio.ini):
//   wt32-eth01  → ETH_CLK_GPIO0_IN   (external 50 MHz into GPIO0)
//   lan-module  → ETH_CLK_GPIO17_OUT (ESP32 APLL clock out on GPIO17)
#define ETH_PHY_ADDR 1
#define ETH_PHY_MDC 23
#define ETH_PHY_MDIO 18
#define ETH_PHY_POWER 16
#define ETH_CLK_GPIO0_IN 0
#define ETH_CLK_GPIO17_OUT 1
#ifndef ETH_CLK_MODE
#define ETH_CLK_MODE ETH_CLK_GPIO0_IN
#endif

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

// UDP control-plane console.
#define CONTROL_CONSOLE_PORT 22
// Max ether_test_* binary payload (fits one Ethernet UDP datagram with header).
#define ETHER_TEST_MAX 1400
// Dedicated Ethernet flood bench (not console RTT). UDP payload incl. 8-byte hdr.
#define ETHER_BENCH_PORT 2223
#define ETHER_BENCH_HDR 8
// IPv4/UDP L2 size = 14+20+8+payload; must fit CONFIG_ETH_DMA_BUFFER_SIZE (1514).
#define ETHER_BENCH_MAX 1472
// Max L2 frame for 1400 B inject UDP (bw_test / manager path).
#define WINJECT_ETH_L2_INJECT_MAX (14u + 20u + 8u + 1400u)
// 1500-byte IP MTU Ethernet frame (L2 header only; FCS not in DMA buffer).
#define WINJECT_ETH_L2_MTU_MAX (14u + 1500u)
#define ETHER_BENCH_MAGIC 0xEB01u
// Must stay below CONFIG_LWIP_TCPIP_TASK_PRIO (18) or the flood starves
// lwIP/EMAC and exhausts RX buffers ("esp.emac: no mem for receive buffer").
// TX stays below tcpip (18) so device→host flood does not starve lwIP/EMAC.
// RX bench at 18 matches tcpip so host→device can dequeue as fast as UDP
// is posted (still yields in rx_task for TWDT).
#define ETHER_BENCH_RX_TASK_PRIO 18
#define ETHER_BENCH_TX_TASK_PRIO 17
#define ETHER_BENCH_TASK_STACK 2048
#define ETHER_BENCH_SOCK_BUF 128 * 1024
#define ETHER_BENCH_RX_YIELD_EVERY 32

// AUTO: if the DHCP client has no lease by this time, apply set_ip /
// ETH_FALLBACK_ADDR as a static /24. Host must be on that subnet (or use
// DHCP on a LAN that leases the radio). The radio never runs a DHCP server.
//
// Bench (192.168.253.0/24, server .1), 2026-09-17:
//   DORA DISCOVER→ACK ≈ 36–50 ms (p50 ≈ 40 ms).
// ESP-IDF then runs DHCP ARP conflict check (acd_dhcp_check.c): two probes
// at ACD_DHCP_ARP_REPLY_TIMEOUT_MS=500 → ≈ 1000 ms before GOT_IP / has_ipv4.
// Quiet path ≈ 1.05 s, but WiFi init overlaps DHCP on boot and can push
// GOT_IP past ~1.5 s — a tight timer then kills dhcpc mid-ACD and pins
// 192.168.32.1 on the LAN (unreachable). Keep several seconds of margin.
#define DHCP_FALLBACK_MS 5000
#define ETH_FALLBACK_ADDR 192, 168, 32, 1

// HTTP firmware update. GET / form, POST /update blob.
#define OTA_HTTP_PORT 80

// Core split: WiFi driver + inject task on core 0; Ethernet/lwIP/console/OTA
// on core 1 (see sdkconfig.defaults).
#define WIFI_RADIO_TASK_CORE 0
#define APP_TASK_CORE 1
#define WIFI_RADIO_TASK_PRIO 20
#define UPSTREAM_TASK_PRIO 18
#define NETMGR_TASK_PRIO 6
#define UPSTREAM_RX_TASK_PRIO 17
#define UPSTREAM_RX_TASK_STACK 4096

// Raw 802.11 radio.
#define WIFI_RADIO_MAX_FRAME 1600
#define WIFI_RADIO_INJECT_MIN 24
#define WIFI_RADIO_INJECT_MAX 1500
// Pool storage is sized per-direction (see packet_allocator init).
#define WIFI_RADIO_RX_QUEUE 8
#define WIFI_RADIO_TX_QUEUE 20
// wifi_tx: EMAC/WiFi timeshare — drain a small batch then idle (see winject.md).
#define WIFI_TX_BURST_SIZE_DEFAULT 8
#define WIFI_TX_BURST_GAP_US 1000
#define WIFI_RADIO_INJECT_RETRIES 8
// Cap outstanding 802.11 TX before submitting another — avoids NO_MEM storms
// that thrash the WiFi DMA and starve EMAC RX. Do not raise without re-checking
// ETH→sut goodput (see docs/winject.md#ethwifi-tx-performance).
#define WIFI_RADIO_MAX_IN_FLIGHT 4
// NO_MEM: yield this many times before a 1-tick sleep (driver ring recovery).
// Higher helps sustain ~MCS7 30 Mbps inject without thrashing on 1 ms sleeps.
#define WIFI_RADIO_INJECT_NOMEM_YIELD 48
#define WIFI_CHANNEL_MIN 1
#define WIFI_CHANNEL_MAX 14
#define WIFI_DEFAULT_CHANNEL 1
#define WIFI_DEFAULT_MODULATION "DSS_1M_L"
#define WIFI_DEFAULT_TX_POWER_DBM 20
#define WIFI_TX_POWER_DBM_MIN 2
#define WIFI_TX_POWER_DBM_MAX 20
// Legacy 64-QAM (48M/54M) raw inject clips above this; peer promisc RX fails
// while USB monitor still looks fine at 20 dBm.
#define WIFI_TX_POWER_64QAM_LEGACY_MAX_DBM 13

#define WIFI_HDR_LEN 24
#define WIFI_QOS_HDR_LEN 26
#define WIFI_PAYLOAD_MAX (WIFI_RADIO_INJECT_MAX - WIFI_HDR_LEN)
#define WIFI_TX_HEADROOM WIFI_HDR_LEN
#define WIFI_TX_PACKET_CAP (WIFI_TX_HEADROOM + WIFI_PAYLOAD_MAX)
#define WIFI_PDU_SLOTS 5
#define WIFI_AIRPORT_MAX 128
#define WIFI_BSSID_PREFIX_TUNNEL 0xBA, 0xDD, 0xCA, 0xFE
#define WIFI_BSSID_PREFIX_STANDALONE 0xCA, 0xFE, 0xBA, 0xBE

#define SETTINGS_SLOT_COUNT 10
#define SETTINGS_MODULATION_MAX 16

#endif  // WINJECT_CONFIG_H_
