# Rover use of this radio

This repository is the **WT32-ETH01 radio** (firmware + `winject-manager`).
The rover vehicle is a different repo: `/home/ubuntu/development/rover`.

## What this radio is

On both the Orange Pi 5 (ground station) and the Orange Pi PC (rover host), a
WT32-ETH01 sits on a dedicated Ethernet (`192.168.32.1` radio, `192.168.32.2`
host). `winject-manager` programs it over the **TCP control console `:2323`**,
then bridges UDP.

Payloads on the air are opaque. Firmware does not know WireGuard, H264, or
drive-console text.

## What this radio is not

| Not | That lives in |
|-----|----------------|
| Rover **slave** (WROOM-32 GPIO expander) | `rover/src/` |
| Rover **master** (H3 UART↔UDP proxy) | `rover/master/` |
| Drive console (`servo` / `motor` / `ping`) | Slave UART + UDP through manager, `rover/docs/console.md` |

Do **not** add `servo` / `motor` / `drive` to `console.cpp`. Vehicle messages are
a UDP upstream, not this TCP console.

The TCP console remains radio/network/upstream only. Canonical command list:
[winject.md](winject.md) § Control Plane Console.

## Planned drive-console upstream

When rover enables the proxy, each manager gains a fourth UDP upstream,
**no FEC**. Payloads are drive messages (`rover/docs/messages.md`: human line
or binary frame, first byte). The GS controller is out of scope.

| Side | Mode | Buses tx/rx | Host |
|------|------|-------------|------|
| Rover H3 | `UDP_SERVER_FORWARDING` | `72` / `81` | bind `127.0.0.1:22090` |
| Ground station | `UDP_CLIENT_FORWARDING` | `81` / `72` | connect `127.0.0.1:21090` |

Domain stays `1234`. Existing WG (`a1`/`b2`) and camera (`c3`/`d4`, `e5`/`f6`)
pairs must not be reused. Full contract: `rover/docs/architecture.md` and
`rover/docs/rc.md`.

## Two ESP32s on the rover

```
H3  --eth-->  WT32-ETH01 (this repo)  --802.11-->  GS radio
H3  --UART2-->  ESP32-WROOM-32 (slave GPIO; master is a proxy)
```

UART0 on the WT32-ETH01 is **logs**, 115200, not a command console. UART0 on
the WROOM-32 is the **drive console**.
