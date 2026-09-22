# WInject-ESP32

ESP-IDF firmware for Wireless-Tag **WT32-ETH01** (ESP32 + LAN8720): a **bfc-tunnel external multicast** radio. The host (typically winject-manager) stamps the 802.11 MPDU — including Addr1/Addr2 bus slots and Addr3 mode+domain — and the radio injects/forwards full MPDUs. Addressing on the air is still a **bus** (`lcid` / `uint8`); demux is done on the host. Up to five PDUs may share one 802.11 MPDU.

```
          Ethernet UDP                              802.11 air

host  -- full MPDU ------>  WT32-ETH01  -- TX inject -->  peer radios
host  <- full MPDU -------  WT32-ETH01  <- RX forward --  peer radios
host  <- console :22 --->  WT32-ETH01
```

UDP payloads on the single inject/forward pair are **complete 802.11 MPDUs** (24-byte header + body). Max MPDU **1500** bytes. Max present PDUs per MPDU: **5**. Max concatenated body **1476** bytes.

**Data path:** `upstream_tx_endpoint` (L2 hijack adopts the EMAC frame into `packet`) → `wifi_tx` queue → inject as-is. Air RX: **`wifi_rx` promisc CB** (filter, pool **`memcpy`**, enqueue) → **`upstream_rx`** task → UDP forward. See [flow_refactor.md](flow_refactor.md).

# 802.11 frame

The **host** stamps a 24-byte IBSS data header (`ToDS=0`, `FromDS=0`):

```
Offset  Size  Field
0       2     Frame Control  0x0008
2       2     Duration       0x0000
4       6     Addr1          PDU slots (with Addr2)
10      6     Addr2          PDU slots (with Addr1)
16      6     Addr3          mode BSSID prefix + domain
22      2     Sequence CTL   12-bit seq, fragment 0
24      N     Body           concatenated present-slot payloads
```

Inject of these Addr1/Addr2 values uses `ieee80211_raw_frame_sanity_check` returning 0 (`-Wl,-z,muldefs`).

### Addr1 || Addr2 — PDU slots (96 bits)

`Addr1[0..5] || Addr2[0..5]` is a **96-bit LSB-first** stream (bit 0 = LSB of `Addr1[0]` = 802.11 **I/G**):

```
emit ig           (1 bit, 1 on TX)
for slot i = 0..4:
    emit bus[i]   (8 bits, LSB-first)
    emit size[i]  (11 bits, LSB-first)
```

| Field | Meaning |
|-------|---------|
| `ig` | **1** on TX (group DA; raw inject skips ACK wait). RX skips this bit. |
| `bus` | logical-channel / bus id (`0` = broadcast). Also **lcid**. |
| `size` | payload length in bytes; **`0`** = slot absent |

Present PDUs are slots with `size > 0`, in order `0…4`. Body is those payloads concatenated. Host RX accepts when `sum(sizes) == body_len` and every `size ≤ WIFI_PAYLOAD_MAX`.

### Addr3 — mode BSSID + domain

Last two octets are big-endian **domain** (`uint16`). Domain `0` is invalid on the air. The radio still runs `set_mode` / `set_domain` so RX accepts only matching Addr3.

| Mode | Addr3 |
|------|-------|
| `STANDALONE` | `CA:FE:BA:BE:DH:DL` |
| `BFC_TUNNEL_DEVICE` | `BA:DD:CA:FE:DH:DL` |

RX accepts when the prefix matches the local mode and the domain equals the local domain.

### Header examples

**One PDU.** Domain `0x1234`, slot 0 `bus=0xB2` `size=3`, body `AA BB CC`:

```
Addr1  65:07:00:00:00:00
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

The **host** packs queued LCPs into one MPDU (manager combines ≤5). The radio injects the UDP datagram unchanged.

# Control Plane Console

UDP port **22** on Ethernet, once the radio has an IPv4 address (DHCP lease in `AUTO`, or the static address). One command datagram is one line; one reply datagram goes back to the sender (`status` / `help` stay in that same datagram). UART0 is logs at 115200.

Bool args: `0|1|true|false|on|off|yes|no`. Radio commands need the radio path; in `OTA` they reply `nok unavailable in OTA mode`. Channel-info subscribers: **128** (`WIFI_AIRPORT_MAX`).

```bash
nc -u 192.168.32.1 22
status
ping
```

| Kind | Body |
|------|------|
| Success, no payload | `ok` |
| Success with args | `ok <args>` (`status` is `ok` plus the snapshot body) |
| Failure | `nok <msg>` |
| `ping` | `pong` |
| `help` / `?` | command list |

Commands:

- `set_mode|sm <mode>` — `BFC_TUNNEL_DEVICE`, `STANDALONE`, `OTA`. To/from `OTA` persists and reboots. Other mode changes apply live (BSSID prefix) until `save`.
- `set_domain|sdom <domain>` — air domain `1`–`65535` hex (optional `0x`). Required for TX and RX.
- `unset_domain|udom` — domain `0`.
- `set_upstream_tx|sut port=<port>` — bind `0.0.0.0:<port>`; each datagram is one full 802.11 MPDU injected as-is. Replaces any previous bind.
- `unset_upstream_tx|uut` — unbind the inject socket.
- `set_upstream_rx|sur host=<ip> port=<port>` — forward every accepted air MPDU to host:port. Replaces any previous dest.
- `unset_upstream_rx|uur` — remove the forward dest.
- `set_logger|sl address=<host>:<port> level=<error|warn|info|debug>` — one UDP text logger (replaces the previous dest). Rate-limited (~50 Hz). Runtime only. Works in `OTA`.
- `unset_logger|ul` — clear the UDP logger.
- `set_allow_failed_crc|saf <allow>` — legacy; RX no longer drops on `rx_state` / `WIFI_PKT_MISC`. Promiscuous filter keeps `FCSFAIL` so inject frames reach the callback; `rx_state==0x41` is counted in `drop_crc_error` only (ESP32 often sets it falsely on good raw-inject frames — do not software-CRC).
- `set_channel|sc <channel>` — 1–14. Channel 14 is 802.11b (DSSS/CCK) only.
- `set_modulation|sd <modulation>` — TX PHY rate (see table).
- `set_network|sn <mode>` — `STATIC`: use `set_ip` immediately. `AUTO` (default): DHCP client; after **5 s** without a lease, apply `set_ip` as static (bench DORA ~40 ms + ESP DHCP ARP check ~1 s, plus boot-time margin while WiFi comes up). The radio never runs a DHCP server — the host/manager must be on a network that can reach the radio (LAN DHCP lease, or a static address on the radio’s `/24`).
- `set_ip|sfi <ip>` / `set_fallback_ip|sfi <ip>` — static / fallback address (`/24`). Used in `STATIC`, and as the AUTO fallback when no lease arrives.
- `save|sv <slot>` — write NVS slot `0`–`9` (current slot if omitted) and make it current. Blob **v7**: mode, radio, network/ethernet, domain, single `sut`/`sur` (no channel-info subscribers). Logger is runtime only. Boot / `use` clears existing upstreams and recreates them from the blob. Older blobs load with CI subscribers ignored (v6) or upstreams cleared (v3–v5).
- `use|u <slot>` — load slot `0`–`9`, apply it, make it current. Empty slot is `nok slot empty`. To/from `OTA` reboots.
- `set_wifi_tx_tune|swtt burst_size=<1-20> burst_gap_us=<0-1000000> max_in_flight=<1-32>` — runtime `wifi_tx` burst drain and outstanding TX cap (any subset of keys). Shown on `status` as `wifi_tx_tune …`.
- `set_cca_enabled|sce <is_enabled>` — TX CCA / CSMA. `0` injects without waiting for idle.
- `set_tx_power|stp <dbm>` — maximum Wi-Fi TX power `2`–`20` dBm. Default `20`. ESP32 maps this to 0.25 dBm units.
- `status|s` — snapshot (see Status).
- `ping` — `pong` (works in `OTA`).
- `ether_test_tx|ett size=<1-1400>` — write patterned test buffer; reply `ok sn=<sn> data=<binary>` (binary to end of datagram). Pattern: `data[i]=(sn+i)&0xff`. Works in `OTA`.
- `ether_test_rx|etr sn=<n> data=<binary>` — read/verify patterned test buffer; reply `ok last_sn=<sn> gap=<n>` (`gap` = missed seqs since previous ok). Works in `OTA`.
- `ether_bench_tx|ebt to=<host>:<port> size=<8-1472> count=<n>` — fire-and-forget UDP flood on port **2223** data path (not console RTT). `count=0` runs until `ether_bench_stop`. Works in `OTA`.
- `ether_bench_rx|ebr` — arm RX counter on **2223** (header check only). Works in `OTA`.
- `ether_bench_stop|ebs` / `ether_bench_status|ebst` — stop flood / read counters.
- `reset|r` — `esp_restart`.
- `help|?` — command list.

`sut` = host transmits onto the air (UDP full MPDU → radio inject). `sur` = host receives from the air (radio → UDP full MPDU). Bus/slot mapping is done by the host inside the MPDU.

| Modulation Code | Modulation | Data rate |
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
| OFDM_MCS0_LGI | BPSK HT20 | 6.5 Mbps |
| OFDM_MCS1_LGI | QPSK HT20 | 13.0 Mbps |
| OFDM_MCS2_LGI | QPSK HT20 | 19.5 Mbps |
| OFDM_MCS3_LGI | 16-QAM HT20 | 26.0 Mbps |
| OFDM_MCS4_LGI | 16-QAM HT20 | 39.0 Mbps |
| OFDM_MCS5_LGI | 64-QAM HT20 | 52.0 Mbps |
| OFDM_MCS6_LGI | 64-QAM HT20 | 58.5 Mbps |
| OFDM_MCS7_LGI | 64-QAM HT20 | 65.0 Mbps |
| OFDM_MCS0_SGI | BPSK HT20 | 7.2 Mbps |
| OFDM_MCS1_SGI | QPSK HT20 | 14.4 Mbps |
| OFDM_MCS2_SGI | QPSK HT20 | 21.7 Mbps |
| OFDM_MCS3_SGI | 16-QAM HT20 | 28.9 Mbps |
| OFDM_MCS4_SGI | 16-QAM HT20 | 43.3 Mbps |
| OFDM_MCS5_SGI | 64-QAM HT20 | 57.8 Mbps |
| OFDM_MCS6_SGI | 64-QAM HT20 | 65.0 Mbps |
| OFDM_MCS7_SGI | 64-QAM HT20 | 72.2 Mbps |

Defaults: channel `1`, modulation `DSS_1M_L`, CCA enabled, TX power `20` dBm, `allow_failed_crc` off, network `AUTO`, static/fallback `192.168.32.1`, domain unset, no upstreams, logger unset. Boot loads the last-used NVS slot (default `0`); an empty slot `0` keeps these defaults. TX packet pool / `wifi_tx` queue depth **20**; RX pool / **`wifi_rx` queue** depth **8**. Inject pacing: manager burst settings and `wifi_tx` in-firmware limits (see [cd-protocol.md](cd-protocol.md)).

# Domain and bus

Domain is required on the radio for RX accept (Addr3 filter). The host stamps the same domain into every TX MPDU. Persisted with `save` / `use`.

Buses live in Addr1/Addr2 slots of the host-built MPDU. The radio does not demux by bus.

# Operating Modes

Both radio modes use the same Addr1/Addr2 packing. Addr3 prefix selects the mode.

## `BFC_TUNNEL_DEVICE` (default)

Multicast radio for bfc-tunnel. Addr3 prefix `BA:DD:CA:FE`.

## `STANDALONE`

Addr3 prefix `CA:FE:BA:BE`. Example — two radios, host stamps bus `b2` A→B and `a1` B→A, domain `0x1234`:

```
# A
set_mode STANDALONE
set_domain 1234
set_upstream_tx port=9000
set_upstream_rx host=192.168.32.10 port=9001

# B
set_mode STANDALONE
set_domain 1234
set_upstream_tx port=9000
set_upstream_rx host=192.168.32.11 port=9001
```

## `OTA`

Ethernet, UDP console, and HTTP OTA. Radio, LC, and endpoints stay down. Radio console commands reply `nok unavailable in OTA mode`. Logger commands still run. `set_mode` to/from `OTA` persists and reboots.

# UDP logger

Runtime diagnostic text to one host. Helper: `python3 tools/udp_log.py --port 9999`.

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

Line shape: `<us_timestamp> <level> <message>\n`. `set_logger` replaces the previous dest/level.

# Status

UDP console `status|s` prints a sectioned snapshot (`#` headers) in one reply datagram. First line is `ok`; fields are space-separated `key=value` tokens.

```
ok
# device
device uptime=<s>s reset_reason=<name> heap_usage=<bytes> heap_free=<bytes> mode=<mode> domain=<hex|unset>
# network
network ip=<ipv4/24|-> mode=<dhcp|static> console_port=22
ota url=http://<ip>:80/update
  | ota waiting
# wifi
wifi_txrx channel=<1-14>
wifi_tx modulation=<code> cca=<enabled|disabled> tx_power=<2-20>
wifi_rx radio rssi=<dBm|-> snr=<dB|-> allow_failed_crc=<true|false>
# upstreams
upstream_tx port=<udp_port>
upstream_rx host=<ip> port=<port>
# channel
channel_tx queue=<n> drop_nomem=<n> retry_count=<n> retry_nomem=<n> retry_other=<n>
           inject_ok=<n> inject_fail=<n> in_flight=<n> tx_latency=<uus|-> inject_wait=<uus|-> udp_tx=<n>
channel_rx queue=<n> drop_no_pkt_pool=<n> drop_queue_full=<n> drop_crc_error=<n> wifi_accept=<n> udp_fwd=<n>  (older FW: `udp_accept` — same counter)
channels_tx drop_no_pkt_pool=<n> drop_queue_full=<n>
channels_rx drop_send_fail=<n>
# logger
logger address=<host>:<port> level=<error|warn|info|debug> emitted=<n> dropped=<n>
```

`# upstreams` / `# logger` are omitted when unset (no binds / no dest). In `OTA`, after network/ota lines status prints `# radio` / `radio disabled (OTA mode)` and returns.

| Line | Meaning |
|------|---------|
| `device` | Seconds since boot; reset cause (`power-on`, `external`, `software`, `panic`, `interrupt-wdt`, `task-wdt`, `wdt`, `brownout`, `unknown`); `MALLOC_CAP_8BIT` used/free; mode; domain hex or `unset` |
| `network` | Current IPv4 `/24` (or `-`); `dhcp` (`AUTO`) or `static`; UDP console port |
| `ota` | Upload URL once Ethernet has an IP; otherwise `ota waiting` |
| `wifi_txrx` | RF channel |
| `wifi_tx` | Modulation, CCA, TX power |
| `wifi_rx` | Last accepted air RSSI/SNR (`-` until a frame is accepted); CRC policy |
| `channel_tx` | `wifi_tx` queue depth; inject abandon (`drop_nomem`); total / `NO_MEM` (retries until success) / other (`WIFI_RADIO_INJECT_RETRIES`) `esp_wifi_80211_tx` retries; inject ok/fail; pending TX-done; mean submit→TX-done; mean wall time in `inject_retry` |
| `channel_rx` | RX queue depth; pool / queue / CRC drops |
| `channels_tx` / `channels_rx` | Inject / forward drop counters |

Example (tunnel, one inject and one forward):

```
ok
# device
device uptime=123s reset_reason=power-on heap_usage=45678 heap_free=12345 mode=BFC_TUNNEL_DEVICE domain=1234
# network
 network ip=192.168.253.14/24 mode=dhcp console_port=22
ota url=http://192.168.253.14:80/update
# wifi
wifi_txrx channel=n
wifi_tx modulation=DSS_1M_L cca=enabled tx_power=20
wifi_rx radio rssi=-42 snr=18 allow_failed_crc=false
# upstreams
upstream_tx port=9000
upstream_rx host=192.168.32.10 port=9001
# channel
channel_tx queue=0 drop_nomem=0 retry_count=0 retry_nomem=0 retry_other=0 inject_ok=0 inject_fail=0 in_flight=0 tx_latency=312us inject_wait=-
channel_rx queue=0 drop_no_pkt_pool=0 drop_queue_full=0 drop_crc_error=0
channels_tx drop_no_pkt_pool=0 drop_queue_full=0
channels_rx drop_send_fail=0
```

# Board / indicators

LAN8720: PHY addr 1, MDC 23, MDIO 18, power 16. RMII REF_CLK is a PlatformIO env choice: `wt32-eth01` = GPIO0 in (stock WT32 oscillator), `lan-module` = GPIO17 out. Activity LEDs (active-low, 1 ms stretch): **IO5** = WiFi RX (silk RXD), **IO17** = WiFi TX (silk TXD). TX LED is disabled when RMII clock uses GPIO17. Hostname `winject-esp32`.

# Ethernet addressing

The radio starts in the configured mode (`BFC_TUNNEL_DEVICE`, `STANDALONE`, or `OTA`). OTA and the UDP console listen once Ethernet has an IPv4 address.

`AUTO` (default) runs a DHCP client. After **5 seconds** without a lease (quiet path ≈ 1.05 s: ~40 ms DORA + ~1 s ESP ARP check; timer keeps margin for WiFi bring-up overlap), Ethernet applies `set_ip` as a static address.

`STATIC` uses `set_ip` immediately. The radio never runs a DHCP server; configure the host/manager NIC (DHCP from the LAN, or a static address on the radio’s `/24`) so it can reach the radio.

- Default / fallback address `192.168.32.1/24`
- UDP console `22` and OTA on the lease or the static address

The STA MAC is the chip-unique factory MAC (eFuse). Ethernet uses the derived `ESP_MAC_ETH` address. One firmware image can be flashed to every radio.

# OTA

Once Ethernet has an IPv4 address, HTTP OTA is on port 80:

- `GET /` upload form
- `POST /update` firmware blob (multipart or raw)

```bash
curl -F "firmware=@.pio/build/wt32-eth01/firmware.bin" http://192.168.32.1/update
# or: .pio/build/lan-module/firmware.bin
```

Use the DHCP-assigned address when a lease arrived before the 5 s fallback. `set_mode OTA` boots network+console+HTTP only; leave with `set_mode BFC_TUNNEL_DEVICE` or `set_mode STANDALONE` (persists and reboots).

# ETH→WiFi TX performance

## Symptom

Host→Ethernet→`sut`→air inject used to stall in the **~8 Mbps** class for 1400 B MPDUs once WiFi TX was busy, even when the PHY (e.g. MCS4/MCS7) and `wifi_bench` (on-device, no ETH) could go much higher. Smooth paced offers near 30 Mbps later plateaued around **~18.7 Mbps** on the radio (`inject_ok` goodput); flood could do better only after EMAC timeshare.

## Root cause

ESP32 **EMAC RX and WiFi TX share DMA / bus time**. Continuous `esp_wifi_80211_tx` kept WiFi DMA busy, so EMAC dropped host frames **before** staging/`udp_tx`. Confirmed by isolation:

| Path | Result |
|------|--------|
| `wifi_bench` → `wifi_tx` (no ETH) | Hits offer (8…~30 Mbps depending on mod/size) |
| ETH → `sut` with null/dry sink | Clean at 30 Mbps |
| ETH → `sut` → real WiFi inject | Cap / loss on the ETH leg while air TX runs |

Soft flow-control on the manager does not fix this — the miss is on-radio between EMAC and WiFi.

## Mitigation (current firmware)

ETH inject goes **directly** into `wifi_tx` (no L2 staging task). The **`wifi_tx`** task drains the queue in **bursts** with a short **gap** between bursts so EMAC RX can win DMA time. **`upstream_tx`** drops when the queue is full (`channels_tx drop_queue_full`).

| Knob | Where | Role |
|------|--------|------|
| `WIFI_TX_BURST_SIZE_DEFAULT`, `WIFI_TX_BURST_GAP_US` | `config.h` / `wifi_tx` | Burst drain + idle gap between bursts |
| `WIFI_RADIO_MAX_IN_FLIGHT` (4) | `wifi_tx` | Cap outstanding 802.11 TX |
| `WIFI_RADIO_TX_QUEUE` (20) | `wifi_tx` | Driver queue depth |
| `emac_rx` prio = `WIFI_RADIO_TASK_PRIO+2` | `ethernet_rmii.cpp` | Prefer EMAC RX when both contend |
| WiFi dynamic TX buffers **16**; ETH DMA **1514 B** × RX **28** / TX **16** | `sdkconfig.defaults` / `lan-module` | One buffer per standard 1500 MTU frame |

Older **`lc_tx`** batch/gap/staging tuning (`set_inject_tune`, `LC_TX_*`) was removed in the upstream refactor; see [flow_refactor.md](flow_refactor.md).

## Verified results (bench, 1400 B, CCA off)

| Setup | Metric | Value |
|-------|--------|-------|
| MCS7 flood ETH→wifi (`.9` / `.14` `lan-module`) | radio `inject_ok` goodput | **≥30 Mbps** (≈32 Mbps / 30 s) |
| MCS7 paced @ 30 Mbps offer (pre-gap plateau) | radio inject | ~18.7 Mbps |
| MCS4_LGI managed UDP offer 25 Mbps | manager `STREAM TOTAL TX` | **≈25.05 Mbps** (`LOST=0`) |
| Same MCS4 run, USB `mon0` **active TX window only** | air body goodput | **≈15.9–16.3 Mbps** (peak 2 s slice ≈16.3) |

So **effective on-air** for MCS4@25M offer is about **16 Mbps** as heard by the USB sniffer; the radio/manager correctly report the full paced TX into the radio.

## Measuring air rate on `mon0`

`scripts/usb_wifi_logger.py` prints cumulative `air=` from sniff start. That **dilutes** after the stream stops (e.g. 40 s average looked like ~4 Mbps). Recalculate over the **actual transmit window**:

1. Find first/last stats samples where `bssid` / body bytes still increase.
2. `Mbps = 8 * Δbody_bytes / Δt / 1e6` (or use 2 s slice deltas).

On the MCS4@25M run, slices during TX were steady **15.4–16.3 Mbps** with **no mid-burst idle gaps**. Capture was a continuous ~36% undersample vs manager TX (~1425 pkt/s heard vs ~2232 offered): strong RSSI (~−11 dBm), `fcs_fail=0`. Treat USB monitor as a lower bound, not proof the radio only sent 16 Mbps.

Do not equate:

- **bw_test `A->B` send column / manager `STREAM TOTAL TX`** — host→manager→radio offer (can be 25 Mbps with 100% peer loss).
- **radio `inject_ok` goodput** — frames accepted by `esp_wifi_80211_tx`.
- **mon0 TX-window `air`** — frames the USB sniffer kept; often ~60–65% of manager TX at MCS4 density.

# Air RX and FCS (promiscuous)

## Peer / A→B tests: prefer `OFDM_24M` for margin

ESP32 promiscuous forward is reliable for **legacy OFDM** inject (11g-style data MPDUs). **HT MCS** (e.g. `OFDM_MCS4_LGI`) can TX and show on USB `mon0`, but the same chip rarely delivers our stamped Addr3 MPDU in the promisc callback (`ht_prefix` ≪ `inject_ok`). Until that changes, treat **MCS as TX / sniffer bench only**.

At the **same offered kbps**, `OFDM_48M` / `OFDM_54M` still deliver frames in **tighter bursts** (shorter on-air time), which can overrun the promiscuous callback unless the RX pool/queue and hot-path work are sized for it (`WIFI_RADIO_RX_QUEUE`, defer `format_phy`/LED off the Wi-Fi task). Use **`OFDM_24M` on both radios** when you want maximum peer-RX margin (`lat_udp_*.cfg` / `bw_*.cfg` default to it; `manager_bw_test.sh` sets it when `--modulation` is omitted).

**Legacy 64-QAM (`OFDM_48M` / `OFDM_54M`) TX power:** raw inject at **20 dBm** often clips 64-QAM; a USB `mon0` capture still looks healthy while the peer ESP32 promisc path sees `leg_ok` ≈ 0. Firmware caps effective TX power to **`WIFI_TX_POWER_64QAM_LEGACY_MAX_DBM` (13)** for those modulations (user `set_tx_power` may be higher; the driver limit is reduced). STA protocol is **11b|11g|11n** on channels 1–13 and is re-applied after `esp_wifi_config_80211_tx_rate()` and promisc enable.

Use `reset_promisc_stats` / `rps` and the `promisc …` line in `status` to compare `inject_ok` vs `leg_prefix` / `domain_word` / `wifi_accept`.

## Symptom

Managed and direct tests showed **100% A→B loss** while `.9` TX and USB `mon0` looked healthy. Radio B had `wifi_accept` ≈ `drop_crc_error` and `udp_fwd=0` with default `allow_failed_crc=0`.

## Can we detect CRC fail without extra CPU?

**Only from hardware** — `wifi_pkt_rx_ctrl_t.rx_state` (`0` = OK, `0x41` = FCS fail when `WIFI_PROMIS_FILTER_MASK_FCSFAIL` is on). There is **no** on-frame FCS in the promiscuous payload (length uses `sig_len` minus 4). **Do not** software-CRC 1400 B MPDUs in the hot path.

On raw `esp_wifi_80211_tx` inject, ESP32 often tags **good** peer frames as `rx_state==0x41` and/or `WIFI_PKT_MISC`. The old gate `(type==MISC || rx_state!=0)` dropped **every** accepted frame. USB monitor still showed clean FCS.

Turning **off** `FCSFAIL` in the promiscuous filter also fails on this chip: Addr3-matched inject frames **never** reach the callback (`wifi_accept` stays 0).

## Fix (`wifi_rx.cpp`)

1. Keep promiscuous filter **DATA | DATA_MPDU | MISC | FCSFAIL** (required for delivery).
2. After Addr3 match, **always enqueue** for forward; do not return early on MISC or `rx_state`.
3. Increment `drop_crc_error` only when `rx_state==0x41` (telemetry; not a drop gate).

## Manager `bw_test` rate ceiling

UDP manager tests use `configuration/winject-tests/lat_udp_*.cfg`. Set
`winject.max_rate_kbps` **≥ 20000** for OFDM_24M peer tests; **10000** caps the
scheduler below 15 Mbps payload goodput even with a clean link. `manager_bw_test.sh`
defaults channel **1** and `--no-cca` for saturated runs.

## Verified (`.9` → `.14`, 1400 B, ch1, `allow_failed_crc=0`)

| Modulation | `inject_ok` | `udp_fwd` / host recv |
|------------|-------------|------------------------|
| `OFDM_24M` | 100/100 | **~89/100** @ 2 Mbps offer |
| `OFDM_MCS4_LGI` / `OFDM_MCS7_LGI` | ~100 | **0** (HT promisc — do not use for peer RX tests; see above) |
