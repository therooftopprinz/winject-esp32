# Control / data (C/D) for manager ↔ radio inject

Inject pacing is **in-firmware and in-manager** (no console `tx_grant` or
`set_inject_tune`). Firmware channel-info UDP was removed; use console **`status`**.

## Planes

| Plane | Path | Purpose |
|-------|------|---------|
| **TX_DATA** | Manager → radio inject UDP (`set_upstream_tx` / `:9000`) | MPDU payloads |
| **RX_DATA** | Radio → manager forward UDP | Air RX → host |
| **Control** | UDP **console** (`:22`) | Mode, PHY, upstream bind, status |

## Inject pacing (current)

**Manager** (`tx_scheduler`):

- Token bucket: **`winject.max_rate_kbps`**
- Per 250 µs tick cap: **`winject.max_data_per_tick`** (config)
- Burst gate on DATA MPDUs: **`winject.tx_burst_size`**, **`winject.tx_burst_interval_us`**
- Per-upstream **`scheduler_budget`** limits how much each stream pulls per tick

**Radio**:

- **`upstream_tx`**: enqueue to `wifi_tx`; drop when the TX queue is full (no grants)
- **`wifi_tx`**: queue depth **20**, **`max_in_flight`** (build default **4**), task drains in
  **bursts** with a **gap** between bursts (`WIFI_TX_BURST_*` in `config.h`)

Backpressure shows up as inject drops / queue full and manager scheduler starvation, not
console credits.

## Console metrics

Use **`status`** (and related lines) for `inject_ok`, `wifi_tx` queue HWM, enqueue ok/fail,
etc. There is no runtime console command for the old `flush_batch` / `emac_gap_ticks` /
`queue_mode` tuning.

## CI (channel-info)

Manager **`get_channel_info` / `gci`** aggregates upstream stats locally (no radio CI UDP).

## Tuning reference

| Knob | Where | Runtime? |
|------|-------|----------|
| Offer rate | `winject.max_rate_kbps` | yes (cfg) |
| DATA MPDUs per scheduler tick | `winject.max_data_per_tick` | yes (cfg) |
| Manager inject burst | `winject.tx_burst_size`, `winject.tx_burst_interval_us` | yes (cfg) |
| Stream pull depth | `upstream-N.scheduler_budget` | yes (cfg) |
| WiFi TX queue | `WIFI_RADIO_TX_QUEUE` (=20) | build |
| WiFi outstanding TX | `WIFI_RADIO_MAX_IN_FLIGHT` (=4) | build |
| WiFi task burst / gap | console **`set_wifi_tx_tune`** (`burst_size`, `burst_gap_us`) | yes |
| WiFi outstanding TX (runtime) | console **`set_wifi_tx_tune max_in_flight=`** | yes |
| Manager burst / tick / rate | manager UDP console **`set_tx_pacing`** / **`get_tx_pacing`** | yes (needs `manager.console_in/out`) |
| EMAC DMA burst length | `set_eth_dma_burst_len` (NVS, reboot) | yes (reboot) |

Historical bench numbers that compared **`ci_pace_inject`** / **`tx_grant`** vs passive
pacing are obsolete; see [bench-asymmetry.md](bench-asymmetry.md) for current asymmetry
experiments.
