# OpenKNX DebugProbe

A portable WiFi debugger built on an ESP32-S3 that services an OpenKNX device (RP2040 or
ESP32) through a **single USB-C cable**: **firmware updates** and a **serial console over
TCP**.

![Overview: PC connects to the probe over WiFi, the probe connects to the OpenKNX device with one USB-C cable](docs/overview.png)

## Why

OpenKNX REG1 devices live inside a distribution cabinet. Until now, a firmware update or a
look at the debug console meant carrying a laptop to the cabinet. The probe stays plugged
into the device and is reached over WiFi.

It is built specifically for OpenKNX devices but is **not part of the OpenKNX project** — a
private tool, for now with no affiliation to the OpenKNX group.

## Status

**Working and verified on real hardware.** The whole development cycle runs over WiFi:

| | |
|---|---|
| WiFi setup | captive portal, 3 connection attempts, then a fallback AP |
| Self-update | HTTP OTA with A/B slots and automatic rollback |
| Target detection | RP2040, ESP32, STM32, CH34x/CP210x/FTDI — including serial number |
| Console | `socket://…:2323` with a 16 KB backlog, survives target resets |
| Control lines | `rfc2217://…:4000` with baud rate and DTR/RTS |
| Firmware to the target | UF2 over HTTP, fully validated, then written over USB MSC |
| Getting into BOOTSEL | 1200-baud touch, issued by the probe itself — no button press |
| Commands to the target | input line in the browser, selectable line ending, history with ↑/↓ |
| Power saving | the radio dozes while no client is attached to 2323 or 4000 |
| Console in the browser | follow along without a terminal; prompt and ANSI are resolved |
| Ready-made configuration | the `platformio.ini` lines as a copy field and via `/api/snippet` |

Architecture, hardware and interfaces in detail: **[docs/](docs/)**.

## Modes of operation

- **Normal operation:** `usb_host_cdc_acm` reads the target's console, a TCP server on port
  2323 forwards it to the PC.
- **Flashing:** 1200-baud touch → target reboots into BOOTSEL → `usb_host_msc` writes the
  UF2 as raw 512-byte sectors → reset. `POST /api/flash` issues the touch itself, **after**
  the UF2 has been fully validated; nobody has to press a button on the device. If the mass
  storage device does not appear, one retry follows, then a `409` with a reason — and the
  console is put back to 115200 with DTR/RTS at their idle levels.

The radio saves power when nobody is listening: while a client is attached to 2323 or 4000
it runs with `WIFI_PS_NONE`, otherwise it dozes in `MIN_MODEM`. The console livestream and
flashing are demonstrably unaffected, while the ESP32-S3 runs noticeably cooler when idle.
Only an accepted TCP client gets a vote: HTTP, the web UI and API calls do **not** keep the
radio awake. So an open browser tab costs nothing, a forgotten `pio device monitor` does.
The current state is reported as `radio_awake` in `/api/status` and as the "Funk" row in the
web UI.

The web UI does not only follow the console, it also has a line for **sending commands** to
the target. The line ending is selectable (CR, LF, CRLF, none), because the OpenKNX console
works character by character while other firmware waits for a complete line. ↑/↓ steps
through recently sent commands.

## Status LED

The module's RGB LED (WS2812 on GPIO48) shows the operating state. Highest urgency wins —
`flashing` beats everything, because pulling the cable there does real damage.

| State | LED | Trigger |
|---|---|---|
| `flashing` | orange, blinking at 320 ms | `POST /api/flash` is writing — **do not unplug** |
| `battery_low` | red, blinking at 1 s | cell nearly empty — **not implemented yet**, see below |
| `no_wifi` | red, pulsing | not connected to WiFi, including portal mode |
| `busy` | solid yellow | a client is attached to 2323 or 4000 |
| `target_ready` | solid green | a target is detected |
| `idle` | green, pulsing | ready, no target attached |


Brightness is capped well below maximum. A WS2812 at full white draws around 60 mA, which
matters on battery.

No extra hardware is needed on the PC side — a few lines in the target project's
`platformio.ini` are enough. The probe hands them out itself, since it knows its own name
and what is plugged in:

```powershell
curl.exe http://openknx-probe-xxxx.local/api/snippet
```

The web UI shows the same lines in two copy fields as soon as a target is detected. If you
would rather write them yourself:

```ini
; RP2040 target (UF2 over HTTP)
monitor_port    = socket://openknx-probe-xxxx.local:2323
upload_protocol = custom
upload_command  = curl --fail-with-body --data-binary "@$BUILD_DIR/${PROGNAME}.uf2" http://openknx-probe-xxxx.local/api/flash

; ESP32 target (esptool over RFC2217)
upload_port     = rfc2217://openknx-probe-xxxx.local:4000
upload_speed    = 460800
```

Fully commented in [tools/platformio-snippet.ini](tools/platformio-snippet.ini).

## Build and flash

```powershell
cd firmware
pio run              # build
pio run -t upload    # flash (the port is detected automatically)
```

PlatformIO with `framework = espidf`, platform espressif32 54.3.21 (ESP-IDF 5.4.2). A
separate ESP-IDF installation is not required.

## First-time setup

1. Flash the firmware. The probe finds no credentials and opens an open WiFi network
   `openknx-probe-<mac>`.
2. Connect to it — the captive portal opens by itself, otherwise `http://192.168.4.1/`.
3. Pick a network, enter the password, optionally assign your own device name (important
   when several probes are in use).
4. After the restart the probe is reachable at `http://<name>.local/`.

## Self-update

From the browser via the start page, or:

```powershell
curl.exe --fail-with-body --data-binary "@firmware/.pio/build/probe/firmware.bin" `
         http://openknx-probe-xxxx.local/api/update
```

A foreign or damaged image is rejected with HTTP 400 before anything is written. If a new
image does not boot cleanly, the bootloader falls back to the previous one automatically.

## Security

The probe has **no authentication**. Anyone who can reach it on the network can, with no
further hurdle,

- write firmware into the attached target (`POST /api/flash`),
- overwrite the probe itself over OTA (`POST /api/update`),
- send commands to the target's console (`POST /api/console`),
- push the target into BOOTSEL (`POST /api/bootsel`), and
- erase the stored WiFi credentials (`POST /api/wifi/forget`).

The setup AP is **open** as well, without WPA — during first-time setup anyone within radio
range can set the credentials.

This is deliberate: the probe is a tool for the bench and your own development network,
where turnaround time matters and a login on every flash would only get in the way. But it
follows where it does **not** belong: not on a guest network, not on a network with
strangers on it, and never behind a port forward or a reverse proxy exposed to the
internet. If it stays in the cabinet permanently, it belongs on the same trusted network as
the devices it is allowed to flash — because that is exactly what anyone who reaches it can
do.

## Documentation

| File | Contents |
|---|---|
| [docs/architecture.md](docs/architecture.md) | state machine, tasks, core split |
| [hardware/hardware.md](hardware/hardware.md) | probe module, power supply, battery, VBUS |
| [docs/protocol.md](docs/protocol.md) | HTTP API and TCP serial interface |

`docs/architecture.md` is currently German only.

## License

[GNU General Public License v3.0 or later](LICENSE) (`GPL-3.0-or-later`).

Espressif's USB host and mDNS components, which the build pulls in through the Component
Manager, are Apache-2.0 licensed and are not part of this repository.
