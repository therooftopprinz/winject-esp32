# Flashing winject radios

This bench has **two** Ethernet board types. Wrong PlatformIO env →
`esp.emac: reset timeout` / dead Ethernet. Always pass `-e` explicitly.

| Radios (LAN IP) | Hardware | PlatformIO env | Boot log |
|-----------------|----------|----------------|----------|
| **`.9`**, **`.14`** | ESP32 + LAN8720 module | **`lan-module`** | `RMII REF_CLK GPIO17 out` |
| **`.11`**, **`.12`** | Stock **WT32-ETH01** | **`wt32-eth01`** | `RMII REF_CLK GPIO0 in` |

Confirm MAC before upload (see Cursor skill `esp32-bench-target`). Typical:
`.9` STA `20:50:0d:07:42:e8`; `.11` STA `20:50:0d:30:39:20`; `.14` ETH
`20:50:0d:08:0d:6b`.

```bash
pio run -e lan-module -t upload --upload-port /dev/ttyUSB0   # .9 / .14
pio run -e wt32-eth01 -t upload --upload-port /dev/ttyUSB0   # .11 / .12
```

OTA must use the matching `.pio/build/<env>/firmware.bin` for that radio.

---

## WT32-ETH01 UART wiring

The WT32-ETH01 has **no USB port**. Flash it over UART using the six pins at the WiFi-antenna end (`board = wt32-eth01` / env `wt32-eth01`).

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
4. Run `pio run -t upload`.
5. When esptool is writing, you can release IO0.
6. After upload, **release IO0** and power-cycle so it boots your firmware, not the ROM downloader.

Do not pull **IO2** high while programming, or **IO12** high at boot (wrong flash voltage).

This project sets `monitor_dtr = 0` and `monitor_rts = 0` so a plain adapter does not hold the chip in reset during serial monitor. Leave `EN` unwired unless your adapter already ties DTR/RTS to EN/IO0.

## Flash this firmware

```bash
pio run -e wt32-eth01          # or: -e lan-module for .9/.14
pio run -e wt32-eth01 -t upload --upload-port /dev/ttyUSB0
pio device monitor --port /dev/ttyUSB0
```

Host unit tests (Google Test) live under `src/test/` — not a PlatformIO env. Run them with:

```bash
pio run -t test
```

They cover 802.11 wrap/unwrap plus manager config and TCP stream logic; they do not replace two-radio air tests in [tests_wt32_eth01.md](tests_wt32_eth01.md).

On Windows without PlatformIO on PATH:

```powershell
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" run
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" run -t upload
& "$env:APPDATA\Python\Python311\Scripts\platformio.exe" device monitor
```

Notes:

- Upload speed is **57600** (external USB-UART on the WT32 is flaky at 115200).
- Leave `upload_port` unset unless auto-detect picks the wrong COM/tty. Then set it in `platformio.ini`, e.g. `/dev/ttyUSB0` or `COM3`.
- If it sits on `Connecting........`, IO0 was not low when power came up. Unplug power, hold IO0, plug power, retry.

## OTA (network)

After the board has an IPv4 address — Ethernet DHCP in `AUTO`, or the static address from `set_ip` — firmware can be updated without UART. A missed DHCP lease falls back to static `192.168.32.1/24` after 1.5 s (configure the host on that `/24` if needed):

```bash
# .11 / .12 (WT32):
curl -F "firmware=@.pio/build/wt32-eth01/firmware.bin;filename=firmware.bin" \
  http://192.168.253.11/update
# .9 / .14 (LAN module) — only after ETH is already up with lan-module:
curl -F "firmware=@.pio/build/lan-module/firmware.bin;filename=firmware.bin" \
  http://192.168.253.9/update
```

Use the Ethernet IP when the cable has a DHCP lease. HTTP `GET /` is the upload form; `POST /update` accepts the firmware blob (multipart from the form or `curl -F`). The partition table is `partitions.csv` (two OTA app slots). There is no ArduinoOTA / UDP 3232. Never OTA a `wt32-eth01` binary onto `.9`/`.14` (or the reverse).

## After it boots

Ethernet PHY wiring is in `src/winject-esp32/config.h` (LAN8720, addr 1, MDC 23, MDIO 18, oscillator enable 16). RMII REF_CLK is selected by PlatformIO env: `pio run -e wt32-eth01` (GPIO0 in) or `pio run -e lan-module` (GPIO17 out).
