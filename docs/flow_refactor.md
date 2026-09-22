# Upstream TX/RX flow (target model)

## TX

`upstream_tx` (EMAC L2 hijack) → `wifi_tx` queue → `wifi_tx` task → `esp_wifi_80211_tx`

- **upstream_tx**
  - Drop if `wifi_tx` queue is full (`channels_tx drop_queue_full`)
  - Adopt EMAC frame into `packet` (no extra pool copy on hijack path)
- **wifi_tx**
  - Queue depth / HWM metrics
  - `tx_enqueue_ok` / `tx_enqueue_fail`
  - Burst drain + inter-burst gap in the task loop
  - `max_in_flight` cap on outstanding 802.11 TX

No L2 staging task, no console `tx_grant`, no `set_inject_tune`. Manager pacing uses
`tx_scheduler` token bucket + burst gate; see [cd-protocol.md](cd-protocol.md).

## RX

`wifi_rx` promisc CB → RX queue → `upstream_rx` task → UDP forward

- **wifi_rx**
  - Drop if RX queue full / pool exhausted
- **upstream_rx**
  - `forward_bytes` to bound `set_upstream_rx` dest

## Telemetry

Firmware **channel_info UDP** removed (NVS settings **v7**). Use console **`status`**
(atomic counters) and manager **`get_channel_info` / `gci`** (stream stats from the
manager process, not radio CI fanout).
