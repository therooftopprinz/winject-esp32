# Control / data (C/D) for manager ↔ radio inject

Explicit **console grant** for inject batches. CI remains telemetry only (RSSI, queue hints).

## Planes

| Plane | Path | Purpose |
|-------|------|---------|
| **TX_DATA** | Manager → radio inject UDP (`set_upstream_tx` / `:9000`) | MPDU payloads |
| **RX_DATA** | Radio → manager forward UDP | Air RX → host |
| **Control** | UDP **console** (`:22`) | Config + **`tx_grant`** |

## Grant cycle (cd-protocol)

```text
Manager:  tx_grant          (console)
Radio:    ok <N>             N = min(staging_free, tx_pool_free)
Manager:  send ≤ N TX_DATA   (inject UDP)
Manager:  tx_grant          (when local credits exhausted)
…
```

1. Manager **`tx_grant`** (`tg`) on a **dedicated console socket** (separate from ping).
2. Radio computes **N**, calls `issue_inject_grant(N)`, replies **`ok N`**.
3. Each accepted inject datagram consumes one grant on the radio; without grant, hijack drops (`drop_inject_grant`).
4. Manager keeps local **`tx_grant_credits`**; decrements per data MPDU sent; at **0** issues another **`tx_grant`**.

Enabled when **`winject.ci_pace_inject = 1`**. Set **0** to disable grant gate (open inject); **`lat_udp_*.cfg`** use **0** for the bench gate until grant RTT keeps up at line rate. With **`skip_console = 1`**, the manager still opens a **grant-only** UDP console socket (`connect_grant_peer`); full console program/ping is skipped. Grant replies are handled **async** on the reactor (pipelined `tx_grant`); radio **`issue_inject_grant`** **accumulates** credits.

## Radio

- `wifi_tx::compute_tx_slots()` — capacity for grant.
- `lc_tx_endpoint::issue_inject_grant` / `take_inject_grant` — enforcement after first grant in a session.
- `unset_upstream_tx` / `clear` resets grant mode.

## Manager

- `console_client::request_tx_grant()` — sync `tx_grant` on grant socket.
- `tx_scheduler::may_emit_data` — blocks data until grant credits > 0.
- `on_data_mpdu_sent` — one credit per data MPDU.

## CI (channel-info)

Unchanged: optional `set_upstream_ci` for **`gci`** / RSSI / legacy `flow=` — **not** used for inject pacing.

## Passive pacing vs explicit grant

**Passive pacing** here means **`winject.ci_pace_inject = 0`**: no `tx_grant` gate.
The manager still rate-limits (`max_rate_kbps`); the radio still backpressures via
L2 staging, `wifi_tx` / `lc_tx` yield, and `set_inject_tune` (see
[bench-asymmetry.md](bench-asymmetry.md)).

**Explicit grant** means **`ci_pace_inject = 1`**: manager credits + console
`tx_grant` + radio `take_inject_grant()`.

Representative **A→B** runs (manager UDP, OFDM_24M, ch1, no-CCA, ~16.5 Mbps
offer, same firmware generation with async grant + radio grant accumulate):

| Mode | Config | A→B goodput | A→B loss | Notes |
|------|--------|-------------|----------|--------|
| Passive | `ci_pace_inject=0` (`lat_udp_*.cfg`) | **~10.7–11.8 Mbps** | **~29–35%** | Default gate bench |
| Explicit grant | `ci_pace_inject=1`, `skip_console=1` | **~1.6–4.7 Mbps** | **~72–90%** | Grant-only UDP; pipelined `tx_grant` |
| Explicit grant | `ci_pace_inject=1`, `skip_console=0` | **~1.0 Mbps** | **~94%** | Sync grant + held console; `tx_grant timeout` |

**Conclusion:** on this bench, **passive pacing wins on goodput**; explicit
grant is for **bounded inject / protocol correctness**, not current line-rate
throughput. Keep **`ci_pace_inject=0`** for `manager_bw_test` until grant RTT
and credit pipelining can sustain the offered rate.

CI-driven **`tx_slots`** inject pacing was tried and removed (telemetry-only CI);
it is not documented as an alternative here.

### Passive pacing knobs (what exists vs common names)

There are **no** manager settings named `burst_interval` or `burst_count`, and
**no** `wifi_tx_burst_interval` / `wifi_tx_burst_count` on the radio. EMAC/WiFi
**timeshare** is implemented on **`lc_tx_endpoint`** (batch + gap), not as
separate burst timers inside `wifi_tx`.

| You might mean | Actual knob | Where | Runtime? |
|----------------|-------------|-------|----------|
| Manager offer / burst pacing | **`winject.max_rate_kbps`** | manager cfg | yes |
| Manager bytes token cap | **`burst`** in `tx_scheduler` (`2 ×` max MPDU payload) | manager C++ | compile-time |
| Manager emit per 250 µs tick | **`k_max_data_per_tick` (=4)** | `tx_scheduler.cpp` | compile-time |
| Manager stream pull depth | **`upstream-N.scheduler_budget`** | manager cfg | yes |
| EMAC/WiFi **batch size** | **`flush_batch`** (`set_inject_tune`, default 8; `LC_TX_FLUSH_BATCH` in `config.h`) | radio `lc_tx` | yes |
| EMAC **idle gap** after batch | **`emac_gap_ticks`** (`set_inject_tune`, ms ticks; prepare uses **0**; `LC_TX_EMAC_GAP_TICKS` default 0) | radio `lc_tx` | yes |
| Staging backpressure | **`staging_margin`**, depth **16** (`LC_TX_STAGING_DEPTH`) | radio `lc_tx` | margin yes, depth build |
| WiFi outstanding TX | **`max_in_flight`** (`set_inject_tune`; default cap **4** `WIFI_RADIO_MAX_IN_FLIGHT`) | radio `wifi_tx` | yes |
| WiFi driver queue depth | **`WIFI_RADIO_TX_QUEUE` (=20)** | `config.h` | **sdk/build only** |
| WiFi RX queue | **`WIFI_RADIO_RX_QUEUE` (=8)** | `config.h` | build only |
| EMAC RX **descriptor count** | **`CONFIG_ETH_DMA_RX_BUFFER_NUM` (=28)** | `sdkconfig.defaults` | build only |
| EMAC TX descriptor count | **`CONFIG_ETH_DMA_TX_BUFFER_NUM` (=16)** | `sdkconfig.defaults` | build only |
| EMAC programmed DMA **burst length** (not buffer count) | **`set_eth_dma_burst_len`** / `eth_dma_burst` in `status` (NVS, **reboot**) | `ethernet_rmii` | yes (reboot) |
| WiFi buffer rings | **`CONFIG_ESP_WIFI_*_BUFFER_NUM`** | `sdkconfig.defaults` | build only |

**Signals to watch** when tuning passive inject (especially `.9`):
`inject_wait`, `pool_free_min`, `queue_hwm` on inject; `drop_addr3` / `leg_ok` on
the listener — see [bench-asymmetry.md](bench-asymmetry.md) and
[winject.md — ETH→WiFi TX performance](winject.md#ethwifi-tx-performance).

**Not passive pacing:** `winject.ci_pace_inject=1` and console **`tx_grant`**
(explicit grant); CI **`flow=`** is telemetry only.
