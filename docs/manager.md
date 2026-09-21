WInject manager is a helper app that runs outside the ESP32. It programs the radio over the UDP console, stamps full 802.11 MPDUs (bus slots + Addr3), and bridges other apps onto one host stream per `upstream-N`. The radio has a single inject/forward UDP pair; bus demux is host-side.

UDP apps send and receive datagrams unchanged on the host sockets. Over the air each LCP body is prefixed with a per-TX `uint16` sequence (see [Air sequence](#air-sequence)) before MPDU packing. TCP apps are terminated here; the byte stream is carried as UDP payloads inside MPDU slots.

# Sample config

```
winject.device        = <host>
winject.console       = <port>
winject.channel       = 1
winject.modulation    = OFDM_24M
winject.power         = 20
winject.mode          = STANDALONE
winject.domain        = 1234
# Optional. Default inject 9000 / forward 9210.
# winject.inject_port   = 9000
# winject.forward_port  = 9210
# winject.forward_base  = 9210   # legacy alias for forward_port
# Optional. Default 10000 kbps if omitted.
# winject.max_rate_kbps = 10000
# Optional local UDP management console (both required together).
# manager.console_in  = 127.0.0.1:2424
# manager.console_out = 127.0.0.1:2425

upstream.size = 5

upstream-0.mode             = UDP_GENERIC_FORWARDING
upstream-0.scheduler_budget = 100
upstream-0.rx               = <interface>:<port>
upstream-0.tx               = <target_host>:<port>
upstream-0.tx_bus           = b2
upstream-0.rx_bus           = a1

# GStreamer udpsink -> manager (bind and receive); TX-only video example
upstream-0.mode             = UDP_SERVER_FORWARDING
upstream-0.scheduler_budget = 1024
upstream-0.bind_address     = <interface>:<port>
upstream-0.tx_bus           = b2
# Optional packet-block Reed-Solomon (ISA-L). TX encode only; RX auto-decodes.
# upstream-0.fec.type         = RS_BLOCK_ERASURE
# upstream-0.fec.k            = 10
# upstream-0.fec.n            = 15
# upstream-0.fec.timeout_ms   = 20

# Manager -> GStreamer udpsrc (send); RX-only peer of the above
upstream-0.mode             = UDP_CLIENT_FORWARDING
upstream-0.scheduler_budget = 1024
upstream-0.connect_address  = <target_host>:<port>
upstream-0.rx_bus           = b2

# SSH client forwarding (manager connects to sshd); TCP ARQ needs both
upstream-0.mode             = TCP_CLIENT_FORWARDING
upstream-0.scheduler_budget = 1024
upstream-0.rcv_buffer_size  = 16384
upstream-0.snd_buffer_size  = 16384
upstream-0.connect_address  = <target_host>:<port>
upstream-0.tx_bus           = c3
upstream-0.rx_bus           = d4

# SSH server forwarding (SSH client connects here)
upstream-0.mode             = TCP_SERVER_FORWARDING
upstream-0.scheduler_budget = 1024
upstream-0.rcv_buffer_size  = 16384
upstream-0.snd_buffer_size  = 16384
upstream-0.bind_address     = <interface>:<port>
upstream-0.tx_bus           = d4
upstream-0.rx_bus           = c3
```

`winject.device` / `winject.console` are the ESP32 Ethernet address and UDP console (firmware default 22). Optional `winject.local_ip` overrides the address used in `set_upstream_rx`; otherwise it is inferred from the console socket. Radio commands (`sc` / `sd` / `sut` / `sur` / …) stay on that UDP socket; see [winject.md](winject.md). `winject.mode` is used when the manager stamps Addr3; it is **not** sent as `set_mode` by the manager. Prepare (`prepare_radios_for_manager.py`) still issues `set_mode STANDALONE` so the radio's RX Addr3 filter matches.

If the radio console is down at startup, the manager stays running and retries until commands succeed. After the UDP console is open, every **500 ms** it sends `ping` and expects `pong`. On missed pong or send/recv error it closes the socket, retries, then re-applies radio settings, the single `sut`/`sur` pair, plus channel-info subscribe (`suc`).

Optional `manager.console_in` / `manager.console_out` start a **local UDP** management console (FEC, scheduler budget, modulation, channel info). See [Manager console](#manager-console).

# Domain and bus

Each `upstream-N` owns one host socket. All streams share one ESP32 inject/forward port pair. The manager stamps buses into MPDU slots and demuxes RX by `rx_bus`.

- `winject.domain` — hex `1`…`ffff` (required). Both managers on a link must use the same domain.
- `winject.inject_port` / `winject.forward_port` — radio UDP ports (defaults `9000` / `9210`). `forward_base` is a legacy alias for `forward_port`.
- `upstream-N.tx_bus` — bus stamped on host→air PDUs. 1–2 hex digits, not `0`. Optional.
- `upstream-N.rx_bus` — bus filtered on air→host PDUs. Optional.
- At least one of `tx_bus` / `rx_bus` is required. TCP modes require both (ARQ). When both are set they must differ.
- All configured `tx_bus` / `rx_bus` values on one manager must be unique.
- Bidirectional peers swap: A’s TX bus is B’s RX bus, and vice versa. Unidirectional peers share one bus (source TX, sink RX).
- `BFC_TUNNEL_DEVICE`: `upstream.size` must be 1 (same domain/bus rules).

The manager issues PHY knobs (`set_channel` / `set_modulation` / `set_tx_power`), then `set_domain`, then once `set_upstream_tx port=<inject>` and `set_upstream_rx host=<local_ip> port=<forward>`. It also binds an ephemeral UDP port and issues `set_upstream_ci to=<local_ip>:<ci_port>` so the radio fans out channel-info (TX flow-control queue depth and RX rssi/snr). The manager caches the last samples with their receive times and prints them (`flow_t` / `rssi_t`) from `gci` and periodic `winject.stats_sec` logging; the TX scheduler gates DATA MPDUs when a fresh FLOW_CTRL sample shows the
radio TX queue above half full (`skip_console` tests use
`tools/configure_manager_ci.py` to subscribe the radio to the manager CI port).

UDP upstream TX uses `peek_tx` / `commit_tx` so a failed non-blocking
`sendto` to the radio inject port does not drop payloads that were already
dequeued from the host txq.

Rover vehicle control does not use this UDP console. Drive-console text is an
extra **UDP** upstream (buses `72`/`81`); see [rover.md](rover.md).

# Air sequence

Every LCP body the manager places in an MPDU slot is prefixed with a big-endian `uint16` sequence number. Each upstream TX has its own counter (starts at 0, wraps). The peer strips the prefix after MPDU unpack / bus demux before FEC/TCP/UDP handling. A forward gap (wrapping subtract, less than 32768) is counted as `rx_pkt_loss`. The first received seq seeds the expected value (joining mid-stream is not a gap). Duplicates and backward seqs are ignored. Host app payloads are unchanged.

The radio forwards/injects opaque full MPDUs (max 1500). Stream payloads are therefore at most 1474 bytes inside an LCP body. Both managers on a link must run this version.

# Modes

| Mode | Local socket |
|------|----------------|
| `UDP_GENERIC_FORWARDING` | bind `rx`, send to `tx` |
| `UDP_SERVER_FORWARDING` | bind `bind_address`, reply to last sender |
| `UDP_CLIENT_FORWARDING` | send/recv `connect_address` |
| `TCP_SERVER_FORWARDING` | listen `bind_address` (one client) |
| `TCP_CLIENT_FORWARDING` | connect `connect_address` after the peer stream is up |

Host TCP receive for the TCP modes runs on a dedicated blocking-read thread per connection; bytes are queued onto the reactor for ARQ and radio inject so a full send window cannot stall ACK processing. Over the air the manager uses selective-repeat ARQ with cumulative ACK + SACK blocks (out-of-order DATA is buffered, only missing SNs are retransmitted). DATA and ACK share the upstream’s `tx_bus` / `rx_bus` pair (same UDP inject/forward path).

`scheduler_budget` is the max bytes this upstream may inject per scheduler wakeup. `winject.max_rate_kbps` caps aggregate DATA inject rate (ACKs are not charged). If omitted or `0`, the default is 10000. The scheduler runs on a 500 µs timer and also immediately after radio RX / TCP ingest so reverse ACKs are not delayed a full tick.

# Manager console

Optional **UDP** console on the host, separate from the radio UDP console. Runtime only: `suf` / `sus` / `sd` update the live streams / radio and the in-memory config; they are not written back to the cfg file. Omit both keys to disable. If one is set, the other is required.

```
manager.console_in  = 127.0.0.1:2424   # bind; receive commands
manager.console_out = 127.0.0.1:2425   # fixed reply destination
```

The manager binds `console_in` and **always** sends replies to `console_out` (not last-sender). `nc` to `console_in` without binding `console_out` as the source port will not show replies.

Interactive (bind the reply port as the UDP source):

```bash
nc -u -p 2425 127.0.0.1 2424
help
gci
```

One-shot (separate listener on the reply port):

```bash
nc -u -l 127.0.0.1 2425 &
echo 'gci' | nc -u -w1 127.0.0.1 2424
```

One datagram = one line (max 255 bytes). Trailing CR/LF/space/tab are stripped. Empty lines and `#` comments are ignored. Tokens split on space/tab. `<index>` is this manager’s `upstream-N` (0-based).

Replies:

| Kind | Body |
|------|------|
| Success, no payload | `ok` |
| Success with args | `ok <args>` (`gci` args may span several lines in **one** datagram) |
| Failure | `nok <msg>` |
| `ping` | `pong` |
| `help` / `?` | command list (no `ok` prefix) |

| Command | Short | Action |
|---------|-------|--------|
| `help` | `?` | list commands |
| `ping` | | `pong` (manager console keepalive; not the radio UDP ping) |
| `get_channel_info` | `gci` | last radio channel-info plus cumulative on-air total / unfecced per-stream counters |
| `get_upstream_fec <index>` | `guf` | TX FEC on that upstream |
| `set_upstream_fec <index> <NONE\|RS_BLOCK_ERASURE> <k> <n>` | `suf` | TX encode; UDP only |
| `get_upstream_scheduler_budget <index>` | `gus` | bytes per scheduler wakeup |
| `set_upstream_scheduler_budget <index> <budget>` | `sus` | budget `> 0` |
| `set_modulation <modulation>` | `sd` | radio TX rate (`set_modulation` on the radio UDP console) |
| `get_modulation` | `gd` | last applied / configured modulation |

`help` prints:

```
set_upstream_fec|suf <index> <NONE|RS_BLOCK_ERASURE> <k> <n>
get_upstream_fec|guf <index>
set_upstream_scheduler_budget|sus <index> <budget>
get_upstream_scheduler_budget|gus <index>
set_modulation|sd <modulation>
get_modulation|gd
get_channel_info|gci
ping
help
```

## FEC (`suf` / `guf`)

UDP upstreams only. TCP or a missing index replies `nok fec only valid for UDP upstreams` / `nok invalid upstream index`.

`NONE` (or `none`) disables TX encode. `k` and `n` are still required on the line; use `0 0`. RX still auto-decodes shard headers on every UDP upstream.

`RS_BLOCK_ERASURE` needs `1 <= k < n <= 255`. Changing encode params flushes any partial TX block first. `fec.timeout_ms` is not settable here; the existing config value (default 20) is kept.

```text
suf 0 RS_BLOCK_ERASURE 10 15
guf 0
suf 0 NONE 0 0
```

Getters: `ok NONE 0 0` or `ok RS_BLOCK_ERASURE <k> <n>`. Same encode rules as [UDP FEC](#udp-fec-rs_block_erasure).

## Scheduler budget (`sus` / `gus`)

Max bytes that upstream may inject per scheduler wakeup (UDP and TCP). `budget` must be `> 0`. Getter: `ok <budget>`.

```text
sus 1 4096
gus 1
```

## Modulation (`sd` / `gd`)

TX PHY rate on this manager’s radio (same names as radio UDP `set_modulation`). Names are case-insensitive. Channel 14 still requires DSSS/CCK. Needs the radio UDP console open; otherwise `nok console not connected`. Getter is the in-memory value (config, then last successful `sd`). Not written back to the cfg file; reconnect re-applies it.

```text
sd OFDM_24M
gd
```

Getter: `ok OFDM_24M`. Unknown name: `nok unknown modulation`. Channel 14 + OFDM/MCS: `nok channel 14 requires DSSS/CCK modulation`.

## Channel info (`gci`)

Last radio channel-info UDP samples plus lifetime counters (peeking does not reset them). Periodic `winject.stats_sec` INF `STREAM` lines are interval kbps / interval `LOST=` for the same window, not these totals. Missing CI samples print `-`. `flow_t` / `rssi_t` are local wall-clock times of those last UDP samples (`YYYY-MM-DDTHH:MM:SS.mmm`).

```
ok flow=<size>/<cap>|- flow_t=<time>|- rssi=<dbm>|- rssi_t=<time>|- snr=<db>|-
stream tx_byte=<n> rx_byte=<n> tx_pkt=<n> rx_pkt=<n> rx_pkt_loss=<n>
stream-N type=<UDP|TCP> fec=<none|block(k,n)> tx_byte=<n> rx_byte=<n> tx_pkt=<n> rx_pkt=<n> rx_pkt_loss=<n> fec_rec=<n> fec_lost=<n>
```

| Field | Meaning |
|-------|---------|
| `flow` | radio TX queue depth / capacity (flow-control sample) |
| `rssi` / `snr` | last RX air sample from this manager’s radio |
| `tx_byte` / `rx_byte` | `stream` total: cumulative on-air / raw bytes from the radio (FEC shards + seq). `stream-N`: cumulative payload after FEC decode. |
| `tx_pkt` / `rx_pkt` | cumulative air datagrams injected / received |
| `rx_pkt_loss` | cumulative missing air packets (uint16 seq gaps) |
| `fec_rec` / `fec_lost` | cumulative recovered originals / unrecoverable RX blocks (`stream-N` only) |
| `type` | `UDP` or `TCP` |
| `fec` | `none`, or `block(k,n)` when RS block erasure TX encode is on |

TCP rows add `queue` and `unacked`. All counters are cumulative from process start.

Example:

```
ok flow=12/32 flow_t=2026-09-10T12:17:04.123 rssi=-45 rssi_t=2026-09-10T12:17:04.180 snr=18
stream tx_byte=958000 rx_byte=12000 tx_pkt=478 rx_pkt=20 rx_pkt_loss=3
stream-0 type=UDP fec=block(10,12) tx_byte=790000 rx_byte=0 tx_pkt=468 rx_pkt=0 rx_pkt_loss=0 fec_rec=12 fec_lost=1
stream-1 type=UDP fec=none tx_byte=10000 rx_byte=12000 tx_pkt=10 rx_pkt=20 rx_pkt_loss=3 fec_rec=0 fec_lost=0
stream-2 type=TCP fec=none tx_byte=5000 rx_byte=4000 tx_pkt=8 rx_pkt=7 rx_pkt_loss=0 fec_rec=0 fec_lost=0 queue=2 unacked=1
```

## Forwarding the console over air

The console is a normal UDP bind/send pair, so another `upstream-N` can bridge it. Typical rover pairing: this manager `console_in=:2424` / `console_out=:2425`, plus a `UDP_GENERIC_FORWARDING` upstream `rx=:2425` `tx=:2424` (replies onto the air, air commands into the bind). The peer binds a `UDP_SERVER_FORWARDING` port and `nc`s that bind (last-sender replies). While that upstream is up it owns `:2425` — do not also `nc -l` the reply port on the same host.

This is not vehicle control and not the radio UDP console.

# UDP FEC (`RS_BLOCK_ERASURE`)

Optional on UDP upstreams that **encode** (TX). Groups `k` original datagrams and sends `n` on-air shards (systematic Cauchy Reed-Solomon via [ISA-L](https://github.com/intel/isa-l): NEON on aarch64, portable C elsewhere). The peer recovers the originals if any `k` of `n` shards arrive. Systematic shards are native size (2-byte length + payload); RS math and parity shards pad to the longest row in the block. Incomplete encode groups flush after `fec.timeout_ms` (default 20); empty data slots go out as a length-0 header, not a full-width zero row. **RX needs no `fec.*` config** — every UDP upstream decodes from the 8-byte shard header (`k`/`n` on the wire) and zero-pads short shards to the longest in that `block_id`. Finished `block_id`s are remembered for `max(timeout_ms*50, rx_hold)` (1 s at the default 20 ms timeout) so late duplicate shards are dropped, then forgotten so a peer restart that reuses ids from 0 is not treated as a replay. Firmware is unchanged; each shard plus the 2-byte seq is still one 802.11 body (max 1476 bytes). Not wire-compatible with `tools/fec.py` (different RS matrix). Both managers must have this decoder; an older peer rejects mixed shard sizes.

```
upstream-0.fec.type         = RS_BLOCK_ERASURE
upstream-0.fec.k            = 10
upstream-0.fec.n            = 15
# upstream-0.fec.timeout_ms   = 20
```

# Pairing example

Host A (SSH client side, video source):

```
winject.mode = STANDALONE
winject.domain = 1234
upstream-0.mode = TCP_SERVER_FORWARDING
upstream-0.bind_address = 127.0.0.1:22022
upstream-0.tx_bus = c3
upstream-0.rx_bus = d4
upstream-1.mode = UDP_SERVER_FORWARDING
upstream-1.bind_address = 127.0.0.1:22081
upstream-1.tx_bus = b2
```

Host B (sshd side, video sink):

```
winject.mode = STANDALONE
winject.domain = 1234
upstream-0.mode = TCP_CLIENT_FORWARDING
upstream-0.connect_address = 127.0.0.1:22
upstream-0.tx_bus = d4
upstream-0.rx_bus = c3
upstream-1.mode = UDP_CLIENT_FORWARDING
upstream-1.connect_address = 127.0.0.1:21082
upstream-1.rx_bus = b2
```

`ssh -p 22022 user@127.0.0.1` on A. GStreamer `udpsink` to A `:22081`, `udpsrc` on B `:21082`. Video uses one bus (`b2`) TX-only on A and RX-only on B; SSH keeps a swapped TX/RX pair for ARQ.

The manager fetches official [BFC](https://github.com/therooftopprinz/BFC) into the CMake build directory for INI parsing, sockets, and the epoll reactor.

Build: `cmake -S src/manager -B build_manager_arm && cmake --build build_manager_arm`. Run: `./build_manager_arm/winject-manager src/manager/winject.conf.example`. Host tests: see `.cursor/skills/build/SKILL.md`. Radio console commands are in [winject.md](winject.md).

# Bandwidth test (bw_test)

Use `scripts/manager_bw_test.sh` (UDP forwarding by default; `--tcp` for length-prefixed ARQ). Radios are `STANDALONE` with domain `1234` and two bus pairs (`b2`/`a1` for A→B, `c3`/`d4` for B→A); `prepare_radios_for_manager.py` programs `sut`/`sur` before managers start.

```bash
chmod +x scripts/manager_bw_test.sh
./scripts/manager_bw_test.sh --modulation OFDM_6M
./scripts/manager_bw_test.sh --modulation OFDM_6M --kbps 2000 --no-cca
./scripts/manager_bw_test.sh --tcp --modulation OFDM_6M --test-bidir
```

UDP configs: `configuration/winject-tests/lat_udp_a.cfg` / `lat_udp_b.cfg`. TCP configs: `bw_a.cfg` / `bw_b.cfg`. Host sends A→B via manager A `:29000`, receives on `:9002`; B→A uses `:29001` and listen `:9001`. Managers use `winject.skip_console` so the script programs the radios first. `tools/bw_test.py --udp` / `--tcp` does not rebind upstreams but still applies `--channel` / `--modulation` / CCA. TCP auto offer is ~55% of the UDP air estimate (ARQ headroom). Prefer `--kbps` pacing or `--no-cca` when measuring; if the air path is lossy, step down. `scripts/manager_tcp_bw_test.sh` remains a thin `--tcp` wrapper.

# Air TX-RX latency (lat_test)

Same-host one-way latency through both managers (ARM or x86). Probes carry `monotonic_ns`; no clock sync. Default spacing is 1 ms, so FEC `k=10` fills a block in ~10 ms instead of waiting for `fec.timeout_ms` (20).

```bash
./scripts/manager_lat_test.sh
./scripts/manager_lat_test.sh --no-cca --count 800 --interval-ms 1
CASES=raw,fec10-15,fec10-11,tcp ./scripts/manager_lat_test.sh
./scripts/manager_lat_test.sh --cases fec10-11 --ba
```

| Case | Path |
|------|------|
| `raw` | UDP, no FEC |
| `fec10-15` | UDP `RS_BLOCK_ERASURE` k=10 n=15 |
| `fec10-11` | UDP `RS_BLOCK_ERASURE` k=10 n=11 |
| `tcp` | manager TCP ARQ (`bw_a.cfg` / `bw_b.cfg`) |

UDP configs: `configuration/winject-tests/lat_udp_a.cfg` / `lat_udp_b.cfg` (same bus pairs and ports as the TCP bw test). Decoder emits a block only after `k` shards; encoder waits for `k` datagrams or the FEC timeout.
