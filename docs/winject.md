# WInject-ESP32

WInject-ESP is a **bfc-tunnel external multicast** radio. It is not a clone of original WInject / ppWiFi: there is no LLC, FEC, or PDCP here. The ESP32 wraps and unwraps 802.11 around opaque UDP payloads.

```
bfc-tunnel  --UDP payload-->  WT32-ETH01  --802.11 TX-->  WiFi
bfc-tunnel  <--UDP payload--  WT32-ETH01  <--802.11 RX--  WiFi
bfc-tunnel  --TCP/Serial cmds-->  control plane
```

# 802.11 frame

Firmware owns the MAC header (no radiotap, no FCS on the UDP path). IBSS data, `ToDS=0`, `FromDS=0`:

```
Offset  Size  Field
0       2     Frame Control  0x0008
2       2     Duration       0x0000
4       6     Addr1 DA       FF:FF:FF:FF:FF:FF     broadcast dst
10      6     Addr2 SA       STA MAC
16      6     Addr3 BSSID    BA:DD:CA:FE:BA:BE
22      2     Sequence       firmware-owned
24      N     Payload        UDP datagram
```

RX accepts Data (and QoS Data) with Addr1 broadcast and Addr3 `BA:DD:CA:FE:BA:BE`, then forwards only the body. Max payload 1476 bytes.

# Control Plane Console

Available on **Serial** (115200) and **TCP** `2323` after Ethernet has an IP, or after the WiFi AP fallback is up.

Commands:

- `set_upstream_rx <port>` - listen on this UDP port. Each datagram payload is wrapped in 802.11 and injected.
- `set_upstream_tx <host> <port>` - every matching WiFi payload is forwarded to this host.

- `set_channel <channel>` - Set WIFI channel (1–13)
- `set_modulation <modulation>` - Set WIFI modulation

| Modulation Code | Modulation | Data Rate |
|-----------------|------------|-----------|
| DSS_1M_L | DBPSK (Long Preamble) | 1 Mbps |
| DSS_2M_S | DQPSK (Short Preamble) | 2 Mbps |
| DSS_2M_L | DQPSK (Long Preamble) | 2 Mbps |
| CCK_5M_L | CCK (Long Preamble) | 5.5 Mbps |
| CCK_5M_S | CCK (Short Preamble) | 5.5 Mbps |
| CCK_11M_L | CCK (Long Preamble) | 11 Mbps |
| CCK_11M_S | CCK (Short Preamble) | 11 Mbps |
| OFDM_6M | BPSK | 6 Mbps |
| OFDM_9M | BPSK | 9 Mbps |
| OFDM_12M | QPSK | 12 Mbps |
| OFDM_18M | QPSK | 18 Mbps |
| OFDM_24M | 16-QAM | 24 Mbps |
| OFDM_36M | 16-QAM | 36 Mbps |
| OFDM_48M | 64-QAM | 48 Mbps |
| OFDM_54M | 64-QAM | 54 Mbps |
| OFDM_MCS0_LGI | BPSK | 6.5 Mbps (20MHz), 13.5 Mbps (40MHz) |
| OFDM_MCS1_LGI | QPSK | 13.0 Mbps (20MHz), 27.0 Mbps (40MHz) |
| OFDM_MCS2_LGI | QPSK | 19.5 Mbps (20MHz), 40.5 Mbps (40MHz) |
| OFDM_MCS3_LGI | 16-QAM | 26.0 Mbps (20MHz), 54.0 Mbps (40MHz) |
| OFDM_MCS4_LGI | 16-QAM | 39.0 Mbps (20MHz), 81.0 Mbps (40MHz) |
| OFDM_MCS5_LGI | 64-QAM | 52.0 Mbps (20MHz), 108.0 Mbps (40MHz) |
| OFDM_MCS6_LGI | 64-QAM | 58.5 Mbps (20MHz), 121.5 Mbps (40MHz) |
| OFDM_MCS7_LGI | 64-QAM | 65.0 Mbps (20MHz), 135.0 Mbps (40MHz) |
| OFDM_MCS0_SGI | BPSK | 7.2 Mbps (20MHz), 15.0 Mbps (40MHz) |
| OFDM_MCS1_SGI | QPSK | 14.4 Mbps (20MHz), 30.0 Mbps (40MHz) |
| OFDM_MCS2_SGI | QPSK | 21.7 Mbps (20MHz), 45.0 Mbps (40MHz) |
| OFDM_MCS3_SGI | 16-QAM | 28.9 Mbps (20MHz), 60.0 Mbps (40MHz) |
| OFDM_MCS4_SGI | 16-QAM | 43.3 Mbps (20MHz), 90.0 Mbps (40MHz) |
| OFDM_MCS5_SGI | 64-QAM | 57.8 Mbps (20MHz), 120.0 Mbps (40MHz) |
| OFDM_MCS6_SGI | 64-QAM | 65.0 Mbps (20MHz), 135.0 Mbps (40MHz) |
| OFDM_MCS7_SGI | 64-QAM | 72.2 Mbps (20MHz), 150.0 Mbps (40MHz) |

- `set_cca_enabled` `<is_enabled>` - Enable or disable TX CCA / CSMA (`1`/`0`, `true`/`false`, `on`/`off`). Disabling lets inject skip wait-for-idle.

Also: `status`, `help`.

Defaults: channel `1`, modulation `DSS_1M_L`, CCA enabled. Upstream RX/TX are unset until configured. UDP payload is the tunnel body only (no 802.11 header, no radiotap, no FCS). Max payload 1476 bytes.

# DHCP and WiFi AP fallback

The raw 802.11 radio starts at boot. OTA and the TCP console listen on Ethernet as soon as DHCP assigns an address.

If no IPv4 lease arrives within **5 seconds**, WiFi **also** starts an **open** AP (APSTA; monitor/inject stays up):

- SSID `winject-<STA MAC>` (12 hex digits, no colons), e.g. `winject-123456789ABC`
- No password
- AP address `192.168.4.1`
- TCP console `2323` and OTA on that address **and** on Ethernet if a lease arrives later

The STA MAC is the base MAC from `ETH_MAC_BYTES` in `include/config.h`.

# OTA

Once any IPv4 address is up (Ethernet and/or AP), both endpoints are served:

- **ArduinoOTA** UDP `3232` (PlatformIO `espota`)
- **HTTP** `GET /` upload form, `POST /update` firmware blob

```bash
pio run -t upload --upload-protocol espota --upload-port 192.168.4.1
curl -F "firmware=@.pio/build/wt32-eth01/firmware.bin" http://192.168.4.1/update
```

Use the Ethernet address instead of `192.168.4.1` when DHCP has assigned one. There is no OTA password.
