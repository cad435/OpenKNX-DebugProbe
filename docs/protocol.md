# Interfaces

Legend: **[live]** = implemented and verified on real hardware.

For uploads the file content is always transferred as the **raw request body**
(`--data-binary`), never as multipart. That saves a multipart parser on the probe and works
equally well from a browser (`fetch(url, {method:"POST", body: file})`) and from `curl`.

## Discovery

mDNS hostname from the portal, default `openknx-probe-<mac>` (e.g.
`openknx-probe-xxxx.local`). Service `_http._tcp`, instance name
"OpenKNX DebugProbe". **[live]**

## TCP — the target's serial console **[live]**

| Port | Protocol | Used for |
|---|---|---|
| 2323 | raw, transparent | monitor, terminal |
| 4000 | RFC2217 (Telnet + COM-PORT-OPTION) | anything needing baud rate and DTR/RTS |

Both share the same 16 KB backlog and may be connected at the same time.

Behaviour on **2323**:

- On connect you first get a status line `[probe] …`, then the backlog — including the boot
  messages that appeared before you connected.
- If the target disappears (reset or flash), **the TCP connection stays up**; the state
  change appears as a `[probe]` line in the stream.
- One client at a time.

Behaviour on **4000**:

- Starts at the current end of the buffer, not at its beginning — a flashing tool should not
  find old console output in its protocol stream.
- Supported subnegotiations: `SET_BAUDRATE`, `SET_DATASIZE`, `SET_PARITY`, `SET_STOPSIZE`,
  `SET_CONTROL` (DTR/RTS), `SET_LINESTATE_MASK`, `SET_MODEMSTATE_MASK`, `PURGE_DATA`. BREAK
  is not passed through yet.
- 0xFF in the payload stream is doubled in both directions.

PC side:

```
monitor_port = socket://openknx-probe-xxxx.local:2323
esptool --port rfc2217://openknx-probe-xxxx.local:4000 write_flash ...
```

## HTTP API

### `GET /` **[live]**

Status and update page. In portal mode the provisioning form instead.

### `GET /api/status` **[live]**

```json
{
  "name": "openknx-probe-xxxx",
  "version": "1",
  "build": "Aug 19 2026 00:23:28",
  "partition": "ota_1",
  "pending_verify": false,
  "wifi": "connected",
  "ssid": "<SSID>",
  "rssi": -43,
  "ip": "<IP>",
  "uptime_s": 47,
  "heap_free": 2230844,
  "target":  { "state": "connected", "description": "…", "vid": "0x2E8A", … },
  "console": { "port": 2323, "client": "…", "rx": 12345, "rfc2217_port": 4000, … },
  "snippet": { "comment": "…", "monitor": "…", "upload": "…" }
}
```

`wifi`: `idle` | `connecting` | `connected` | `portal`
`pending_verify`: `true` while a freshly flashed image is still on probation.
`target` and `console` are absent while the USB host has not been started; the fields inside
`target` only exist in state `connected`.

Four fields inside `target` are the exception — they exist in every state, because the web UI
needs them to decide whether the restart buttons do anything at all:

| field | meaning |
|---|---|
| `can_reset` | a restart into the target's own application is possible |
| `reset_how` | the method used, or — when `can_reset` is `false` — the reason |
| `can_bootmode` | the target can be halted in its bootloader |
| `bootmode_how` | same, for the bootloader |

Both `*_how` fields are plain German text meant to be shown to a human. Do not parse them;
they are documentation, not a protocol.

One call is enough for the whole web UI page — it polls this single route.

### `POST /api/update` **[live]** — firmware of the **probe**

Raw body with the ESP32 application image. Writes into the inactive OTA slot and reboots
afterwards.

```powershell
curl.exe --fail-with-body --data-binary "@firmware/.pio/build/probe/firmware.bin" http://…/api/update
```

| Status | Meaning |
|---|---|
| 200 | written, reboot follows (`{"status":"ok","partition":"ota_1"}`) |
| 400 | not a valid ESP32 image, wrong project, or bad checksum — **nothing was written** |
| 409 | an update is already running |
| 413 | image larger than the OTA slot |

Checked before the first write: image magic `0xE9`, a present `esp_app_desc_t`, and a
matching project name. In particular this keeps a UF2 that was actually meant for the target
from ending up in the ESP32 by accident.

### `POST /api/fs` **[live]** — the web UI (filesystem image)

Writes the LittleFS image into the `storage` partition. The counterpart of `/api/update`:
that one carries the firmware, this one the interface. Body is the raw `littlefs.bin`.

```
curl.exe --fail-with-body --data-binary "@firmware/.pio/build/probe/littlefs.bin" http://openknx-probe-xxxx.local/api/fs
```

```json
{"status":"ok","written":196608,"total_kb":180,"used_kb":28,"index":true}
```

`index` is `false` when the image contains no `index.html` — the upload worked, but the probe
will still serve the built-in emergency page.

| code | meaning |
|---|---|
| 400 | empty body, too short, or not a LittleFS image |
| 409 | an upload is already running |
| 413 | image larger than the partition |
| 500 | erase, write, or remount failed |

**Why this endpoint exists at all.** Without it, every change to the interface needs a serial
`pio run -t uploadfs`. On a probe whose USB-C port is occupied by the target under test, that
means unplugging cables — the exact thing this project exists to abolish.

**The image is validated before anything is erased.** LittleFS carries the string `littlefs`
at offset 8 of its first block; that is checked the same way `/api/update` checks the ESP32
image magic. A `firmware.bin` sent here is refused rather than silently turned into an empty
interface.

**An aborted upload is not fatal.** The filesystem is unmounted for the duration, so a broken
transfer leaves the partition incomplete — the probe keeps running, serves the emergency page,
and this endpoint stays reachable. A second attempt repairs it. What cannot be repaired this
way is a probe that no longer joins the network; that is what the serial path is for.

### `GET /api/scan` **[live]**

List of visible WiFi networks, sortable by signal strength:
`[{"ssid":"…","rssi":-43,"open":false}, …]` (max. 20 entries).

### `POST /api/wifi/connect` **[live]**

`application/x-www-form-urlencoded` with `ssid`, `pass`, optionally `name` (device name).
Stores to NVS and reboots. Used by the captive portal.

### `POST /api/wifi/forget` **[live]**

Erases the stored credentials and reboots into the portal.

---

### `POST /api/flash` **[live]** — firmware of the **target**

Raw body with the UF2. It is buffered in PSRAM, **fully validated**, and only then written
as raw 512-byte sectors. Magics, the block chain, `numBlocks` and the family ID are checked;
if anything fails, not a single sector reaches the target.

If the target is still running its application, the endpoint pushes it into BOOTSEL
**itself** with a 1200-baud touch — only after validation, so a broken file cannot tear the
target out of its application. Nobody has to press a button. If the target already sits in
BOOTSEL, nothing is touched.

```powershell
curl.exe --fail-with-body --data-binary "@firmware.uf2" http://openknx-probe-xxxx.local/api/flash
```

| Status | Meaning |
|---|---|
| 200 | written (`{"blocks":…,"written":…}`), the target reboots |
| 400 | invalid UF2 — **nothing** on the target was changed |
| 409 | touch attempted, but BOOTSEL not reached (reason in `error`) |
| 413 | no memory for the file |
| 502 | write error midway through |

Every rejection names its reason in `{"error":…}`. Checks run in the order in which the
causes actually occur: **the magic at the start of the file first**, only then the size. The
other way round, the most common case — an accidentally sent `firmware.bin` — would get
"not a multiple of 512", and that points in the wrong direction.

```json
{"error":"kein UF2-Magic (0x0A324655) am Dateianfang, sondern 0x20003FFC, 572636 Byte
          empfangen - das sieht nach einer .bin aus. In der platformio.ini
          \"@$BUILD_DIR/${PROGNAME}.uf2\" statt \"@$SOURCE\" verwenden."}
```

The probe decides this after the first 512 bytes, **before** it requests memory for the
file. For the text to also show up in the PlatformIO log, the snippet needs
`--fail-with-body` instead of `-f`: with `-f`, curl swallows the body and all that remains
is `curl: (22)`.

`?validate_only=1` checks the file without writing and returns
`{"blocks":…,"payload_bytes":…,"first_address":…,"family":…,"status":"valid"}`.

The endpoint establishes BOOTSEL mode itself (1200-baud touch, see above) — **as long as the
target's firmware still enumerates**. Only when it no longer does, somebody has to step in:
BOOTSEL and RUN are accessible from the front panel, and the probe detects the state change
by itself. A power cycle over VBUS explicitly does not help there, because all OpenKNX
devices are KNX bus powered — see [hardware/](../hardware/).

### `GET /api/console?cursor=N` **[live]**

The target console's backlog for the browser. `cursor` is an **absolute** position in the
stream, not a ring-buffer index; without the parameter you get the entire available buffer.

```json
{ "cursor": 20480, "skipped": 0, "more": false, "data": "…" }
```

| Field | Meaning |
|---|---|
| `cursor` | use this value for the next request |
| `skipped` | this many bytes were overwritten before they were fetched |
| `more` | there is more waiting — fetch again immediately, without waiting |
| `data` | the raw bytes, JSON-escaped (control characters as `\u00XX`) |

At most 4 KB per response, so that a single response burdens neither the heap nor the
browser; hence the `more` flag.

The raw bytes stay raw: CR, LF and ANSI sequences pass through unchanged. The OpenKNX
console rewrites its prompt after every log line and erases it next time with `ESC[2K` — the
web UI runs a small line state machine for that (CR only moves the cursor, it erases
nothing).

`/api/status` carries `radio_awake` alongside: `true` while a client is attached to 2323 or
4000 and the radio therefore runs with `WIFI_PS_NONE`, otherwise `false`
(`WIFI_PS_MIN_MODEM`). HTTP itself does **not** keep the radio awake.

### `POST /api/console` **[live]** — command to the target

The body is the raw text **without** a line ending; `?eol=cr|lf|crlf|none` decides what gets
appended (default `cr`). Response `{"status":"sent","bytes":N}`.

Why it is selectable: the OpenKNX console works character by character — every keystroke is
processed and echoed immediately — while other firmware waits for a complete line. A
hard-coded line ending would break one case in order to serve the other. An empty body with
`eol=cr` sends just the line ending, which makes the prompt redraw.

The bytes take the same path as those of a TCP client on port 2323, so they are counted in
`bytesToTarget()` and appear in `/api/status` under `console.tx`. Anything longer than
512 bytes is rejected with `413` — this endpoint is for typed commands, not for files.

| Status | Meaning |
|---|---|
| 200 | written |
| 400 | unknown `eol`, or nothing to send |
| 409 | no serial session to the target is open |
| 413 | command longer than 512 bytes |
| 502 | writing to the target failed |

Verified on hardware against an OpenKNX fan actuator: `h` returns the complete help,
`uptime` the running time — both entered through the web console.

### `POST /api/reset` **[live]** — restart the target

Restarts the attached target into its own application. No body.

```json
{"status":"reset","method":"RTS-Puls auf EN (esptool-Hard-Reset)","msc_ready":false}
```

`method` names what the probe actually did, which differs per target — there is no single
mechanism that restarts every device over USB:

| target | reset into the application |
|---|---|
| UART bridge (CH34x / CP210x / FTDI) | RTS pulse on EN, the esptool hard reset |
| Espressif with native USB | RTS pulse through the USB-Serial-JTAG, after clearing the boot latch |
| RP2040 (CDC) | **not directly** — but bootmode followed by reset gets there via the boot ROM |
| RP2040 in BOOTSEL | PICOBOOT reboot over the boot ROM's vendor interface |

**A running RP2040 with native USB cannot be reset directly.** DTR and RTS end up in the
target's firmware, not at its RUN pin, and there is no line in between. The endpoint says so
instead of pretending: `409` with the reason in `error`. Ask `/api/status` beforehand —
`can_reset` carries the same answer without touching anything.

**It still works remotely, in two steps:** `POST /api/bootmode`, then `POST /api/reset`. The
first uses the 1200-baud touch, the second the boot ROM's PICOBOOT interface — the same one
`picotool reboot` drives. The route goes through the boot ROM, but it needs no cable. Once a
target sits in BOOTSEL, `/api/status` reports `picoboot: true` and `can_reset: true`.

### `POST /api/bootmode` **[live]** — halt the target in its bootloader

The counterpart. No body.

```json
{"status":"bootmode","method":"1200-Baud-Touch","msc_ready":true,"msc_kb":131071}
```

| target | into the bootloader |
|---|---|
| UART bridge | DTR/RTS sequence on IO0 and EN, the esptool bootloader reset |
| Espressif with native USB | DTR/RTS sequence through the USB-Serial-JTAG, setting the boot latch |
| RP2040 (CDC) | 1200-baud touch — the same thing `/api/flash` does internally |
| RP2040 in BOOTSEL | nothing to do, `method` says so |

Useful for testing the touch without overwriting the target's firmware, and as a button in
the web UI. On failure, `409` with the same `error` text as `/api/flash`.

An RP2040 returns to its application through a reset (RUN button or power cycle) or through
the next `POST /api/flash`. The flash contents are untouched by the touch.

**Both control-line sequences set DTR and RTS in a single USB control transfer.** The
intermediate state that pyserial produces over RFC2217 — one line at a time, roughly 50 ms
apart — never occurs here. That state is exactly what the auto-reset circuit on ESP32 boards
is built to ignore.

**On chips with a native USB-Serial-JTAG the same interlock lives inside the chip**, which
is why download mode is not selected by a pin level but by a latch: a phase with DTR
asserted is remembered, and the next reset then boots into download mode. The probe clears
that latch before a plain restart and sets it before a bootloader entry. Verified on an
ESP32-S3: `/api/reset` yields `boot:0x2b (SPI_FAST_FLASH_BOOT)`, `/api/bootmode` yields
`boot:0x23 (DOWNLOAD(USB/UART0))`.

### `POST /api/bootsel` **[live]** — deprecated alias

The old name of `/api/bootmode`, from when the only supported target was an RP2040. Same
handler, same response — including `status:"bootmode"`. New code should use `/api/bootmode`.

### `GET /api/snippet` **[live]**

The ready-made `platformio.ini` lines for the currently attached target, as `text/plain`.
Meant for scripts and AI agents: one call is enough to get the right configuration without
knowing the ports and baud rates yourself.

```
; OpenKNX DebugProbe - openknx-probe-xxxx.local
; Ziel: Raspberry Pi Pico (USB-CDC) (2E8A:000A)
monitor_port  = socket://openknx-probe-xxxx.local:2323
monitor_speed = 115200
upload_protocol = custom
upload_command  = curl --fail-with-body --data-binary "@$BUILD_DIR/${PROGNAME}.uf2" http://openknx-probe-xxxx.local/api/flash
```

Which upload path appears depends on the detected target: **VID `0x2E8A`** (RP2040) gets the
UF2 path over `/api/flash`, everything else `upload_port = rfc2217://…:4000` with
`upload_speed = 460800`. With no target attached only the two console lines remain, with
`; Ziel: kein Zielgeraet angesteckt` as the comment.

The same strings appear under `snippet` in `/api/status` and fill the two copy fields in the
web UI. They are built **in the firmware** (`buildSnippet()` in `main.cpp`) — that way the
endpoint and the display cannot drift apart when a port changes.

## PlatformIO integration in the target project **[live]**

```ini
[env:openknx_remote]
monitor_port    = socket://openknx-probe-xxxx.local:2323
upload_protocol = custom
upload_command  = curl --fail-with-body --data-binary "@$BUILD_DIR/${PROGNAME}.uf2" http://openknx-probe-xxxx.local/api/flash
```

Fully commented in `tools/platformio-snippet.ini`, which also has the `rfc2217://` variant
for ESP32 targets.

More convenient than copying it by hand is fetching both — the probe knows its own name and
the attached target:

```powershell
curl.exe http://openknx-probe-xxxx.local/api/snippet
```

The web UI shows the same lines in two copy fields.
