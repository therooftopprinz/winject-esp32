# winject-esp32

Firmware for the Wireless-Tag **WT32-ETH01** (ESP32 + LAN8720 Ethernet), built with PlatformIO and **ESP-IDF**.

This board is a **bfc-tunnel external multicast** radio: Ethernet UDP in/out, raw 802.11 monitor/inject on WiFi. See [docs/winject.md](docs/winject.md). It is not a copy of original WInject.

WiFi driver and the inject task run on **CPU0**. Ethernet, lwIP, the TCP console, and HTTP OTA run on **CPU1**.

## Layout

```
src/winject-esp32/          Firmware: console, upstream, settings, OTA, board config
src/winject-esp32/radio/    WiFi radio and 802.11 frame wrap/unwrap
src/winject-esp32/netmgr/   Ethernet PHY, DHCP client/server, IP manager
src/bfc-esp32/              ESP32 BFC subset (FreeRTOS / lwIP)
src/manager/                Host helper: programs the radio and bridges UDP/TCP apps
src/test/                   Google tests (frame wrap, manager config/stream)
sdkconfig.defaults          IDF options (core pin, buffers, OTA table)
partitions.csv              Two OTA app slots
platformio.ini              Board, framework, and serial settings
```

`pio run` builds `wt32-eth01`. Host unit tests: `pio run -t test` (or CMake under `src/test/`). Manager:

```
cmake -S src/manager -B src/manager/build && cmake --build src/manager/build
./src/manager/build/winject-manager src/manager/winject.conf.example
```

See [docs/manager.md](docs/manager.md), [docs/flashing.md](docs/flashing.md). Two-radio air tests: [docs/tests_wt32_eth01.md](docs/tests_wt32_eth01.md).

## Build and flash

The WT32-ETH01 has no USB port. Connect a 3.3 V USB-UART adapter to `TXD`/`RXD`/`GND`, hold `BOOT` (IO0) to GND, then apply power:

```powershell
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" run
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" run -t upload
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" device monitor
```

If PlatformIO is on your PATH, the same commands work as `pio run`, `pio run -t upload`, and `pio device monitor`.

Set `upload_port` in `platformio.ini` only if auto-detect picks the wrong COM port.

## Ethernet

Default PHY wiring matches the WT32-ETH01: LAN8720 at address `1`, MDC `23`, MDIO `18`, oscillator enable `16`, RMII clock `GPIO0` in. Hostname is set in `src/winject-esp32/config.h`. Wi-Fi and Ethernet MACs are the chip-unique factory addresses. If the link never comes up, set `ETH_CLK_MODE` to `ETH_CLK_GPIO17_OUT`.

Default Ethernet mode is `AUTO`: DHCP client, then static `192.168.32.1/24` if no lease in 5 seconds. The DHCP server is off until `set_enable_dhcp_server` and only runs in `STATIC`. WiFi stays in BFC/standalone monitor/inject. See [docs/winject.md](docs/winject.md).

## PlatformIO install (Windows)

Installed with `pip install -U platformio`. The executable lives at:

`C:\Users\mynam\AppData\Roaming\Python\Python311\Scripts`

That directory is not always on PATH. Add it, or invoke `platformio.exe` with the full path as shown above.
