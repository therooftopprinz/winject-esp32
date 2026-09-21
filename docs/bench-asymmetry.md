# Bench asymmetry (A→B vs B→A)

Findings from manager UDP bandwidth tests on the **lan-module** pair **`.9` / `.14`**
(OFDM_24M, channel 1, no-CCA, `prepare_radios_for_manager` inject tune). Host
**`.106`**. Default configs: `lat_udp_*.cfg` with **`winject.ci_pace_inject = 0`**
(grant pacing off for the gate bench).

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
| **`inject_wait`** | Stall/retry/yield before TX accepts | **~200–535 µs** | **~150–160 µs** |
| **`pool_free_min` / `queue_hwm`** | TX pool / driver queue stress | Tighter on `.9` under load | Looser on `.14` |

**PHY TX time is similar.** Asymmetry shows up in **`inject_wait`** and TX
pool/queue pressure on **`.9`** (EMAC → L2 staging → `wifi_tx`, including
`may_feed_from_lc_tx()` / `lc_tx_yield`). Both radios get the same
`set_inject_tune` from prepare (`flush_batch=8`, `emac_gap_ticks=0`,
`max_in_flight=6`, `staging_margin=4`).

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
- **`tx_grant` on default gate** — `skip_console=1` and `ci_pace_inject=0` in
  `lat_udp_*.cfg`; open inject. Passive vs explicit grant goodput:
  [cd-protocol.md — Passive pacing vs explicit grant](cd-protocol.md#passive-pacing-vs-explicit-grant).
- **`eth_dma_burst` on `.9`** — restore **32** after dma sweeps; both radios at
  32 in recent status.

## Tuning directions

| Target | Knobs / signals |
|--------|------------------|
| **`.9` inject** | `set_inject_tune` on `.9` only (`max_in_flight`, `emac_gap_ticks`, `flush_batch`); watch `inject_wait`, `pool_free_min`, `queue_hwm` via `dir_timing_compare` |
| **`.14` listen** | `drop_addr3` / promisc under load; air RX docs in [winject.md](winject.md) |
| **Gate** | A→B ≥15 Mbps with `manager_bw_test.sh --test-ab` |

## Inject pacing matrix (`.9` → `.14`, 2026-09-20)

Sweep: manager **`winject.max_data_per_tick`** (1 vs 4) × radio **`.9`**
`set_inject_tune` **`flush_batch`** (1 vs 8) × **`emac_gap_ticks`** (0 vs 1).
Passive pacing (`ci_pace_inject=0`). Tool: `tools/inject_pacing_matrix.py`.

| mgr/tick | flush | emac_gap | A→B kbps | loss % |
|---------:|------:|---------:|---------:|-------:|
| 4 | 8 | 0 | 10813 | 34.5 |
| 1 | 8 | 0 | 9513 | 42.4 |
| **4** | **1** | **0** | **10956** | **33.7** |
| 4 | 1 | 1 | 9610 | 41.8 |
| 1 | 1 | 0 | 9990 | 39.5 |
| 1 | 1 | 1 | 9415 | 43.0 |
| 1 | 8 | 1 | 9289 | 43.8 |
| 4 | 8 | 1 | 10544 | 36.2 |

**Best cell:** `max_data_per_tick=4`, `flush_batch=1`, `emac_gap_ticks=0` (~**+0.4
Mbps** vs default prepare `flush_batch=8` in the same session). Still **below**
the 15 Mbps gate. Lowering manager ticks per scheduler pass or adding EMAC gap
ticks consistently regresses goodput.

`prepare_radios_for_manager` always resets inject tune to `flush_batch=8`. To
match the best cell on the official gate script:

```bash
./scripts/manager_bw_test.sh --a 192.168.253.9 --b 192.168.253.14 --no-cca --test-ab \
  --inject-tune-a 'flush_batch=1 emac_gap_ticks=0 max_in_flight=6 staging_margin=4'
```

## Example `dir_timing_compare` snapshot

| leg | kbps | loss% | inject_wait (µs) | leg_ok Δ | drop_addr3 Δ |
|-----|------|-------|------------------|----------|--------------|
| A→B | 10752 | 34.9 | 535 (.9) | 5384 | 984 |
| B→A | 13483 | 18.4 | 157 (.14) | 6026 | 367 |

Numbers vary run-to-run; trends (inject_wait, addr3%, loss) are stable on this
pair.
