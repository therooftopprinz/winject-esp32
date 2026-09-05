# Manager files-only restructure

Rename and folder-layout host manager sources to mirror original winject roles. No wire format, INI schema, or runtime behavior changes. Binary stays `winject-manager`.

```
EP (UDP/TCP) → (FEC | Llc ARQ) → TxScheduler → WifiUdp → ESP32
                                    ↑
                                 AppRrc (+ ConsoleClient)
```

## Layout

```
src/manager/
  main.cpp
  app_rrc.{h,cpp}          # AppRRC
  illc.h                   # ILLC (scheduler-facing)
  tx_scheduler.{h,cpp}     # TxScheduler
  wifi_udp.{h,cpp}         # WIFIUDP
  udp_endpoint.{h,cpp}     # UDPEndPoint
  tcp_endpoint.{h,cpp}     # TCP server/client EndPoint (one class)
  console_client.*         # ESP32 console (host helper)
  config.*  net_util.*  reactor.h  log.h
  llc/
    llc.{h,cpp}            # LLC AM ARQ (was tcp_stream)
  frames/
    basic_fec.{h,cpp}      # frames/basic_fec (was fec)
```

Includes use path prefixes like original: `"llc/llc.h"`, `"frames/basic_fec.h"`. Root stays on the include path.

## Name map

| Old file(s) | New file(s) | New type | Original analogue |
|-------------|-------------|----------|-------------------|
| `app.{h,cpp}` | `app_rrc.{h,cpp}` | `AppRrc` | `AppRRC` |
| `scheduler.{h,cpp}` | `tx_scheduler.{h,cpp}` | `TxScheduler` | `TxScheduler` |
| `upstream.h` | `illc.h` | `ILlc` (+ `StreamStats`) | `ILLC` |
| `tcp_stream.{h,cpp}` | `llc/llc.{h,cpp}` | `Llc` | `LLC` (AM ARQ) |
| `udp_endpoint.{h,cpp}` | same | `UdpEndPoint` | `UDPEndPoint` |
| `tcp_endpoint.{h,cpp}` | same | `TcpEndPoint` | TCP EndPoint (one class) |
| `radio_udp.{h,cpp}` | `wifi_udp.{h,cpp}` | `WifiUdp` | `WIFIUDP` |
| `fec.{h,cpp}` | `frames/basic_fec.{h,cpp}` | `RsBlockErasure` | `frames/basic_fec` |

Snake_case filenames (repo Google style). No PDCP layer. No split of `TcpEndPoint` into server vs client files.

`UpstreamConfig` / `UpstreamMode` in `config.h` stay — those name the INI `upstream-N` radio bindings, not the `ILlc` interface.

## Tests

| Old | New |
|-----|-----|
| `tcp_stream_test.cpp` | `llc_test.cpp` (`Llc`, `LlcTest`) |
| `fec_test.cpp` | `basic_fec_test.cpp` |
