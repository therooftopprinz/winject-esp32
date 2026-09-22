# Bench asymmetry (A→B vs B→A)

Findings from manager UDP bandwidth tests on the **lan-module** pair **`.9` / `.14`**
(OFDM_24M, channel 1, no-CCA, `prepare_radios_for_manager`). Host **`.106`**.
Configs: `lat_udp_*.cfg` (manager `skip_console=1`, burst pacing defaults).

Related: [tests_wt32_eth01.md](tests_wt32_eth01.md), [cd-protocol.md](cd-protocol.md),
[winject.md — Air RX](winject.md#air-rx-and-fcs-promiscuous).

## Summary

| Leg | Inject → recv | Typical goodput | Typical host loss | vs 15 Mbps gate |
|-----|----------------|-----------------|-------------------|-----------------|
| **A→B** | `.9` → `.14` | ~10.7–11.8 Mbps | ~30–35% | **Below** |
| **B→A** | `.14` → `.9` | ~13.4–14.0 Mbps | ~15–18% | **Near** |

**B→A is not “the reverse path is magically better” in the abstract.** Swapping
`--a` / `--b` on the same script shows goodput tracks **which physical radio
injects** and **which listens**:

| Script label (after IP swap) | Actual path | Rough result |
|------------------------------|-------------|--------------|
| A→B with `--a .14 --b .9` | `.14` → `.9` | ~14 Mbps, ~15% loss |
| B→A with `--a .14 --b .9` | `.9` → `.14` | ~12 Mbps, ~25% loss |

Worst case is **`.9` injecting toward `.14` listening** (default A→B).

## How to reproduce

```bash
./scripts/manager_bw_test.sh --a 192.168.253.9 --b 192.168.253.14 --no-cca --test-ab --test-ba

# Inject timing + recv promisc deltas (both legs, summary table)
python3 tools/dir_timing_compare.py --a 192.168.253.9 --b 192.168.253.14

# Confirm per-radio behavior (not manager slot names)
python3 tools/dir_timing_compare.py --a 192.168.253.14 --b 192.168.253.9
```

Single-direction promisc on the listener (default: `.14` during A→B):

```bash
python3 tools/promisc_ab_delta.py --a 192.168.253.9 --b 192.168.253.14
```

## Timing (inject side)

`status` **after** each leg (`channel_tx`):

| Metric | Meaning | `.9` inject (A→B) | `.14` inject (B→A) |
|--------|---------|-------------------|---------------------|
| **`tx_latency`** | Wi‑Fi TX path (enqueue → done) | ~580–600 µs | ~600 µs |
| **`inject_wait`** | Stall/retry before TX accepts | **~200–535 µs** | **~150–160 µs** |
| **`pool_free_min` / `queue_hwm`** | TX pool / driver queue stress | Tighter on `.9` under load | Looser on `.14` |

**PHY TX time is similar.** Asymmetry shows up in **`inject_wait`** and TX
pool/queue pressure on **`.9`** (EMAC hijack → `wifi_tx` queue, shared EMAC/WiFi DMA).

## Receive side (promisc on listener)

One A→B run with promisc reset on **`.14`** (representative):

- Host **~35% loss** (~4.8k recv / 7.3k sent).
- Promisc **`leg_ok` Δ ~5.4k** — domain-matched frames seen on air.
- **`drop_addr3` Δ ~0.9–1.0k** (~13–15% of promisc `data`) — on-air frames
  rejected by Addr3 / `accept_mpdu` before the normal UDP forward path.
- **`drop_crc_error`** on `channel_rx` — **0** in leg deltas (not the dominant
  story in these runs).
- **`leg_ok` − host recv** ~**500–600** — gap after promisc accept (forward /
  manager path).

B→A with **`.9` listening**: lower host loss (~15–18%), **`drop_addr3` ~5%** of
promisc `data` in `dir_timing_compare` runs.

## Loss budget (default A→B, qualitative)

```text
Host offer (manager A → .9 EMAC → air)
    → .14 promisc (drop_addr3, drop_len, …)
    → .14 UDP RX / forward → manager B → host
```

Bottlenecks combine:

1. **`.9` inject** — higher `inject_wait`, shallower TX pool headroom.
2. **`.14` RX filter** — more `drop_addr3` when `.14` is the peer.
3. **Smaller post-promisc gap** — not all `leg_ok` frames appear at the host.

## Ruled out or secondary (this bench)

- **Manager A vs B upstream layout alone** — IP swap isolates **radio** identity.
- **`eth_dma_burst` on `.9`** — restore **32** after dma sweeps; both radios at
  32 in recent status.

## Inject pacing vs old `tx_grant`

See [inject-pacing-investigation.md](inject-pacing-investigation.md). Summary:
**`tx_grant` capped goodput (~1–5 Mbps)**; passive inject was **~10–11 Mbps**.
After burst pacing without radio feedback, recent ch13 runs showed **~7–8 Mbps**
and **~21% mgr→radio UDP loss** in drop stages — EMAC/WiFi contention, not
missing grant RTT alone.

## Tuning directions (current knobs)

| Target | Knobs / signals |
|--------|------------------|
| **Manager offer** | `winject.max_rate_kbps`, `winject.max_data_per_tick`, `winject.tx_burst_size`, `winject.tx_burst_interval_us` |
| **`.9` inject** | `channels_tx drop_queue_full`, `inject_wait`, `pool_free_min`, `queue_hwm` via `dir_timing_compare`; build-time `WIFI_RADIO_MAX_IN_FLIGHT`, `WIFI_TX_BURST_*` |
| **`.14` listen** | `drop_addr3` / promisc under load; air RX docs in [winject.md](winject.md) |
| **Gate** | A→B ≥15 Mbps with `manager_bw_test.sh --test-ab` — **not met** on 2026-09-21 (~10 Mbps / ~12 Mbps at ~16.5 Mbps offer; loss ~44% / ~31%) |

Historical sweeps that used console **`set_inject_tune`** (`flush_batch`, `emac_gap_ticks`)
and **`ci_pace_inject` / `tx_grant`** are obsolete; see git history and archived tables
in pre-2026-09 refactor commits. Replacement tools: tune manager burst settings in
`lat_udp_*.cfg` rather than `tools/inject_pacing_matrix.py`.

## Example `dir_timing_compare` snapshot

| leg | kbps | loss% | inject_wait (µs) | leg_ok Δ | drop_addr3 Δ |
|-----|------|-------|------------------|----------|--------------|
| A→B | 10752 | 34.9 | 535 (.9) | 5384 | 984 |
| B→A | 13483 | 18.4 | 157 (.14) | 6026 | 367 |

Numbers vary run-to-run; trends (inject_wait, addr3%, loss) are stable on this
pair.
