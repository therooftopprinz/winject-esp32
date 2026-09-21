# WT32-ETH01 tests

Three paths:

1. **Manager UDP (preferred)** — `scripts/manager_bw_test.sh` (default; `lat_udp_*.cfg`).
2. **Manager TCP** — `scripts/manager_bw_test.sh --tcp` (`bw_*.cfg`, length-prefixed ARQ).
3. **Direct** — host stamps MPDUs to radios (`scripts/stand_alone_test.py` / `bw_test.py --direct`).

Frame format and console commands: [winject.md](winject.md). Manager details: [manager.md](manager.md).

Channel, modulation, CCA, and TX power are left as already configured unless `--channel`, `--modulation` / `--all`, `--cca` / `--no-cca`, or `--power` is given. `--all` sweeps every firmware modulation. Prepare always sets `STANDALONE` mode + domain + upstreams.

**Peer air RX (A→B / integrity):** use **`OFDM_24M`** on both radios. HT MCS is for TX / USB sniff benchmarks only — see [winject.md — Air RX](winject.md#air-rx-and-fcs-promiscuous). `manager_bw_test.sh` applies `OFDM_24M` when you omit `--modulation`.

# Topology

```
host UDP/TCP :29000 → manager A → radio A inject MPDU(bus=b2) → air → radio B forward → manager B demux b2 → host :9002
host :9001  ← manager A demux d4 ← radio A forward ← air ← radio B inject MPDU(bus=d4) ← manager B ← host :29001
```

| Item | Value |
|------|--------|
| Mode / domain | Prepare sets radio `set_mode STANDALONE` + `set_domain`; manager stamps Addr3 from `winject.mode` / `winject.domain` (`1234`) |
| Pair 1 (A→B) | A `upstream_tx=b2` `upstream_rx=a1`; B swapped |
| Pair 2 (B→A) | A `upstream_tx=c3` `upstream_rx=d4`; B swapped |
| Inject / forward | A `9000`/`9010` → host `9210`/`9211`; B → `9220`/`9221` |
| UDP configs | `configuration/winject-tests/lat_udp_a.cfg` / `lat_udp_b.cfg` |
| TCP configs | `configuration/winject-tests/bw_a.cfg` / `bw_b.cfg` |
| Channel | omit `--channel` to keep; `--channel N` sets both radios before each modulation |
| Modulation | omit `--modulation` to keep; `--modulation NAME` or `--all` |
| CCA | enabled; `--no-cca` skips wait-for-idle |
| Payload | 1400 bytes (max 1476); TCP path is length-prefixed |

Radios share a domain; Addr3 mode prefix is stamped by the manager. Isolation is manager-side bus stamp/demux after the radio forwards full MPDUs.

`bw_test.py` does not rebind `sut`/`sur` under `--udp`/`--tcp` (prepare + managers own those) but still applies `--channel` / `--modulation` / CCA unless `--skip-config` is also set.

```bash
# Manager UDP (default PHY = OFDM_24M for peer RX)
./scripts/manager_bw_test.sh --a 192.168.253.9 --b 192.168.253.14 --no-cca
./scripts/manager_bw_test.sh --modulation OFDM_24M --kbps 8000

# Manager TCP ARQ
./scripts/manager_bw_test.sh --tcp --modulation OFDM_24M

# Direct MPDU (no managers)
python3 scripts/stand_alone_test.py --a 192.168.253.9 --b 192.168.253.14 --modulation OFDM_24M --no-cca
```

With managers already up:

```bash
python3 tools/bw_test.py --udp --modulation OFDM_24M
python3 tools/bw_test.py --tcp --test-integ --test-ab --test-ba --modulation OFDM_24M

# A→B vs B→A: inject_wait / promisc deltas (swap --a/--b to confirm per-radio asymmetry)
python3 tools/dir_timing_compare.py --a 192.168.253.9 --b 192.168.253.14
```

Analysis for **`.9` / `.14`**: [bench-asymmetry.md](bench-asymmetry.md).

# Runner flags

| Flag | Default | Meaning |
|------|---------|---------|
| `--a` / `--b` | `.11` / `.12` | Radio Ethernet addresses |
| `--host` | auto | Host IP used when probing radio status |
| `--domain` | `1234` | Shared air domain (hex, 1–ffff); informational check vs radio status |
| `--bus-ab` / `--bus-ba` | `b2` / `a1` | Informational (managers stamp buses from cfg) |
| `--channel` | omit | `set_channel` on **both** radios (1–14; 14 is 802.11b-only). Omit to keep the radios’ current channel |
| `--modulation` | omit | One name or comma-separated list. Omit to keep the radios’ current modulation |
| `--all` | off | Sweep every firmware modulation (`set_modulation` on both radios). Not with `--modulation` |
| `--size` | `1400` | Payload bytes (16–1476) |
| `--duration` | `5` | Seconds per bandwidth phase |
| `--drain` | `1` | Wait up to N seconds for recv==sent, then settle N more (`0` = skip) |
| `--kbps` | auto | Payload offer in kbit/s. `-1` (default) is estimated air goodput; TCP path uses ~55% for ARQ headroom. `0` floods |
| `--integrity` | `20` | Packets per direction for `--test-integ` |
| `--cca` / `--no-cca` | omit | `set_cca_enabled` only when flagged; omit keeps radio CCA |
| `--skip-config` | off | Do not send console commands |
| `--udp` / `--tcp` / `--direct` | required | Manager UDP, manager TCP ARQ, or host MPDU inject |
| `--tcp-send-a` / `--tcp-send-b` | `29000` / `29001` | Manager send ports (UDP or TCP) |
| `--test-integ` | on* | Integrity check (both directions) |
| `--test-ab` | on* | Unidirectional A→B bandwidth |
| `--test-ba` | on* | Unidirectional B→A bandwidth |
| `--test-bidir` | off | Simultaneous A+B bandwidth |
| `--verbose` | off | Print full console replies |

\* If no `--test-*` flag is given, defaults to `--test-integ --test-ab --test-ba`.
Each rate step always sends `set_mode STANDALONE`. `set_modulation` / `set_channel` / `set_cca_enabled` only when those flags (or `--all`) are given.

After a channel or modulation change the runner waits 1.2 s so `esp_wifi` can reapply rate/channel/monitor.

Prefer `--kbps 7000`–`8000` or `--no-cca` when measuring high rates; if the air path is lossy, step down.

# Test cases (per modulation)

## Integrity

20 datagrams of 64 bytes, 30 ms apart, each direction, sequential. One automatic retry if a count is short.

## Unidirectional bandwidth

`--test-ab` / `--test-ba`: `--duration` seconds of `--size` payloads at the auto (or `--kbps`) offer for that modulation.

## Simultaneous bidirectional bandwidth

`--test-bidir`: both directions send at once. Each direction is offered **half** the unidirectional rate. Both must stay within the loss limit.

# Pass criteria (per modulation)

| Check | Pass |
|-------|------|
| Config | `set_cca_enabled` returns `ok` on both radios (`set_channel` / `set_modulation` too if those flags were given) |
| Integrity | `--test-integ`; 20/20 each way (after retry) |
| Unidirectional | `--test-ab` / `--test-ba`; each selected direction `loss%` ≤ 25 at the auto/`--kbps` offer |
| Simultaneous | `--test-bidir`; each direction `loss%` ≤ 10 at half uni offer |

Sweep exit status is 0 only if **every** listed modulation passes. High PHY rates can fail the loss limit because TCP ARQ and the ESP32 inject path cannot offer the estimated air goodput; the table still records delivered **goodput kbps**.

Radio ETH→WiFi inject timeshare (EMAC gap), MCS7 flood ≥30 Mbps, and how to read USB `mon0` TX-window rates (~16 Mbps effective at MCS4@25M): [winject.md — ETH→WiFi TX performance](winject.md#ethwifi-tx-performance).

# Modulations

Same names as `set_modulation` / `help`:

`DSS_1M_L DSS_2M_S DSS_2M_L CCK_5M_L CCK_5M_S CCK_11M_L CCK_11M_S OFDM_6M OFDM_9M OFDM_12M OFDM_18M OFDM_24M OFDM_36M OFDM_48M OFDM_54M OFDM_MCS0_LGI … OFDM_MCS7_LGI OFDM_MCS0_SGI … OFDM_MCS7_SGI`

# Example

```bash
./scripts/manager_bw_test.sh --a 192.168.253.11 --b 192.168.253.12 --channel 1 --all --test-integ --test-ab --test-ba --test-bidir
```
