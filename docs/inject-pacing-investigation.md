# Inject pacing investigation (post–`tx_grant`)

## What `tx_grant` actually did

Explicit **`tx_grant`** (`ci_pace_inject=1`) was **credit-based back-pressure**: the
manager could not inject another DATA MPDU until the radio returned credits on the
console. That **bounded** offer to `wifi_tx` queue headroom.

Bench result (historical, same pair): **~1–5 Mbps** goodput — much **lower** than
passive inject (`ci_pace_inject=0`, **~10–11 Mbps** A→B). Grant helped
correctness under overload; it did **not** maximize throughput.

## What replaced it

| Layer | Mechanism |
|-------|-----------|
| Manager | Token bucket (`max_rate_kbps`), `max_data_per_tick`, optional burst gate |
| Radio | `upstream_tx` → `wifi_tx` queue; drop when full |
| Radio | `wifi_tx` burst drain + gap (EMAC/WiFi timeshare) |

There is **no** closed-loop signal from radio queue depth to the manager when
`skip_console=1` (no CI, no grant).

## Offer scaling (constant loss %, higher goodput)

When the host **paces** payload with `--kbps N`, `bw_test` sends about
`N × duration` bits in the 5 s window (~`N/1.2` MPDUs/s for 1400 B payloads).
If each stage keeps a **fixed fraction** of packets, **goodput in Mbps tracks
offer** until something saturates.

| Offer (kbps) | A→B sent | A→B recv | Delivery % | A→B goodput | Host loss % | `mgr_wifi→radio_udp` drop |
|-------------:|---------:|---------:|-----------:|------------:|------------:|--------------------------:|
| 15000 | 6697 | 3281 | 49.0% | ~7.7 Mbps | ~55% | ~21% |
| 20000 | 8929 | 4301 | 48.2% | ~10.7 Mbps | ~52% | ~22% |

So the 20 Mbps run did **not** magically improve efficiency — it pushed **more**
packets through the same ~48% end-to-end funnel. Loss **rate** stayed ~50%;
**absolute** recv and goodput rose with offer.

Rough multiplicative funnel (A→B @ 20 Mbps offer):

1. Host → manager app: **0%** drop (scheduler keeps up).
2. Manager air TX → radio `udp_tx`: **~22%** drop (`8929 → 6935`) — Ethernet
   before inject accept; not shown in `channels_tx drop_queue_full`.
3. Radio inject → peer promisc accept: **~15%** (`6935 → 5920`).
4. Return path manager → host: **~10%** of forwarded (`4768 → 4301`).

`0.78 × 0.85 × 0.90 ≈ 0.48` matches recv/sent.

**Next step:** sweep offer until goodput flattens:

```bash
python3 tools/offer_goodput_sweep.py --a radio-em0 --b radio-em1 \\
  --channel 13 --test-ab --offers 10000,15000,20000,25000,30000,35000
```

Plateau usually means manager `max_rate_kbps`, PHY airtime, or EMAC/WiFi
contention — not “wrong kbps knob” alone.

## Recent bench (`.9` → `.14`, ch13, 15 Mbps offer, 2026-09-21)

From `manager_bw_test.sh` + `--drop-stages`:

| Leg | Goodput | Host loss |
|-----|---------|-----------|
| A→B | ~7.7 Mbps | ~55% |
| B→A | ~8.5 Mbps | ~50% |

Older passive gate (same pair, ch1): **~10–11 Mbps**, **~30–35%** loss
([bench-asymmetry.md](bench-asymmetry.md)).

### Drop stages (A→B, one run)

```
mgr_wifi→radio_udp       6697 →  5296   drop  1401   (~21%)
radio_udp→inject         5296 →  5296   drop     0
inject→on_air            5296 →  5296   drop     0
```

Interpretation:

1. **Largest gap** is manager counted air TX vs radio **`udp_tx`** — host UDP
   reached the radio’s Ethernet path but **~21% never became inject datagrams**.
   Frames dropped in EMAC/DMA before L2 hijack do **not** increment
   `channels_tx drop_queue_full` (only post-hijack queue full does).

2. **`tx_grant`** avoided this by not sending until the radio granted credits;
   the new path can **burst UDP** at the scheduler rate while WiFi DMA is busy,
   which matches the old “ETH starved before staging” failure mode
   ([winject.md — ETH→WiFi](winject.md#ethwifi-tx-performance)).

3. Manager **`tx_burst_interval_us=2000`** (initial default) added an extra
   **open-loop** pause unrelated to radio state — not a grant replacement.

## 10 Mbps offer profile (bench `.9` → `.14`, inject FW, 2026-09-21)

Swept `set_inject_tune` × `winject.max_data_per_tick` at `--kbps 10000` via
`tools/tune_10mbps_loss.py` and a finer matrix. Baseline (factory inject tune):
**~47% host loss**, **~6.1 Mbps** goodput (ch13).

| Setting | ch1 loss | ch13 loss | Goodput (ch1) |
|---------|---------:|----------:|--------------:|
| Default | ~47% | ~47% | ~6.1 Mbps |
| **Profile `10mbps`** | **~26%** | **~30–35%** | **~7.9 Mbps** |

**Profile `10mbps`** (applied by `prepare_radios_for_manager.py --profile 10mbps`
and `manager_bw_test.sh --profile 10mbps`):

- Radio: `flush_batch=6 emac_gap_ticks=0 max_in_flight=6 staging_margin=4`
  (upstream FW: `set_wifi_tx_tune burst_size=6 burst_gap_us=0 max_in_flight=6`)
- Manager A: `max_data_per_tick = 4` (token bucket stays `max_rate_kbps = 24000`)

### Splitting EMAC/DMA vs post-hijack loss

After flashing firmware with `channels_eth` in `status`, `--drop-stages` prints a
**mgr_wifi→radio_udp split**:

| Bucket | Counter / inference | Typical cause |
|--------|---------------------|---------------|
| **eth_pre_cb** | `mgr_radio_tx_pkt − Δeth_inject_l2` | Frame never reached `eth_input_cb` (EMAC RX / DMA vs WiFi) |
| **queue_drop** | `Δdrop_queue_full` on `channels_tx` | `wifi_tx` full while WiFi DMA busy |
| **pool_drop** | `Δdrop_no_pkt_pool` | TX packet pool exhausted at hijack |
| **len_drop** | `Δeth_inject_len_drop` | Payload outside inject size limits |
| **ok** | `Δudp_tx` | Accepted into `wifi_tx` queue |

Bench: compare **`set_inject_sink null`** (should drive `eth_inject_null_sink`, ~0
`udp_tx`) vs **`wifi`** at the same offer — if `eth_pre_cb` collapses with null
sink, air-side DMA contention was coupling into the eth leg.

Drop stages (profile, ch13, one run): `mgr_wifi→radio_udp` **~2.4%**;
`on_air→peer_accept` **~10%**; `mgr_udp→host` **~5%** — air + return path
dominate, not EMAC inject starvation.

```bash
./scripts/manager_bw_test.sh --a radio-em0 --b radio-em1 \\
  --profile 10mbps --channel 1 --test-ab --kbps 10000
```

Re-sweep after flashing **lan-module** upstream FW:

```bash
python3 tools/tune_10mbps_loss.py --channel 1 --mgr-tick 4
```

## 15 Mbps offer profile (bench `.9` → `.14`, inject FW, 2026-09-21)

At `--kbps 15000` on **ch1**, factory inject tune: **~41% host loss**, **~9.5 Mbps**
goodput. The **10 Mbps radio profile** only shaves a few points (~38% loss).

Sweep (`tools/tune_10mbps_loss.py --kbps 15000 --channel 1`):

| Setting | Host loss (typ.) | Goodput |
|---------|-----------------:|--------:|
| Default | ~41% | ~9.5 Mbps |
| `--profile 10mbps` | ~38% | ~9.8 Mbps |
| **`--profile 15mbps`** | **~35–37%** | **~10.0–10.2 Mbps** |

**Profile `15mbps`:**

- Radio: `flush_batch=8 emac_gap_ticks=1 max_in_flight=6 staging_margin=4`
  (upstream FW: `set_wifi_tx_tune burst_size=8 burst_gap_us=1000 max_in_flight=6`)
- Manager A: `max_rate_kbps = 15000`, `max_data_per_tick = 4`

```bash
./scripts/manager_bw_test.sh --a radio-em0 --b radio-em1 \\
  --profile 15mbps --channel 1 --test-ab --kbps 15000
```

```bash
python3 tools/tune_10mbps_loss.py --kbps 15000 --channel 1
```

## Tuning to try (after default fix in tree)

**Manager** (`lat_udp_*.cfg` or `set_tx_pacing`):

- `winject.tx_burst_interval_us = 0` — disable burst cooldown (keep bucket + tick cap).
- Raise `max_data_per_tick` only if CPU allows; does not fix EMAC loss alone.

**Radio** (`set_wifi_tx_tune` on inject side, e.g. `.9`):

```text
set_wifi_tx_tune burst_size=8 burst_gap_us=1000 max_in_flight=6
```

Defaults were updated toward this (8 / 1000 µs) after this investigation.

**Measure again:**

```bash
./scripts/manager_bw_test.sh --a radio-em0 --b radio-em1 --channel 13 --test-ab --kbps 15000
python3 tools/drop_path_probe.py ...   # mgr_wifi→radio_udp line
python3 tools/dir_timing_compare.py --a 192.168.253.9 --b 192.168.253.14
```

## If we need grant-like behavior again

Options (not implemented):

- **Lightweight credits** without sync console RTT (e.g. piggyback credits on
  forward UDP or a side channel).
- **Manager poll** of radio `status` (`queue_hwm`, `drop_queue_full`) when
  console is up.
- Re-enable **CI FLOW_CTRL**-style gating on the manager only (radio CI removed).

For line-rate bench, history favors **passive radio EMAC timeshare + manager rate
limit**, not console grant.

## Why Ethernet looks fine without WiFi (and why inject fails only with WiFi on)

### What the ether bench measures

`scripts/winject_eth_test.py` + console `ether_bench_*` (UDP **2223**, works in
**OTA** = WiFi stack not loaded):

| Direction | Path | Typical LAN8720 module (WiFi off) |
|-----------|------|-----------------------------------|
| **Device → host** (`ether_bench_tx`) | EMAC **TX** + lwIP send | **~99 Mbps** class (wire-limited flood) |
| **Host → device** (`ether_bench_rx`) | EMAC **RX** + dedicated drain task | **~30 Mbps** class (already below wire rate) |

Host→device must be **paced**; an unpaced `sendto` loop overflows the ESP32 UDP
mailbox and losses never show as `rx_gap` (see comment in `winject_eth_test.py`).

**Manager inject** uses a different path: host UDP to **port 9000**, **L2 hijack**
in `eth_input_cb` → `wifi_tx` (not the `ether_bench` socket). It still competes
for the same **EMAC RX** hardware as `ether_bench_rx`.

### Why WiFi turns it on

With **WiFi off** (OTA / idle), EMAC RX only serves bench or console traffic —
no continuous `esp_wifi_80211_tx`, no RF/coexistence, no `wifi_tx` DMA load on
core 0. That matches “LAN8720 works fine without WiFi.”

With **WiFi inject on**, the radio must **receive Ethernet** and **transmit
802.11** at the same time. On ESP32, EMAC RX and WiFi TX **share internal bus /
DMA time** ([winject.md — ETH→WiFi](winject.md#ethwifi-tx-performance)).
Firmware mitigations (burst+gap, `max_in_flight`, EMAC RX pinned to CPU1 with
higher prio) increase headroom but do not remove sharing.

Isolation table (same firmware):

| Test | WiFi | Result |
|------|------|--------|
| `ether_bench` TX/RX | Off | High TX / ~30 Mbps RX |
| `set_inject_sink null` + ETH flood | TX on air path idle | ETH clean to ~30 Mbps offer |
| Real inject (`sink=wifi`) | TX busy | Loss on **ETH leg** (`eth_pre_cb` / `mgr_wifi→radio_udp`) |

So the failure mode is **not** “Ethernet is broken” — it is **EMAC RX under
concurrent WiFi TX**, which inject always exercises.

### How the ~30 Mbps RX bench relates to 15–18 Mbps inject

The **~30 Mbps** host→device ether bench (WiFi off) is already an **RX-path
ceiling** on this SoC + lwIP + task setup — not 100 Mbps wire speed.

- **Inject at 15–18 Mbps** is **below** that idle RX ceiling on **WT32** (GPIO0
  + external REF_CLK): `eth_pre_cb = 0` at 18 Mbps offer (wt0↔wt1, OFDM_36M).
- **Same offer on lan-module** (GPIO17 REF_CLK out, module layout) can show
  large `eth_pre_cb` while WT32 does not — **margin and clock integrity**, not a
  different PHY part (both LAN8720).

Framing:

- **Not:** “ESP32 DMA caps inject at 15 Mbps always.”
- **Yes:** “WiFi-on inject needs reliable **EMAC RX** in a regime where idle RX
  bench is already ~30 Mbps max; modules lose that margin sooner than WT32.”

### Bench tools

```bash
# WiFi off: use OTA or STANDALONE with inject idle; ether_bench works on :2223
python3 scripts/winject_eth_test.py -d 192.168.253.14 --test both

# ETH wire vs wifi_bench on inject port 9000
python3 scripts/eth_wire_under_wifi_bench.py --a 192.168.253.9
```

Compare `channels_eth` / `--drop-stages` **`eth_pre_cb`** with `wifi_bench` idle
vs running on the same radio.
