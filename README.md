# winject-esp32

Firmware for the Wireless-Tag **WT32-ETH01** (ESP32 + LAN8720 Ethernet), built with PlatformIO and the Arduino framework.

## Layout

```
include/          Public headers and board config
lib/              Project-local libraries (optional)
src/              Firmware sources
test/             PlatformIO unit tests (optional)
platformio.ini    Board, framework, and serial settings
```

## Build and flash

The WT32-ETH01 has no USB port. Connect a 3.3 V USB-UART adapter to `TXD`/`RXD`/`GND`, hold `BOOT` (IO0) while resetting to enter download mode, then:

```powershell
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" run
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" run -t upload
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" device monitor
```

If PlatformIO is on your PATH, the same commands work as `pio run`, `pio run -t upload`, and `pio device monitor`.

Set `upload_port` in `platformio.ini` only if auto-detect picks the wrong COM port.

## Ethernet

Default PHY wiring matches the WT32-ETH01: LAN8720 at address `1`, MDC `23`, MDIO `18`, oscillator enable `16`, RMII clock `GPIO0` in. Hostname and MAC are set in `include/config.h`. If the link never comes up, switch `ETH_CLK_MODE` to `ETH_CLOCK_GPIO17_OUT`.

## PlatformIO install (Windows)

Installed with `pip install -U platformio`. The executable lives at:

`C:\Users\mynam\AppData\Roaming\Python\Python311\Scripts`

That directory is not always on PATH. Add it, or invoke `platformio.exe` with the full path as shown above.
