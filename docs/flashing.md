# Flashing the WT32-ETH01

The WT32-ETH01 has **no USB port**. Flash it over UART using the six pins at the WiFi-antenna end, then upload this project with PlatformIO (`board = wt32-eth01` in `platformio.ini`).

Hardware notes come from the unofficial [wt32-eth01](https://github.com/egnor/wt32-eth01) guide.

## Wiring

Use a **3.3 V TTL** USB-UART adapter (not 5 V, not RS-232). Cross RX/TX:

| USB-UART | WT32-ETH01 |
|---|---|
| RX | IO1 (`TXD`) |
| TX | IO3 (`RXD`) |
| GND | GND |
| 3.3 V **or** 5 V | matching `3.3V` or `5V` pin — **not both** |

Those six top pins are the programming interface. Do not supply 3.3 V and 5 V at the same time. The onboard regulator is not a true AMS1117; keep 5 V input at about 5–6 V.

## Enter download mode (power-on)

The ESP32 starts the ROM downloader when **IO0 is low at the moment power (or EN) goes high**.

1. Unplug WT32 power (`5V` / `3.3V`).
2. Hold **IO0 (`BOOT`)** to GND.
3. Apply power. Keep IO0 held.
4. Run `pio run -t upload` (or `pio run -e test -t upload`).
5. When esptool is writing, you can release IO0.
6. After upload, **release IO0** and power-cycle so it boots your firmware, not the ROM downloader.

Do not pull **IO2** high while programming, or **IO12** high at boot (wrong flash voltage).

This project sets `monitor_dtr = 0` and `monitor_rts = 0` so a plain adapter does not hold the chip in reset during serial monitor. Leave `EN` unwired unless your adapter already ties DTR/RTS to EN/IO0.

## Flash this firmware

```bash
pio run
pio run -t upload
pio device monitor
```

Bring-up image instead of winject:

```bash
pio run -e test -t upload
```

On Windows without PlatformIO on PATH:

```powershell
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" run
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" run -t upload
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" device monitor
```

Notes:

- Upload speed is **115200**.
- Leave `upload_port` unset unless auto-detect picks the wrong COM/tty. Then set it in `platformio.ini`, e.g. `/dev/ttyUSB0` or `COM3`.
- If it sits on `Connecting........`, IO0 was not low when power came up. Unplug power, hold IO0, plug power, retry.

## OTA (network)

After the board has an IPv4 address — Ethernet DHCP, and/or the open AP `winject-<MAC>` at `192.168.4.1` if DHCP did not arrive in 5 s — firmware can be updated without UART:

```bash
curl -F "firmware=@.pio/build/wt32-eth01/firmware.bin" http://192.168.4.1/update
```

Use the Ethernet IP when the cable has a lease. HTTP `GET /` is the upload form; `POST /update` accepts the firmware blob (multipart from the form or `curl -F`). The partition table is `partitions.csv` (two OTA app slots). There is no ArduinoOTA / UDP 3232.

## After it boots

Ethernet PHY wiring is already set for this board in `include/config.h` (LAN8720, addr 1, MDC 23, MDIO 18, oscillator enable 16, clock on GPIO0). If the Ethernet link never comes up, try `ETH_CLK_GPIO17_OUT` instead of `ETH_CLK_GPIO0_IN`.
