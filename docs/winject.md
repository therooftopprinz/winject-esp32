# WInject-ESP32

ESP-IDF firmware for Wireless-Tag **WT32-ETH01** (ESP32 + LAN8720): a **bfc-tunnel external multicast** radio. Addressing on the air is a **bus** (`lcid` / `uint8`), not a peer airport MAC. Up to five PDUs may share one 802.11 MPDU; demux is by bus only. This is the as-built reference; [refactor.md](refactor.md) is the design precursor.

```
host  --UDP payload-->  WT32-ETH01  --802.11 TX-->  WiFi
host  <--UDP payload--  WT32-ETH01  <--802.11 RX--  WiFi
host  --TCP cmds-->  control plane (2323)
host  <--UDP channel_info--  WT32-ETH01
```

There is no LLC mux header, no FEC, and no PDCP in firmware. UDP payloads are LCP bodies only (no 802.11 header, no radiotap, no FCS). Max payload **1476** bytes (`WIFI_PAYLOAD_MAX` = 1500 − 24). Max present PDUs per MPDU: **5**.

**Data path:** `lc_tx_endpoint` (UDP bind) → `lc_tx` queue → `wifi_tx` (combine ≤5, stamp, inject). Air RX: `wifi_rx` promiscuous → `lc_rx` (unpack slots) → `lc_rx_endpoint` (fan-out). **CPU0** runs the WiFi driver + inject task; **CPU1** runs Ethernet/lwIP, TCP console, OTA, and endpoint tasks.

# 802.11 frame

Firmware owns the MAC header. IBSS data, `ToDS=0`, `FromDS=0`:

```
Offset  Size  Field
0       2     Frame Control  0x0008
2       2     Duration       0x0000
4       6     Addr1          PDU slot bit packing (with Addr2)
10      6     Addr2          PDU slot bit packing (with Addr1)
16      6     Addr3          mode BSSID prefix + domain
22      2     Sequence       firmware-owned
24      N     Body           concatenated payloads (present slots only)
```

Arbitrary Addr1/Addr2 rely on the `ieee80211_raw_frame_sanity_check` override (`return 0`) and `-Wl,-z,muldefs`. The STA eFuse MAC is board identity / logs only — it is **not** on the air.

### Addr1 || Addr2 — PDU slots (96 bits)

Treat `Addr1[0..5] || Addr2[0..5]` as a **96-bit LSB-first** bit stream (bit 0 = LSB of `Addr1[0]` = 802.11 **I/G**).

Five fixed slots × (`bus` `uint8` + `size` `uint11`) = **95 bits**, plus **1 bit** reserved as Addr1 I/G (former trailing spare):

```
emit ig           (1 bit, always 1 on TX)   # group DA → raw inject skips ACK wait
for slot i = 0..4:
    emit bus[i]   (8 bits, LSB-first)
    emit size[i]  (11 bits, LSB-first)
```

| Field | Meaning |
|-------|---------|
| `ig` | **1** on TX (group / multicast DA). RX ignores. Without this, an even `bus` made Addr1 unicast and the ESP MAC waited for ACKs that never arrived. |
| `bus` | logical-channel / bus id (`0` = broadcast). Also called **lcid**. Full 8-bit value; not constrained by I/G. |
| `size` | payload length in bytes; **`0` = slot absent** (`bus` ignored, no body bytes) |

**Present PDUs** = slots with `size > 0`, in order `0…4`. Body = those payloads concatenated. RX drops if `sum(sizes) != body_len`, or any `size > WIFI_PAYLOAD_MAX`.

### Addr3 — mode BSSID + domain

Last two octets = big-endian **domain** (`uint16`). Domain `0` is unset / invalid on the air.

| Mode | Addr3 prefix (4 bytes) | Full form |
|------|------------------------|-----------|
| `STANDALONE` | `CA:FE:BA:BE` | `CA:FE:BA:BE:DH:DL` |
| `BFC_TUNNEL_DEVICE` | `BA:DD:CA:FE` | `BA:DD:CA:FE:DH:DL` |

| Domain | Last two octets | Standalone Addr3 | Tunnel Addr3 |
|--------|-----------------|------------------|--------------|
| `0x0001` | `00:01` | `CA:FE:BA:BE:00:01` | `BA:DD:CA:FE:00:01` |
| `0x1234` | `12:34` | `CA:FE:BA:BE:12:34` | `BA:DD:CA:FE:12:34` |

RX: Addr3 prefix must match the local mode; domain must equal the local domain (unset or mismatch → drop).

### Header examples

**One PDU.** Domain `0x1234`, slot 0 `bus=0xB2` `size=3`, body `AA BB CC`:

```
Addr1  65:07:00:00:00:00   (I/G=1, then bus/size…)
Addr2  00:00:00:00:00:00
Addr3  BA:DD:CA:FE:12:34   (tunnel) or CA:FE:BA:BE:12:34 (standalone)
Body   AA BB CC
```

**Two PDUs.** Slot 0 `bus=0xB2` `size=2` (`AA BB`), slot 1 `bus=0xC3` `size=3` (`DD EE FF`):

```
Addr1  65:05:30:3C:00:00
Addr2  00:00:00:00:00:00
Body   AA BB DD EE FF
```

### PDU combine (TX)

`wifi_tx` may pack several queued LCPs into one MPDU:

1. Pop `lc_tx` head → output buffer (already has 24-byte headroom).
2. Slot 0 ← `{bus, size}`; present count = 1.
3. While present `< 5` and body + next payload ≤ `WIFI_PAYLOAD_MAX`, pop the next donor (any bus), append its payload, fill the next slot.
4. Pack slots into Addr1||Addr2; stamp Addr3 = mode prefix + domain; inject.

Solo send skips step 3 (no copy). Domain unset → TX drops without inject.

# Control Plane Console

Available on **TCP** `2323` after Ethernet has an IP (DHCP lease in `AUTO`, or the static address). Up to **4** concurrent clients. Banner: `WInject-ESP32  bus/domain radio`. UART0 is logs only (115200). Bool args accept `0|1|true|false|on|off|yes|no`. Radio commands need the radio path (unavailable in `OTA`). Upstream bind/forward tables hold up to **128** entries (`WIFI_AIRPORT_MAX`).

Commands:

- `set_mode|sm <mode>` - set operating mode (`BFC_TUNNEL_DEVICE`, `STANDALONE`, `OTA`). Switching to/from `OTA` persists and reboots. Other mode changes apply live (BSSID prefix) until `save`.
- `set_domain|sdom <domain>` - set air domain (`1`–`65535`, hex; optional `0x` prefix). Required for TX and RX.
- `unset_domain|udom` - clear domain (`0`); air path invalid until set again.
- `set_upstream_tx|sut bus=<lcid> <udp_port>` - bind this UDP port; each datagram is one LCP stamped with that bus on TX. Same bus replaces; same port on another bus steals the port. Bind is `0.0.0.0`.
- `unset_upstream_tx|uut bus=<lcid>` - unbind the inject UDP socket for that bus.
- `set_upstream_rx|sur bus=<lcid> <host> <udp_port>` - forward air PDUs matching that bus to this host. Same bus+dest is idempotent; multiple dests per bus are allowed. Exact bus match only (`bus=0` matches broadcast slots only).
- `unset_upstream_rx|uur bus=<lcid>` - remove all forward dests for that bus.
- `set_upstream_ci|suc to=<host>:<port>` - subscribe to channel-info UDP (flow control + RX air metrics).
- `unset_upstream_ci|usuc to=<host>:<port>` - remove a channel-info subscriber.
- `set_logger|sl address=<host>:<port> level=<error|warn|info|debug>` - single UDP text logger (replaces any previous dest). Rate-limited (~50 Hz). Not persisted in NVS. Works in `OTA`.
- `unset_logger|ul` - disable the UDP logger.
- `set_allow_failed_crc|saf <allow>` - Forward failed-CRC air frames (bool). Default `0`.
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
- `set_enable_dhcp_server|sed <enabled>` - Enable the DHCP server (bool). Default `0`. Takes effect only in `STATIC`; in `AUTO` the setting is stored but the server stays blocked.
- `set_ip|sfi <ip>` / `set_fallback_ip|sfi <ip>` - Set the static / fallback address. In `STATIC`, the DHCP server (if enabled) serves that `/24`; pool is host `.1`–`.64` except the device if it is in that range. `.65`–`.254` are for static/external hosts.
- `save|sv [0-9]` - save settings to NVS slot `0`–`9` (or the current slot if omitted) and make that slot current. Blob **v3**: mode, radio, network, domain, `sut`/`sur` tables, CI subscribers — **not** the UDP logger.
- `use|u <slot>` - load slot `0`–`9`, apply it, and make it current. Empty slot is an error.
- `set_cca_enabled|sce <is_enabled>` - Enable or disable TX CCA / CSMA (bool). Disabling lets inject skip wait-for-idle.
- `set_tx_power|stp <dbm>` - Maximum Wi-Fi TX power in dBm (`2`–`20`). Default `20`. ESP32 maps this to 0.25 dBm units internally.
- `status|s` - device, network, wifi, upstreams, channel metrics, logger, channel_info
- `reset|r` - software-restart the ESP32 (`esp_restart`)

Also: `help` / `?`.

**Naming:** `set_upstream_tx` / `sut` = host **transmits** onto the air (UDP → radio). `set_upstream_rx` / `sur` = host **receives** from the air (radio → UDP). Bus value is 1–2 hex digits (`bus=b2`, `bus=0`).

Defaults: channel `1`, modulation `DSS_1M_L`, CCA enabled, TX power `20` dBm, `allow_failed_crc` off, network `AUTO`, DHCP server off, static/fallback `192.168.32.1`, domain unset, no upstreams, no CI subscribers, logger unset. Boot loads the last-used NVS slot (default `0`); an empty slot `0` keeps these defaults. TX/RX packet pools and LC queues are depth **16**.

# Domain and bus

## Domain

| Command | Alias | Arguments |
|---------|-------|-----------|
| `set_domain` | `sdom` | `<domain>` — hex `1`…`65535` (`0` reserved / unset) |
| `unset_domain` | `udom` | (none) |

Written into Addr3 last two octets on every TX. RX drops frames whose domain ≠ local domain. Persisted with `save` / `use`.

## Bus (`bus=<lcid>`)

Canonical console key: **`bus`**. One byte; `bus=0` is broadcast.

| Path | Console | Bus role |
|------|---------|----------|
| UDP → air | `set_upstream_tx` / `sut` | stamp this bus into the air slot |
| air → UDP | `set_upstream_rx` / `sur` | match present air slots with this bus |

**RX match (exact, not wildcard):**

| Air slot `bus` | Matches `sur` bind |
|----------------|--------------------|
| `N` (1…255) | only `bind.bus == N` |
| `0` (broadcast) | only `bind.bus == 0` |

A broadcast air slot does **not** fan out to every `sur` bind. A non-zero air bus does **not** match a `bus=0` bind. Multiple `sur` dests for the same bus all receive a copy. Self-TX frames that match a local `sur` are forwarded like any other (no self-TX blackhole).

# Operating Modes

Both radio modes use the same Addr1/Addr2 packing. Only the Addr3 prefix differs, so tunnel and standalone traffic on the same channel and domain do not mix.

## `BFC_TUNNEL_DEVICE` (default)

Multicast radio for bfc-tunnel. Same combine path as standalone.

```
set_mode BFC_TUNNEL_DEVICE
set_domain 1234
set_upstream_tx bus=b2 9000
set_upstream_rx bus=a1 192.168.32.10 9001
```

## `STANDALONE`

Same wire format; Addr3 prefix is `CA:FE:BA:BE`. Use distinct buses for distinct flows (e.g. data vs ACK). Example — two radios, data bus `b2` A→B and return bus `a1` B→A, domain `0x1234`:

```
# A
set_mode STANDALONE
set_domain 1234
set_upstream_tx bus=b2 9000
set_upstream_rx bus=a1 192.168.32.10 9001

# B
set_mode STANDALONE
set_domain 1234
set_upstream_tx bus=a1 9000
set_upstream_rx bus=b2 192.168.32.11 9001
```

## `OTA`

Boot-only / console-selected: Ethernet + TCP console + HTTP OTA only — **no WiFi, no LC, no endpoints**. Radio console commands return `error: unavailable in OTA mode`. Logger commands still work. Entering or leaving `OTA` via `set_mode` persists the mode and reboots.

# Channel info

UDP telemetry to `set_upstream_ci` subscribers (packed little structs):

| `info_type` | Payload | When |
|-------------|---------|------|
| `1` `E_CHANNEL_INFO_TYPE_FLOW_CTRL` | `tx_queue_size`, `tx_queue_capacity` | published when `lc_tx` occupancy is above half capacity |
| `2` `E_CHANNEL_INFO_TYPE_RX_AIR` | `rssi`, `snr` | ~100 ms while air RX is valid |

# UDP logger

Runtime diagnostic text lines to one host (not persisted). Host helper: `python3 tools/udp_log.py --port 9999`.

```
set_logger address=192.168.32.10:9999 level=warn
unset_logger
```

| Level | Includes |
|-------|----------|
| `error` | `tx_fail` (inject abandoned) |
| `warn` | `tx_retry` (`NO_MEM` / other), `tx_drop no_pkt_pool` / `queue_full` |
| `info` | `tx_ok after_retries=…` |
| `debug` | every successful `tx_ok` |

Line shape: `<us_timestamp> <level> <message>\n`. Only one logger at a time; `set_logger` replaces the previous dest/level.

# Status page

TCP console `status|s` prints a sectioned snapshot (`#` headers). Fields are space-separated `key=value` tokens.

```
# device
device uptime=<s>s reset_reason=<name> heap_usage=<bytes> heap_free=<bytes> mode=<mode> domain=<hex|unset>
# network
network ip=<ipv4|-> console_port=2323
dhcp mode=<STATIC|AUTO> static_ip=<ipv4> dhcps=<off|blocked|enabled|active> pool=<start>-<end_host> netmask=<a.b.c.0>/24
ota url=http://<ip>:80/update
  | ota waiting
# wifi
wifi channel=<1-13> modulation=<code> cca=<enabled|disabled> tx_power=<2-20> allow_failed_crc=<true|false>
# radio
radio rssi=<dBm|-> snr=<dB|->
# upstreams
upstream_tx bus=<hex> address=<udp_port>
upstream_rx bus=<hex> address=<host>:<port>
  | upstream unset
# channel
channel_tx queue=<n> drop_nomem=<n> retry_count=<n> retry_nomem=<n> retry_other=<n>
           inject_ok=<n> inject_fail=<n> in_flight=<n> tx_latency=<uus|-> inject_wait=<uus|->
channel_rx queue=<n> drop_no_pkt_pool=<n> drop_queue_full=<n> drop_crc_error=<n>
channels_tx bus=<hex> drop_no_pkt_pool=<n> drop_queue_full=<n>
channels_rx bus=<hex> drop_send_fail=<n>
# logger
logger address=<host>:<port> level=<error|warn|info|debug> emitted=<n> dropped=<n>
  | logger unset
# channel_info
channel_info subscribers=<host:port[,…]|->
```

In `OTA` mode, after network/ota lines status prints `# radio` / `radio disabled (OTA mode)` and stops (no wifi, upstream, channel, logger, or channel_info sections).

| Line | Meaning |
|------|---------|
| `device` | Seconds since boot; last reset cause (`power-on`, `external`, `software`, `panic`, `interrupt-wdt`, `task-wdt`, `wdt`, `brownout`, `unknown`); 8-bit heap used/free; mode; domain hex or `unset` |
| `network` | Current Ethernet IPv4 (`-` until assigned); TCP console port |
| `dhcp` | Network mode; `set_ip` static/fallback; DHCP server state (`off`, `blocked` in `AUTO` when enabled, `enabled` in `STATIC` but not running, `active` when serving); pool start–end host octet; `/24` of the static address |
| `ota` | Upload URL once Ethernet has an IP; otherwise `ota waiting` |
| `wifi` | Configured channel, TX modulation code, CCA, TX power dBm, CRC-forward policy |
| `radio` | Last heard air RSSI/SNR (`-` until a frame is accepted) |
| `upstream_tx` / `upstream_rx` | One line per inject bind / forward dest. With none: `upstream unset` |
| `channel_tx` | `lc_tx` depth; final inject abandon (`drop_nomem`, any exhausted retry); total / `NO_MEM` (infinite retry) / other (`WIFI_RADIO_INJECT_RETRIES`) `esp_wifi_80211_tx` retries; successful / failed injects; pending TX-done count; mean head-of-line submit→TX-done latency; mean wall time inside `inject_retry` |
| `channel_rx` | RX queue depth; pool / queue / CRC drops |
| `channels_tx` / `channels_rx` | Per-bus drop counters for inject / forward |
| `logger` | Active UDP logger dest + level and emit/drop counts, or `logger unset` |
| `channel_info` | Comma-separated UDP subscribers, or `-` when none |

Example (tunnel, one inject and one forward):

```
# device
device uptime=123s reset_reason=power-on heap_usage=45678 heap_free=12345 mode=BFC_TUNNEL_DEVICE domain=1234
# network
network ip=192.168.32.1 console_port=2323
dhcp mode=AUTO static_ip=192.168.32.1 dhcps=off pool=192.168.32.2-64 netmask=192.168.32.0/24
ota url=http://192.168.32.1:80/update
# wifi
wifi channel=1 modulation=DSS_1M_L cca=enabled tx_power=20 allow_failed_crc=false
# radio
radio rssi=-42 snr=18
# upstreams
upstream_tx bus=B2 address=9000
upstream_rx bus=A1 address=192.168.32.10:9001
# channel
channel_tx queue=0 drop_nomem=0 retry_count=0 retry_nomem=0 retry_other=0 inject_ok=0 inject_fail=0 in_flight=0 tx_latency=312us inject_wait=-
channel_rx queue=0 drop_no_pkt_pool=0 drop_queue_full=0 drop_crc_error=0
channels_tx bus=B2 drop_no_pkt_pool=0 drop_queue_full=0
channels_rx bus=A1 drop_send_fail=0
# logger
logger unset
# channel_info
channel_info subscribers=-
```

# Board / indicators

WT32-ETH01 LAN8720: PHY addr 1, MDC 23, MDIO 18, power 16, RMII clock on GPIO0 in (optional GPIO17 out). Activity LEDs (active-low, 1 ms stretch): **IO5** = WiFi RX (silk RXD), **IO17** = WiFi TX (silk TXD). TX LED is disabled when RMII clock uses GPIO17. Hostname `winject-esp32`.

# Ethernet addressing

The radio starts at boot in the configured mode (`BFC_TUNNEL_DEVICE`, `STANDALONE`, or `OTA`). OTA and the TCP console listen on Ethernet as soon as it has an IPv4 address.

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

`set_mode OTA` boots network+console+HTTP only (no WiFi). Leave OTA with `set_mode BFC_TUNNEL_DEVICE` or `set_mode STANDALONE` (persists and reboots).
