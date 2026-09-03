WInject manager is a helper app that runs outside the ESP32. It programs the radio over the TCP console and bridges other apps onto one ESP32 UDP upstream per manager upstream.

UDP apps send and receive datagrams unchanged. TCP apps are terminated here; the byte stream is carried as UDP payloads on that upstream’s inject/forward ports.

# Sample config

```
winject.device        = <host>
winject.console       = <port>
winject.channel       = 1
winject.modulation    = OFDM_24M
winject.power         = 20
winject.mode          = STANDALONE
# Optional. Default 10000 kbps if omitted.
# winject.max_rate_kbps = 10000

upstream.size = 5

upstream-0.mode             = UDP_GENERIC_FORWARDING
upstream-0.airport          = <airport>
upstream-0.scheduler_budget = 100
upstream-0.rx               = <interface>:<port>
upstream-0.tx               = <target_host>:<port>

# GStreamer udpsink -> manager (bind and receive)
upstream-0.mode             = UDP_SERVER_FORWARDING
upstream-0.airport          = <airport>
upstream-0.scheduler_budget = 1024
upstream-0.bind_address     = <interface>:<port>
# Optional packet-block Reed-Solomon (ISA-L). Same k/n on both managers.
# upstream-0.fec.type         = RS_BLOCK_ERASURE
# upstream-0.fec.k            = 10
# upstream-0.fec.n            = 15
# upstream-0.fec.timeout_ms   = 20

# Manager -> GStreamer udpsrc (send)
upstream-0.mode             = UDP_CLIENT_FORWARDING
upstream-0.airport          = <airport>
upstream-0.scheduler_budget = 1024
upstream-0.connect_address  = <target_host>:<port>

# SSH client forwarding (manager connects to sshd)
upstream-0.mode             = TCP_CLIENT_FORWARDING
upstream-0.airport          = <airport>
upstream-0.scheduler_budget = 1024
upstream-0.rcv_buffer_size  = 16384
upstream-0.snd_buffer_size  = 16384
upstream-0.connect_address  = <target_host>:<port>

# SSH server forwarding (SSH client connects here)
upstream-0.mode             = TCP_SERVER_FORWARDING
upstream-0.airport          = <airport>
upstream-0.scheduler_budget = 1024
upstream-0.rcv_buffer_size  = 16384
upstream-0.snd_buffer_size  = 16384
upstream-0.bind_address     = <interface>:<port>
```

`winject.device` / `winject.console` are the ESP32 Ethernet address and TCP console (firmware default 2323). Optional `winject.local_ip` overrides the address used in `set_upstream_tx`; otherwise it is inferred from the console socket.

# Airport

Each `upstream-N` owns one ESP32 UDP upstream. `airport` is required and must be unique on this radio.

- STANDALONE broadcast: `00:00:00:xx:xx:xx`
- STANDALONE P2P: `xx:xx:xx:yy:yy:yy` (you|peer). The peer manager uses the swapped `yy:yy:yy:xx:xx:xx`.
- BFC_TUNNEL_DEVICE: airport `0`, and `upstream.size` must be 1.

The manager issues `set_upstream_rx <airport> <inject_port>` and `set_upstream_tx <airport> <local_ip> <forward_port>` for each entry. Inject and forward UDP ports are assigned automatically.

# Modes

| Mode | Local socket |
|------|----------------|
| `UDP_GENERIC_FORWARDING` | bind `rx`, send to `tx` |
| `UDP_SERVER_FORWARDING` | bind `bind_address`, reply to last sender |
| `UDP_CLIENT_FORWARDING` | send/recv `connect_address` |
| `TCP_SERVER_FORWARDING` | listen `bind_address` (one client) |
| `TCP_CLIENT_FORWARDING` | connect `connect_address` after the peer stream is up |

Host TCP receive for the TCP modes runs on a dedicated blocking-read thread per connection; bytes are queued onto the reactor for ARQ and radio inject so a full send window cannot stall ACK processing. Over the air the manager uses selective-repeat ARQ with cumulative ACK + SACK blocks (out-of-order DATA is buffered, only missing SNs are retransmitted).

`scheduler_budget` is the max bytes this upstream may inject per scheduler wakeup. `winject.max_rate_kbps` caps aggregate DATA inject rate (ACKs are not charged). If omitted or `0`, the default is 10000. The scheduler runs on a 500 µs timer and also immediately after radio RX / TCP ingest so reverse ACKs are not delayed a full tick.

# UDP FEC (`RS_BLOCK_ERASURE`)

Optional on UDP upstreams only. Groups `k` original datagrams and sends `n` on-air shards (systematic Cauchy Reed-Solomon via [ISA-L](https://github.com/intel/isa-l): NEON on aarch64, portable C elsewhere). The peer recovers the originals if any `k` of `n` shards arrive. Incomplete encode groups flush after `fec.timeout_ms` (default 20). Both managers must enable the same `k`/`n`. Firmware is unchanged; each shard is still one 802.11 body (max 1476 bytes, 8-byte FEC header). Not wire-compatible with `tools/fec.py` (different RS matrix).

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
upstream-0.mode = TCP_SERVER_FORWARDING
upstream-0.airport = 02:02:03:04:05:06
upstream-0.bind_address = 127.0.0.1:22022
upstream-1.mode = UDP_SERVER_FORWARDING
upstream-1.airport = 00:00:00:AA:BB:CC
upstream-1.bind_address = 127.0.0.1:22081
```

Host B (sshd side, video sink), swapped P2P airport on TCP:

```
winject.mode = STANDALONE
upstream-0.mode = TCP_CLIENT_FORWARDING
upstream-0.airport = 04:05:06:02:02:03
upstream-0.connect_address = 127.0.0.1:22
upstream-1.mode = UDP_CLIENT_FORWARDING
upstream-1.airport = 00:00:00:AA:BB:CC
upstream-1.connect_address = 127.0.0.1:21082
```

`ssh -p 22022 user@127.0.0.1` on A. GStreamer `udpsink` to A `:22081`, `udpsrc` on B `:21082`.

The manager fetches official [BFC](https://github.com/therooftopprinz/BFC) into the CMake build directory for INI parsing, sockets, and the epoll reactor.

Build: `cmake -S src/manager -B src/manager/build && cmake --build src/manager/build`. Run: `./src/manager/build/winject-manager src/manager/winject.conf.example`. Host tests: `pio run -t test`. Radio console commands are in [winject.md](winject.md).

# TCP bandwidth test (bw_test)

Use manager TCP forwarding with `tools/bw_test.py --tcp` (length-prefixed records over manager TCP; radios stay in `STANDALONE` with P2P airports).

```bash
chmod +x scripts/manager_tcp_bw_test.sh
./scripts/manager_tcp_bw_test.sh --modulation OFDM_24M
./scripts/manager_tcp_bw_test.sh --modulation OFDM_24M --kbps 1500 --no-cca
./scripts/manager_tcp_bw_test.sh 192.168.253.11 192.168.253.12 192.168.253.106 -- --bidir
```

Configs: `configuration/winject-tests/bw_a.cfg` (radio A) and `bw_b.cfg` (radio B). Host sends A→B via manager A `:29000`, receives on TCP `:9002`; B→A uses `:29001` and listen `:9001`. Managers use `winject.skip_console` so the script programs the radios first. Auto TCP offer is ~55% of the UDP estimate (original winject AM target). Prefer `--kbps 7000`–`8000` or `--no-cca` when measuring; if the air path is lossy, step down.

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

UDP configs: `configuration/winject-tests/lat_udp_a.cfg` / `lat_udp_b.cfg` (same P2P airports and ports as the TCP bw test). Decoder emits a block only after `k` shards; encoder waits for `k` datagrams or the FEC timeout.
