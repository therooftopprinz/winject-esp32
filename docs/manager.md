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
winject.max_rate_kbps = 10000

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

`scheduler_budget` is bytes this upstream may inject per 1 ms tick. `winject.max_rate_kbps` caps all inject traffic.

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

Build: `cmake -S manager -B manager/build && cmake --build manager/build`. Run: `./manager/build/winject-manager manager/winject.conf.example`. Radio console commands are in [winject.md](winject.md).
