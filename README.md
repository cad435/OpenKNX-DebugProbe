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
| Target detection | RP2040, ESP32, CH32V003, CH34x/CP210x/FTDI — including serial number |
| Console | `socket://…:2323` with a 16 KB backlog, survives target resets |
| Control lines | `rfc2217://…:4000` with baud rate and DTR/RTS |
| Firmware to the target | UF2 over HTTP, fully validated, then written over USB MSC |
| Getting into BOOTSEL | 1200-baud touch, issued by the probe itself — no button press |
| Commands to the target | input line in the browser, selectable line ending, history with ↑/↓ |
| Power saving | the radio dozes while no client is attached to 2323 or 4000 |
| Console in the browser | follow along without a terminal; prompt and ANSI are resolved |
| Ready-made configuration | the `platformio.ini` lines as a copy field and via `/api/snippet` |
| Restarting the target | `/api/reset` and `/api/bootmode`, two buttons in the UI — the probe decides what a given target actually supports and says so |
| Firmware to a CH32V003 | `/api/ch32/flash`, over the rv003usb bootloader's own protocol — no minichlink on the PC |
| Web UI outside the image | in a LittleFS partition, replaceable over WiFi with `/api/fs` |
| Why the probe last started | `reset_reason` tells a brownout from a crash — a target with a high inrush can reset the probe |
| Cell voltage | measured through a divider on the adapter board, reported as `battery` in `/api/status` |

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

## Supported targets and their USB IDs

**Read the caveat before the table.** On targets with native USB, the VID:PID of the
*running application* is chosen by its firmware, not by the chip. The values below are the
common defaults, not a guarantee. Only the **bootloader** IDs are fixed — they live in the
boot ROM or in a bootloader that is flashed once and then left alone.

That asymmetry is why the probe identifies bootloaders by VID:PID and applications by their
USB class: the first is reliable, the second is not.

| Target | Application (typical) | Bootloader / download | How the probe gets there |
|---|---|---|---|
| RP2040 (arduino-pico) | `2E8A:00C0` | `2E8A:0003` RPI-RP2, mass storage | 1200-baud touch |
| RP2040 (pico-sdk) | `2E8A:000A`, `2E8A:0009` | `2E8A:0003` | 1200-baud touch |
| ESP32-S3 / -C3, native USB | `303A:1001` | **same IDs** — the ROM uses the same USB-Serial-JTAG | DTR/RTS sequence with boot latch |
| ESP32-S2, native USB | `303A:0002`, `303A:1000` | `303A:0002` | DTR/RTS sequence |
| Any board behind a UART bridge | the bridge chip, see below | **unchanged** — the bridge stays visible either way | DTR/RTS on IO0 and EN |
| CH32V003 with rv003usb | firmware's choice (`1209:D003` in the example) | `1209:B003` | HID feature report `0xAB` |

UART bridges, for completeness — these never change between application and bootloader,
because the mode is selected with DTR/RTS rather than by re-enumerating:

`1A86:7523` CH340 · `1A86:5523` CH341 · `1A86:55D3` CH343 · `1A86:55D4` CH9102 ·
`10C4:EA60` CP2102 · `10C4:EA70` CP2105 · `10C4:EA71` CP2108 · `0403:6001` FT232R ·
`0403:6010` FT2232 · `0403:6014` FT232H · `0403:6015` FT230X · `067B:2303` PL2303

### What the probe can do with each

**✅ means verified on real hardware**, not merely implemented. Where a row says
*untested*, the code is there and follows the reference implementation — but nothing has
confirmed it yet, and this table would be worth little if it blurred that line.

| Target | Console | Flash | Restart into the application | Into the bootloader |
|---|---|---|---|---|
| RP2040 (Pico) | ✅ | ✅ UF2 over `/api/flash` | ✅ PICOBOOT, from BOOTSEL only | ✅ 1200-baud touch |
| ESP32-S3 / -C3, native USB | ✅ | ✅ esptool over RFC2217 | ✅ RTS pulse | ✅ DTR/RTS with boot latch |
| CH32V003 (rv003usb bootloader) | — | ✅ `/api/ch32/flash` | ✅ starts the application | ✅ HID feature report `0xAB` |
| ESP32 behind a UART bridge | ✅ | esptool over RFC2217 | *untested* | *untested* |
| Any other HID device | — | — | — | the `0xAB` attempt; a stall is reported as a refusal |

**A running RP2040 cannot be reset over the cable.** Native USB has no reset line, and
PICOBOOT lives only in the boot ROM. Bootmode followed by reset gets there anyway — through
the boot ROM, two clicks, no cable.

**The CH32V003 flasher is written for the V003 specifically.** Sector size and the flash
controller addresses are fixed in the code. Chip *detection* also knows V002/004/005/006/007
and X033, but flashing them is not claimed — a different sector size would have to be taught
first. The scratchpad negotiation and the choice of boot routine already handle the larger
bootloaders, so that part is done.

## Status LED

The module's RGB LED (WS2812 on GPIO48) shows the operating state. Highest urgency wins —
`flashing` beats everything, because pulling the cable there does real damage.

| State | LED | Trigger |
|---|---|---|
| `flashing` | orange, blinking at 320 ms | `POST /api/flash` is writing — **do not unplug** |
| `battery_low` | red, blinking at 1 s | cell nearly empty |
| `no_wifi` | red, pulsing | not connected to WiFi, including portal mode |
| `busy` | solid yellow | a client is attached to 2323 or 4000 |
| `target_ready` | solid green | a target is detected |
| `idle` | green, pulsing | ready, no target attached |


The current mode is reported as `led.mode` in `/api/status`.

Brightness is capped well below maximum. A WS2812 at full white draws around 60 mA, which
matters on battery.

Two more LEDs sit on the SuperMini:

| LED | Meaning |
|---|---|
| blue | the Li-ion charger: on while charging, off once the cell is full, blinking when no cell is connected. |
| red | **shares GPIO48 with the RGB LED.** It lights while the pin is left alone and stays dark as soon as the firmware drives the WS2812 — so on this firmware it is permanently off. |

The shared pin was confirmed on hardware: moving the status LED to another pin brings the
red one back, moving it to GPIO48 turns it off again. It is therefore not a power indicator,
and the RGB LED costs you the red one. If you would rather keep the red LED, point the
status LED at a free pin with `POST /api/led?gpio=N` — the value is stored in NVS.

## Battery

On the adapter board a 1M/1M divider sits behind the battery switch and feeds GPIO5
(ADC1). `/api/status` reports it, and the UI shows a "Zelle" row:

```json
"battery": { "available": true, "level": "normal", "settled": true,
             "mv": 3910, "pin_mv": 1955, "percent": 67, "trim": 1000 }
```

| Field | Meaning |
|---|---|
| `level` | `absent`, `normal`, `low` (< 3.55 V), `critical` (< 3.30 V), or `unknown` while warming up |
| `settled` | false for the first ~25 s after boot, see below |
| `mv` / `pin_mv` | cell voltage and the raw voltage at the divider tap — if the two disagree by anything other than the factor of two, the divider is not what you think it is |
| `percent` | a rough gauge, not a measurement: a generic Li-ion curve over 13 support points, linearly interpolated, with no load compensation — at 30-120 mA the cell sits well below 0.1 C, so the IR drop is small against the spread of the curve itself. It reads 0 % at 3.30 V, the same point as `critical`, because the module's LDO gives up somewhere between 3.0 and 3.5 V; capacity below that is not usable here |

Two things worth knowing:

- **`absent` is the normal case on USB.** The divider is behind the battery switch, so
  with the switch off — or no cell at all — the tap sits at ground. That is not a fault.
- **The first ~25 s after boot report `unknown`.** The tap carries 10 µF against a 500 kΩ
  source impedance, so it charges with a 5 s time constant and reads low until it has
  settled. Without that hold-off the probe would announce an empty cell at every start.

If the reading disagrees with a multimeter, trim it once: measure at the cell and post
the value.

```bash
curl -X POST "http://openknx-probe-xxxx.local/api/battery/calibrate?mv=3870"
```

The factor is stored in NVS and applied from then on. 1 % resistors alone allow 2 % on the
divider ratio, and the ADC characteristic adds to that — around 100 mV at 3.8 V, enough to
move the low-battery warning by half an hour. Corrections beyond 25 % are rejected.

## PlatformIO integration

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
   named after itself, e.g. `OpenKNX-Probe-A1B2`.
2. Connect to it — the captive portal opens by itself, otherwise `http://192.168.4.1/`.
3. Pick a network and enter the password. The device name is not configurable: it is
   always `OpenKNX-Probe-<last two MAC bytes>`, so it is unique per board and matches
   the setup network you just joined.
4. After the restart the probe is reachable at `http://<name>.local/`.

The probe remembers **one** network. Entering another one replaces it. Also note that the
fallback AP only ever opens during startup: once the probe has an address, a link that
gets weak or drops is retried indefinitely and never falls back to the portal. To reach
the portal on purpose, restart the probe while its network is out of reach.

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
| [hardware/](hardware/) | probe module, power supply, battery, VBUS |
| [docs/protocol.md](docs/protocol.md) | HTTP API and TCP serial interface |

`docs/architecture.md` is currently German only.

## License

[GNU General Public License v3.0 or later](LICENSE) (`GPL-3.0-or-later`).

Espressif's USB host and mDNS components, which the build pulls in through the Component
Manager, are Apache-2.0 licensed and are not part of this repository.
