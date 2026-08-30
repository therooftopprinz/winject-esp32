# WT32-ETH01 tests

Two-radio checks against WInject-ESP32 firmware. Ethernet UDP in, raw 802.11 in the air, UDP out the other board. Frame format and console commands are in [winject.md](winject.md).

The runner **sets channel and modulation on both radios** over TCP `2323` (`set_channel`, `set_modulation`) before each rate. Default is a sweep of every firmware modulation.

# Topology

```
host  --UDP 9000-->  radio A  --802.11 TX-->  air  --802.11 RX-->  radio B  --UDP 9002-->  host
host  <--UDP 9001--  radio A  <--802.11 RX--  air  <--802.11 TX--  radio B  <--UDP 9000--  host
```

| Role | Default | Notes |
|------|---------|--------|
| Radio A | `192.168.253.11` | `set_upstream_rx 9000`, `set_upstream_tx <host> 9001` |
| Radio B | `192.168.253.12` | `set_upstream_rx 9000`, `set_upstream_tx <host> 9002` |
| Host | this machine | TCP console `2323`; UDP listen `9001` / `9002` |
| Channel | `1` | `set_channel` on both radios before each modulation |
| Modulation | `all` | `set_modulation` on both radios; see list below |
| Payload | 1400 bytes | UDP body only; firmware max 1476 |

The host must be on the same IPv4 subnet as both Ethernet ports. STA MACs are chip-unique; BSSID is `BA:DD:CA:FE:BA:BE` on both. Radios do not loop their own TX back to `upstream_tx`.

# Runner

```bash
python3 tools/bw_test.py
python3 tools/bw_test.py --channel 1 --modulation all
python3 tools/bw_test.py --modulation DSS_1M_L
python3 tools/bw_test.py --modulation OFDM_6M,OFDM_24M,OFDM_54M --channel 6
python3 tools/bw_test.py --kbps 400 --modulation CCK_11M_S
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--a` / `--b` | `.11` / `.12` | Radio Ethernet addresses |
| `--host` | auto | Address the radios send `upstream_tx` back to |
| `--channel` | `1` | `set_channel` on **both** radios (1–13) |
| `--modulation` | `all` | One name, comma-separated list, or `all` |
| `--size` | `1400` | UDP payload bytes (16–1476) |
| `--duration` | `5` | Seconds per bandwidth phase |
| `--drain` | `1` | Wait after sending for late frames |
| `--kbps` | auto | Payload offer in kbit/s. `-1` (default) is 85% of estimated 802.11 goodput for **that** modulation. `0` floods the Ethernet inject path (not a goodput test) |
| `--integrity` | `20` | Packets per direction before bandwidth |
| `--skip-config` | off | Do not send console commands |
| `--skip-bidir` | off | Skip the simultaneous A+B phase |
| `--verbose` | off | Print full console replies |

Each modulation step sends, on both radios:

```
set_channel <channel>
set_modulation <modulation>
```

Upstream ports are configured once at the start. After a modulation change the runner waits 0.8 s so `esp_wifi` can reapply rate/channel/monitor.

Do not use `--kbps 0` to measure air rate. The ESP32 poll loop will drop UDP before inject (`udp_rx_drop`), goodput collapses, and the boards can reboot.

# Test cases (per modulation)

## Integrity

20 datagrams of 64 bytes, 30 ms apart, each direction, sequential. One automatic retry if a count is short.

## Unidirectional bandwidth

`--duration` seconds of `--size` payloads, A→B then B→A, at the auto (or `--kbps`) offer for that modulation.

## Simultaneous bidirectional bandwidth

Both directions send at once. Each direction is offered **half** the unidirectional rate. Both must stay within the loss limit.

# Pass criteria (per modulation)

| Check | Pass |
|-------|------|
| Config | `set_channel` and `set_modulation` return `ok` on both radios |
| Integrity | 20/20 each way (after retry) |
| Unidirectional | each direction `loss%` ≤ 5 at the auto/`--kbps` offer |
| Simultaneous | each direction `loss%` ≤ 10 at half uni offer |

Sweep exit status is 0 only if **every** listed modulation passes. High PHY rates can fail the loss limit because the ESP32 inject path cannot offer the estimated air goodput; the table still records delivered **goodput kbps**.

# Counters

| Counter | Meaning |
|---------|--------|
| `udp_rx` | Ethernet datagrams accepted and injected |
| `udp_rx_drop` | Ethernet datagrams not injected (too long, inject fail, or overflow) |
| `wifi_tx` / `wifi_tx_fail` | Inject attempts |
| `wifi_rx` | Promiscuous frames that matched DA broadcast + BSSID |
| `udp_tx` | Payloads forwarded to the host |

# Modulations

Same names as `set_modulation` / `help`:

`DSS_1M_L DSS_2M_S DSS_2M_L CCK_5M_L CCK_5M_S CCK_11M_L CCK_11M_S OFDM_6M OFDM_9M OFDM_12M OFDM_18M OFDM_24M OFDM_36M OFDM_48M OFDM_54M OFDM_MCS0_LGI … OFDM_MCS7_LGI OFDM_MCS0_SGI … OFDM_MCS7_SGI`

# Example

Bench, 2026-08-30, channel 1, 1400-byte payload, 5 s phases, auto offer. `set_channel 1` and `set_modulation` applied on both radios before each row.

```bash
python3 tools/bw_test.py --a 192.168.253.11 --b 192.168.253.12 --channel 1 --modulation all
```

| modulation      | ch | offer kbps |  int A->B |  int B->A | A->B kbps | A->B loss | B->A kbps | B->A loss | A+B A kbps | A+B B kbps | A+B A loss | A+B B loss | result |
|-----------------|---:|-----------:|----------:|----------:|----------:|----------:|----------:|----------:|-----------:|-----------:|-----------:|-----------:|--------|
| DSS_1M_L        |  1 |        794 |        20 |        20 |     790.7 |       0.6 |     795.2 |       0.0 |      387.5 |      392.0 |        2.8 |        1.7 | PASS   |
| DSS_2M_S        |  1 |       1512 |        20 |        20 |    1509.8 |       0.3 |    1503.0 |       0.7 |      728.0 |      737.0 |        3.8 |        2.7 | PASS   |
| DSS_2M_L        |  1 |       1512 |        20 |        20 |    1507.5 |       0.4 |    1509.8 |       0.3 |      723.5 |      732.5 |        4.4 |        3.3 | PASS   |
| CCK_5M_L        |  1 |       3564 |        20 |        20 |    3559.4 |       0.1 |    3561.6 |       0.1 |     1718.1 |     1745.0 |        3.6 |        2.1 | PASS   |
| CCK_5M_S        |  1 |       3564 |        20 |        20 |    3543.7 |       0.6 |    3552.6 |       0.3 |     1731.5 |     1751.7 |        2.9 |        1.8 | PASS   |
| CCK_11M_L       |  1 |       5820 |        20 |        20 |    5803.8 |       0.3 |    5812.8 |       0.2 |     2822.4 |     2820.2 |        3.1 |        3.2 | PASS   |
| CCK_11M_S       |  1 |       5820 |        20 |        20 |    5808.3 |       0.2 |    5799.4 |       0.4 |     2847.0 |     2851.5 |        2.2 |        2.1 | PASS   |
| OFDM_6M         |  1 |       3810 |        20 |        20 |    3794.6 |       0.4 |    3774.4 |       0.9 |     1859.2 |     1863.7 |        2.5 |        2.2 | PASS   |
| OFDM_9M         |  1 |       5102 |        20 |        20 |    5078.1 |       0.5 |    5071.4 |       0.6 |     2481.9 |     2466.2 |        2.7 |        3.3 | PASS   |
| OFDM_12M        |  1 |       8356 |        20 |        20 |    8305.9 |       0.6 |    8281.3 |       0.9 |     4094.7 |     4079.0 |        2.0 |        2.4 | PASS   |
| OFDM_18M        |  1 |      11569 |        20 |        20 |   11504.6 |       0.6 |   11464.3 |       0.9 |     5653.8 |     5638.1 |        2.3 |        2.6 | PASS   |
| OFDM_24M        |  1 |      14323 |        20 |        20 |   14192.6 |       0.9 |   14152.3 |       1.2 |     6919.4 |     6917.1 |        3.4 |        3.4 | PASS   |
| OFDM_36M        |  1 |      18798 |        20 |        20 |   18531.5 |       1.4 |   18238.1 |       3.0 |     8684.5 |     8621.8 |        7.6 |        8.3 | PASS   |
| OFDM_48M        |  1 |      22278 |        20 |        20 |   21300.2 |       4.4 |   20148.8 |       9.5 |     9625.3 |     9000.3 |       13.6 |       19.2 | FAIL   |
| OFDM_54M        |  1 |      23743 |        20 |        20 |   21275.5 |      10.4 |   19971.8 |      15.9 |     8971.2 |     8514.2 |       24.4 |       28.3 | FAIL   |
| OFDM_MCS0_LGI   |  1 |       4047 |        20 |        18 |    4027.5 |       0.5 |    4023.0 |       0.6 |     1964.5 |     1962.2 |        3.0 |        3.1 | FAIL   |
| OFDM_MCS1_LGI   |  1 |       8928 |        20 |        20 |    8895.0 |       0.4 |    8857.0 |       0.8 |     4363.5 |     4361.3 |        2.3 |        2.3 | PASS   |
| OFDM_MCS2_LGI   |  1 |      12296 |        20 |        20 |   12252.8 |       0.4 |   12208.0 |       0.7 |     5936.0 |     5904.6 |        3.5 |        4.0 | PASS   |
| OFDM_MCS3_LGI   |  1 |      15156 |        20 |        20 |   15019.2 |       0.9 |   15081.9 |       0.5 |     7345.0 |     7340.5 |        3.1 |        3.1 | PASS   |
| OFDM_MCS4_LGI   |  1 |      19747 |        20 |        20 |   19609.0 |       0.7 |   19290.9 |       2.3 |     9392.3 |     9329.6 |        4.9 |        5.5 | PASS   |
| OFDM_MCS5_LGI   |  1 |      23272 |        20 |        20 |   22182.7 |       4.7 |   22225.3 |       4.5 |    10209.9 |    10028.5 |       12.3 |       13.8 | FAIL   |
| OFDM_MCS6_LGI   |  1 |      24744 |        20 |        20 |   22760.6 |       8.0 |   22805.4 |       7.8 |    10171.8 |    10071.0 |       17.8 |       18.6 | FAIL   |
| OFDM_MCS7_LGI   |  1 |      26064 |        20 |        20 |   22241.0 |      14.7 |   22509.8 |      13.6 |     9663.4 |    10053.1 |       25.9 |       22.9 | FAIL   |
| OFDM_MCS0_SGI   |  1 |       4363 |        20 |        20 |    4345.6 |       0.4 |    4325.4 |       0.9 |     2137.0 |     2125.8 |        2.1 |        2.6 | PASS   |
| OFDM_MCS1_SGI   |  1 |       9703 |        20 |        20 |    9661.1 |       0.4 |    9620.8 |       0.9 |     4746.6 |     4737.6 |        2.2 |        2.4 | PASS   |
| OFDM_MCS2_SGI   |  1 |      13315 |        20 |        20 |   13274.2 |       0.3 |   13236.2 |       0.6 |     5156.5 |     5149.8 |       22.6 |       22.7 | FAIL   |
| OFDM_MCS3_SGI   |  1 |      16296 |         0 |         0 |       0.0 |     100.0 |       0.0 |     100.0 |        0.0 |        0.0 |      100.0 |      100.0 | FAIL   |
| OFDM_MCS4_SGI   |  1 |      21011 |         0 |         0 |       0.0 |     100.0 |       0.0 |     100.0 |        0.0 |        0.0 |      100.0 |      100.0 | FAIL   |
| OFDM_MCS5_SGI   |  1 |      24594 |         0 |         0 |       0.0 |     100.0 |       0.0 |     100.0 |        0.0 |        0.0 |      100.0 |      100.0 | FAIL   |
| OFDM_MCS6_SGI   |  1 |      26064 |         0 |         0 |       0.0 |     100.0 |       0.0 |     100.0 |        0.0 |        0.0 |      100.0 |      100.0 | FAIL   |
| OFDM_MCS7_SGI   |  1 |      27373 |         0 |         0 |       0.0 |     100.0 |       0.0 |     100.0 |        0.0 |        0.0 |      100.0 |      100.0 | FAIL   |

**19/31 PASS.** Sweep takes about 12 minutes at `--duration 5`.

- DSS, CCK, and OFDM through `OFDM_36M` meet the loss limits at auto offer.
- `OFDM_48M` / `OFDM_54M` and MCS5–7 LGI still deliver ~20–23 Mbps but miss the 5%/10% loss limits — the ESP32 inject path cannot fill the estimated air rate.
- `OFDM_MCS0_LGI` failed only integrity (18/20 on retry); goodput was fine.
- `OFDM_MCS3_SGI`–`MCS7_SGI` going to 0/20 in that sweep was **not** an SGI PHY bug. Isolated probes (20-byte integrity) pass MCS3/MCS7 SGI 20/20 with `wifi_tx`/`wifi_rx` matching. Replaying `MCS2_SGI` → `MCS3_SGI` → `MCS7_SGI` after a cold start: MCS2/MCS3 SGI pass the loss limits; MCS7 SGI delivers ~23.5 Mbps but misses 5% loss at a 27 Mbps offer (same inject ceiling as MCS7 LGI). The original blackout was leftover radio state after the long high-rate sweep (`MCS2_SGI` bidir was already 22% loss there vs ~3% on a short replay). Re-run SGI rows on their own before treating them as unsupported.
