# WInject-ESP32

WInject-ESP is a **bfc-tunnel external multicast** radio.

```
bfc-tunnel  --UDP payload-->  WT32-ETH01  --802.11 TX-->  WiFi
bfc-tunnel  <--UDP payload--  WT32-ETH01  <--802.11 RX--  WiFi
bfc-tunnel  --TCP cmds-->  control plane
```

# 802.11 frame

Firmware owns the MAC header (no radiotap, no FCS on the UDP path). IBSS data, `ToDS=0`, `FromDS=0`:

```
Offset  Size  Field
0       2     Frame Control  0x0008
2       2     Duration       0x0000
4       6     Addr1 DA       FF:FF:FF:FF:FF:FF     broadcast dst
10      6     Addr2 SA       mode + airport (see Operating Modes)
16      6     Addr3 BSSID    mode-selected group address
22      2     Sequence       firmware-owned
24      N     Payload        UDP datagram
```

RX accepts Data (and QoS Data) whose Addr3 is the mode BSSID. In `BFC_TUNNEL_DEVICE`, Addr3 is the tunnel BSSID; in `STANDALONE`, Addr3 is the standalone BSSID **and** Addr2 is a configured airport (RX table as-is, or the TX-table filter SA). Forwarding also requires Addr1 broadcast, not our TX SA, and a `set_upstream_tx` dest (tunnel: any dest; standalone: Addr2 is the dest airport on broadcast, or the swapped P2P response). Then the body is forwarded. Max payload 1476 bytes.

| Mode | Addr2 SA (TX) | Addr3 BSSID | Airport |
|------|---------------|-------------|---------|
| `BFC_TUNNEL_DEVICE` | WiFi STA MAC (eFuse) | `BA:DD:CA:FE:BA:BE` | ignored, always `0` |
| `STANDALONE` | airport MAC | `DE:AD:CA:FE:BA:BE` | `00:00:00:xx:xx:xx` broadcast, or `xx:xx:xx:yy:yy:yy` P2P |

# Control Plane Console

Available on **TCP** `2323` after Ethernet has an IP (DHCP lease in `AUTO`, or the static address). UART0 is logs only (115200).

Commands:

- `set_upstream_rx|sur [airport] <udpport>` - bind this UDP port and inject each datagram as one 802.11 frame. Airport defaults to `0`. In `STANDALONE`, airport is the TX SA (`00:00:00:xx:xx:xx` broadcast or `xx:xx:xx:yy:yy:yy` P2P); repeat the command with another airport to add another endpoint.
- `set_upstream_tx|sut [airport] <udp_host> <udp_port>` - forward matching WiFi payloads to this host. Airport defaults to `0`. In `STANDALONE`, use the same airport as inject: broadcast matches Addr2 as-is, P2P matches the swapped response `yy:yy:yy:xx:xx:xx`. Repeat to add another dest.
- `unset_upstream_rx [airport]` - unbind the RX UDP socket for this airport. Empty uses the default airport `0`.
- `unset_upstream_tx [airport]` - remove the TX forwarding entry for this airport. Empty uses the default airport `0`.
- `set_allow_failed_crc|saf <allow>` - Forward failed CRC to upstream (`1`/`0`, `true`/`false`, `on`/`off`). Default `0`.
- `set_mode|sm <mode>` - set operating mode
    - Available modes
    - `BFC_TUNNEL_DEVICE` (default) - Used as bfc-tunnel external multicast device.
    - `STANDALONE` - Used as standalone winject device.
- `set_channel|sc <channel>` - Set WIFI channel (1–13)
- `set_modulation|sd <modulation>` - Set WIFI modulation

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
- `set_network|sn <mode>` - Set Ethernet address mode
	- Available modes
    - `STATIC` - use the address from `set_ip`. DHCP server is allowed.
    - `AUTO` (default) - DHCP client. If no lease in 5 s, apply `set_ip` as a static fallback. DHCP server is blocked.
- `set_enable_dhcp_server|sed <enabled>` - Enable the DHCP server (`1`/`0`, `true`/`false`, `on`/`off`). Default `0`. Takes effect only in `STATIC`; in `AUTO` the setting is stored but the server stays blocked.
- `set_ip|sfi <ip>` - Set the static / fallback address. In `STATIC`, the DHCP server (if enabled) serves that `/24`; pool is host `.1`–`.64` except the device if it is in that range. `.65`–`.254` are for static/external hosts.
- `save|sv <slot>` - save settings to NVS slot `0`–`9` and make that slot current
- `save|sv` - save to the current slot (last `save <n>` / `use <n>`, or `0` at first boot)
- `use|u <slot>` - load slot `0`–`9`, apply it, and make it current. Empty slot is an error.
- `set_cca_enabled|sce` `<is_enabled>` - Enable or disable TX CCA / CSMA (`1`/`0`, `true`/`false`, `on`/`off`). Disabling lets inject skip wait-for-idle.
- `set_tx_power|stp` `<dbm>` - Maximum Wi-Fi TX power in dBm (`2`–`20`). Default `20`. ESP32 maps this to 0.25 dBm units internally.
- `status|s` - Ethernet, mode, radio, and upstream

Also: `help|h`.

Defaults: channel `1`, modulation `DSS_1M_L`, CCA enabled, TX power `20` dBm, `allow_failed_crc` off, network `AUTO`, DHCP server off, static/fallback `192.168.32.1`. Upstream RX/TX are unset until configured. Boot loads the last-used slot (default slot `0`); an empty slot `0` keeps these defaults. UDP payload is the tunnel body only (no 802.11 header, no radiotap, no FCS). Max payload 1476 bytes. `set_upstream_rx` binds the Ethernet unicast; the air-to-UDP send socket binds `0.0.0.0` so `set_ip` does not break TX.

# Operating Modes

Airport is the 802.11 source address, not a UDP port. The two modes pick different BSSIDs so tunnel and standalone traffic on the same channel do not mix.

## `BFC_TUNNEL_DEVICE` (default)

One multicast radio for bfc-tunnel. Identity is the chip, not the console airport.

- Airport is hardcoded to `0`. If the argument is present it must be `0`; any other value is an error.
- TX Addr2 is the WiFi STA MAC from `esp_wifi_get_mac(WIFI_IF_STA)` (factory eFuse). Every flashed board already has a unique SA.
- Addr3 is `BA:DD:CA:FE:BA:BE`.
- RX heard: Addr3 is that BSSID (other networks on the channel are ignored).
- RX match: heard + DA broadcast + not own STA SA + at least one `set_upstream_tx`. Every other radio on the tunnel group is forwarded to every TX dest.
- Omitted airport on `set_upstream_rx` / `set_upstream_tx` stays valid (`0`).

```
set_mode BFC_TUNNEL_DEVICE
set_upstream_rx 9000
set_upstream_tx 192.168.253.10 9001
```

## `STANDALONE`

Same role as [../winject](../../winject): a point-to-point inject radio with operator-assigned identity and several UDP endpoints. It is not a clone of that stack. There is still no LLC, FEC, or PDCP.

Original winject packs Addr2 as `SRC(24)|DST(24)` (`-src 000101 -dst 000202`) and muxes several L3 PDUs into one 802.11 body (LLC `nLC` / LCID). Standalone muxes by **frame**: one UDP datagram is one 802.11 MPDU, and the airport **is** Addr2. Demux on RX is “which SA”, not “which LCID in the payload”.

Standalone airports are 3-byte halves, written `xx:xx:xx:yy:yy:yy` or `xxxxxxxxxxxx` (unicast; not all-zero; not IEEE broadcast/multicast). P2P also requires the peer half `yy:yy:yy` to be unicast so the swapped response SA is valid.

| Form | Airport | TX Addr2 | RX filter |
|------|---------|----------|-----------|
| Broadcast | `00:00:00:xx:xx:xx` | `00:00:00:xx:xx:xx` | same SA (no swap) |
| P2P | `xx:xx:xx:yy:yy:yy` (X = you, Y = peer) | `xx:xx:xx:yy:yy:yy` | `yy:yy:yy:xx:xx:xx` |

- Each `set_upstream_rx <airport> <udpport>` binds that UDP port and stamps that airport as Addr2 on inject. Another airport adds another binding; the same airport replaces the port.
- Each `set_upstream_tx <airport> <udp_host> <udp_port>` uses the same airport string as inject. Broadcast forwards frames whose Addr2 equals that airport. P2P forwards the swapped response `yy:yy:yy:xx:xx:xx`. Another airport adds another dest; the same airport replaces the dest.
- Addr3 is `DE:AD:CA:FE:BA:BE` (not winject’s `DE:AD:BE:EF:CA:FE`). Same-channel PC winject and ESP32 standalone do not match.
- RX heard: Addr3 is that BSSID **and** Addr2 is a local TX SA or a TX-table filter SA. Other SAs on that BSSID are ignored.
- RX match: heard + DA broadcast + SA matches a `set_upstream_tx` filter. Drop P2P SAs that are local RX-table airports (no self-loop). Broadcast airports share Addr2 on every radio, so they are not classified as self.
- The STA MAC is not used in the 802.11 header. `status` still prints it for the AP SSID / board identity.
- Two P2P radios that should hear each other use swapped airports: A is `xx:xx:xx:yy:yy:yy`, B is `yy:yy:yy:xx:xx:xx`. Each radio uses that string on both `set_upstream_rx` and `set_upstream_tx`. Firmware filters the swap for the response.

```
set_mode STANDALONE
set_upstream_rx 02:02:03:04:05:06 9000
set_upstream_tx 02:02:03:04:05:06 192.168.253.10 9001
set_upstream_rx 00:00:00:AA:BB:CC 9010
set_upstream_tx 00:00:00:AA:BB:CC 192.168.253.10 9011
```

That is one P2P link and one broadcast endpoint on the same radio. A datagram on UDP 9000 goes out as SA `02:02:03:04:05:06`; an air frame with SA `04:05:06:02:02:03` is sent to `…:9001`. Broadcast inject on UDP 9010 uses SA `00:00:00:AA:BB:CC`; receivers that configured that same broadcast airport forward it to `…:9011`. Each UDP bind is a second 802.11 frame, not a second LLC in the first frame.

On the peer radio the P2P pair is swapped:

```
set_mode STANDALONE
set_upstream_rx 04:05:06:02:02:03 9000
set_upstream_tx 04:05:06:02:02:03 192.168.253.10 9002
```

`set_mode` changes BSSID and SA rules immediately. Existing UDP binds stay; the next inject and the next RX match use the new mode. `save` / `use` persist mode, radio, network mode, static IP, DHCP server enable, and the airport tables in NVS slots `0`–`9`. Boot loads the last-used slot.

# Ethernet addressing

The raw 802.11 radio starts at boot in `BFC_TUNNEL_DEVICE` or `STANDALONE` and stays there. OTA and the TCP console listen on Ethernet as soon as it has an IPv4 address.

`AUTO` (default) runs a DHCP client. If no lease arrives within **5 seconds**, Ethernet applies the static address from `set_ip`. That fallback is static only: it does not start a DHCP server.

`STATIC` uses `set_ip` immediately. `set_enable_dhcp_server 1` starts a DHCP server on that `/24`; the server is blocked automatically in `AUTO` and unblocked on `STATIC`.

- Default address `192.168.32.1/24` (`set_ip` changes this)
- DHCP pool host `.1`–`.64`, minus the device host if it falls in that range (contiguous; if the device sits in the middle, the larger remaining side is used)
- Hosts `.65`–`.254` are left for static / externally managed addresses
- TCP console `2323` and OTA on the DHCP lease or the static address

`set_ip 192.168.32.1` → pool `192.168.32.2`–`.64`. `set_ip 192.168.32.100` → pool `192.168.32.1`–`.64`.

The STA MAC is the chip-unique factory MAC (eFuse). Ethernet uses the derived `ESP_MAC_ETH` address. One firmware image can be flashed to every radio.

# OTA

Once Ethernet has an IPv4 address (DHCP lease or static), HTTP OTA is served on port 80:

- `GET /` upload form
- `POST /update` firmware blob (multipart or raw)

```bash
curl -F "firmware=@.pio/build/wt32-eth01/firmware.bin" http://192.168.32.1/update
```

Use the DHCP-assigned Ethernet address when a lease arrived before the 5 s timeout. There is no OTA password.
