# Schnittstellen

Legende: **[live]** = implementiert und am Gerät verifiziert, **[geplant]** = Zielbild,
**[verworfen]** = bewusst nicht gebaut, mit Begründung.

Bei Uploads wird der Dateiinhalt immer als **roher Request-Body** übertragen
(`--data-binary`), nicht als Multipart. Das spart einen Multipart-Parser auf der Probe
und funktioniert aus Browser (`fetch(url, {method:"POST", body: file})`) wie aus `curl`
gleichermaßen.

## Auffindbarkeit

mDNS-Hostname aus dem Portal, Default `openknx-probe-<mac>` (z. B.
`openknx-probe-xxxx.local`). Service `_http._tcp`, Instanzname
"OpenKNX DebugProbe". **[live]**

## TCP — serielle Konsole des Zielgeräts **[live]**

| Port | Protokoll | wofür |
|---|---|---|
| 2323 | roh, transparent | Monitor, Terminal |
| 4000 | RFC2217 (Telnet + COM-PORT-OPTION) | alles mit Baudrate und DTR/RTS |

Beide teilen sich denselben 16-KB-Mitschnitt und können gleichzeitig verbunden sein.

Verhalten auf **2323**:

- Beim Verbinden erst eine Statuszeile `[probe] …`, dann der Mitschnitt — also auch die
  Boot-Meldungen, die vor dem Verbinden aufliefen.
- Verschwindet das Ziel (Reset/Flash), **bleibt die TCP-Verbindung bestehen**; der
  Zustandswechsel erscheint als `[probe]`-Zeile im Strom.
- Ein Client gleichzeitig.

Verhalten auf **4000**:

- Startet beim aktuellen Ende des Puffers, nicht bei dessen Anfang — ein Flash-Werkzeug
  soll keine alten Konsolenausgaben in seinem Protokollstrom finden.
- Unterstützte Subnegotiationen: `SET_BAUDRATE`, `SET_DATASIZE`, `SET_PARITY`,
  `SET_STOPSIZE`, `SET_CONTROL` (DTR/RTS), `SET_LINESTATE_MASK`, `SET_MODEMSTATE_MASK`,
  `PURGE_DATA`. BREAK wird noch nicht durchgereicht.
- 0xFF im Nutzdatenstrom wird in beide Richtungen verdoppelt.

PC-Seite:

```
monitor_port = socket://openknx-probe-xxxx.local:2323
esptool --port rfc2217://openknx-probe-xxxx.local:4000 write_flash ...
```

## HTTP-API

### `GET /` **[live]**

Status- und Update-Seite. Im Portal-Modus stattdessen das Provisioning-Formular.

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
`pending_verify`: `true`, solange ein frisch geflashtes Image noch auf Probation ist.
`target` und `console` fehlen, solange der USB-Host nicht gestartet ist; die Felder
innerhalb von `target` gibt es nur im Zustand `connected`.

Ein Aufruf reicht dem Web-UI für die ganze Seite — es pollt nur diese eine Route.

### `POST /api/update` **[live]** — Firmware der **Probe**

Roher Body mit dem ESP32-Anwendungsimage. Schreibt in den inaktiven OTA-Slot und startet
danach neu.

```powershell
curl.exe --fail-with-body --data-binary "@firmware/.pio/build/probe/firmware.bin" http://…/api/update
```

| Status | Bedeutung |
|---|---|
| 200 | geschrieben, Neustart folgt (`{"status":"ok","partition":"ota_1"}`) |
| 400 | kein gültiges ESP32-Image, falsches Projekt oder Prüfsumme falsch — **nichts geschrieben** |
| 409 | es läuft bereits ein Update |
| 413 | Image größer als der OTA-Slot |

Geprüft wird vor dem ersten Schreibzugriff: Image-Magic `0xE9`, vorhandener
`esp_app_desc_t` und übereinstimmender Projektname. Damit landet insbesondere keine UF2,
die eigentlich fürs Zielgerät gedacht war, versehentlich im ESP32.

### `GET /api/scan` **[live]**

Liste der sichtbaren WLANs, nach Signalstärke sortierbar:
`[{"ssid":"…","rssi":-43,"open":false}, …]` (max. 20 Einträge).

### `POST /api/wifi/connect` **[live]**

`application/x-www-form-urlencoded` mit `ssid`, `pass`, optional `name` (Gerätename).
Speichert in NVS und startet neu. Wird vom Captive Portal benutzt.

### `POST /api/wifi/forget` **[live]**

Löscht die gespeicherten Zugangsdaten und startet ins Portal.

---

### `POST /api/flash` **[live]** — Firmware des **Zielgeräts**

Roher Body mit der UF2. Wird im PSRAM gepuffert, **vollständig validiert** und erst dann
in rohen 512-Byte-Sektoren geschrieben. Geprüft werden Magics, Blockkette, `numBlocks`
und Family-ID; schlägt etwas fehl, geht kein einziger Sektor ans Ziel.

Läuft das Ziel noch seine Anwendung, schickt der Endpunkt es **selbst** per
1200-Baud-Touch nach BOOTSEL — erst nach der Validierung, damit eine kaputte Datei das
Ziel nicht aus seiner Anwendung reisst. Taster drücken muss dafür niemand. Steht das Ziel
schon im BOOTSEL, wird nichts angefasst.

```powershell
curl.exe --fail-with-body --data-binary "@firmware.uf2" http://openknx-probe-xxxx.local/api/flash
```

| Status | Bedeutung |
|---|---|
| 200 | geschrieben (`{"blocks":…,"written":…}`), Ziel startet neu |
| 400 | UF2 ungültig — es wurde **nichts** am Ziel verändert |
| 409 | Touch versucht, aber kein BOOTSEL erreicht (Grund im `error`) |
| 413 | kein Speicher für die Datei |
| 502 | Schreibfehler mitten im Vorgang |

Jede Ablehnung nennt den Grund in `{"error":…}`. Geprüft wird in der Reihenfolge, in der
die Ursachen tatsächlich vorkommen: **zuerst das Magic am Dateianfang**, erst danach die
Grösse. Umgekehrt bekäme der häufigste Fall — eine versehentlich geschickte
`firmware.bin` — die Meldung „kein Vielfaches von 512", und die zeigt in die falsche
Richtung.

```json
{"error":"kein UF2-Magic (0x0A324655) am Dateianfang, sondern 0x20003FFC, 572636 Byte
          empfangen - das sieht nach einer .bin aus. In der platformio.ini
          \"@$BUILD_DIR/${PROGNAME}.uf2\" statt \"@$SOURCE\" verwenden."}
```

Dafür entscheidet die Probe bereits nach den ersten 512 Byte, **bevor** sie Speicher für
die Datei anfordert. Damit der Text auch im PlatformIO-Log landet, gehört in das Snippet
`--fail-with-body` statt `-f`: mit `-f` verschluckt curl den Body und übrig bleibt nur
`curl: (22)`.

`?validate_only=1` prüft die Datei, ohne zu schreiben, und liefert
`{"blocks":…,"payload_bytes":…,"first_address":…,"family":…,"status":"valid"}`.

Den BOOTSEL-Modus stellt der Endpunkt selbst her (1200-Baud-Touch, siehe oben) —
**solange die Firmware des Ziels noch enumeriert**. Nur wenn sie das nicht mehr tut, muss
jemand ran: BOOTSEL und RUN sind an der Frontblende bedienbar, und die Probe erkennt den
Zustandswechsel von selbst. Ein Power-Cycle über VBUS hilft dort ausdrücklich nicht, weil
alle OpenKNX-Geräte KNX-bus-gespeist sind — siehe [hardware.md](hardware.md).

### `POST /api/reset` **[verworfen]**

Setzte einen Power-Cycle über VBUS voraus. Bei busgespeisten Zielen bewirkt der keinen
Reset — siehe [hardware.md](hardware.md). Über RFC2217 lässt sich DTR/RTS durchreichen,
was bei ESP32-Zielen den Auto-Reset auslöst.

Die ursprünglich mitverworfene Idee `POST /api/bootsel` ist dagegen **umgesetzt**, nur auf
einem anderen Weg: nicht über VBUS, sondern über den 1200-Baud-Touch auf der CDC. Siehe
den eigenen Abschnitt weiter unten.

### `GET /api/console?cursor=N` **[live]**

Der Mitschnitt der Zielkonsole für den Browser. `cursor` ist eine **absolute** Position
im Datenstrom, kein Ringpuffer-Index; ohne Parameter kommt der gesamte vorhandene Puffer.

```json
{ "cursor": 20480, "skipped": 0, "more": false, "data": "…" }
```

| Feld | Bedeutung |
|---|---|
| `cursor` | mit diesem Wert den nächsten Abruf stellen |
| `skipped` | so viele Bytes sind überschrieben worden, bevor sie abgeholt wurden |
| `more` | es liegt noch mehr an — sofort erneut abrufen, ohne zu warten |
| `data` | die Rohbytes, JSON-escapt (Steuerzeichen als `\u00XX`) |

Pro Antwort höchstens 4 KB, damit eine Antwort weder den Heap noch den Browser
belastet; deshalb das `more`-Flag.

Die Rohbytes bleiben roh: CR, LF und ANSI-Sequenzen kommen unverändert durch. Die
OpenKNX-Konsole schreibt ihren Prompt nach jeder Logzeile neu und radiert ihn beim
nächsten Mal mit `ESC[2K` weg — das Web-UI führt dafür eine kleine
Zeilen-Zustandsmaschine (CR bewegt nur den Cursor, es löscht nichts).

`/api/status` führt dazu `radio_awake`: `true`, solange ein Client auf 2323 oder 4000
hängt und der Funk deshalb mit `WIFI_PS_NONE` läuft, sonst `false` (`WIFI_PS_MIN_MODEM`).
HTTP selbst hält den Funk **nicht** wach.

### `POST /api/console` **[live]** — Kommando ans Zielgerät

Body ist der rohe Text **ohne** Zeilenende, `?eol=cr|lf|crlf|none` bestimmt, was
angehängt wird (Standard `cr`). Antwort `{"status":"sent","bytes":N}`.

Warum das wählbar ist: die OpenKNX-Konsole arbeitet zeichenweise — jeder Tastendruck
wird sofort verarbeitet und zurückgeschrieben — während andere Firmware auf eine
abgeschlossene Zeile wartet. Ein fest eingebautes Zeilenende würde den einen Fall
kaputtmachen, um den anderen zu bedienen. Leerer Body mit `eol=cr` schickt nur das
Zeilenende, was den Prompt neu zeichnen lässt.

Die Bytes gehen denselben Weg wie die eines TCP-Clients auf Port 2323, laufen also in
`bytesToTarget()` mit und erscheinen in `/api/status` unter `console.tx`. Mehr als
512 Byte werden mit `413` abgelehnt — dieser Endpunkt ist für getippte Kommandos, nicht
für Dateien.

| Status | Bedeutung |
|---|---|
| 200 | geschrieben |
| 400 | `eol` unbekannt, oder nichts zu senden |
| 409 | keine serielle Sitzung zum Ziel offen |
| 413 | Kommando länger als 512 Byte |
| 502 | Schreiben auf das Ziel fehlgeschlagen |

Am Gerät geprüft gegen einen OpenKNX-Fan-Aktor: `h` liefert die komplette Hilfe,
`uptime` die Laufzeit — beides über die Web-Konsole eingegeben.

### `POST /api/bootsel` **[live]**

Schickt ein laufendes RP2040-Ziel per 1200-Baud-Touch nach BOOTSEL, ohne etwas zu
schreiben. Kein Body. Dasselbe, was `/api/flash` intern tut — nützlich, um den Touch zu
prüfen, ohne die Firmware des Ziels zu überschreiben, und als Knopf im Web-UI.

```json
{"status":"bootsel","touched":true,"msc_kb":131071}
```

`touched` ist `false`, wenn das Ziel schon im BOOTSEL stand und nichts zu tun war. Bei
Misserfolg `409` mit demselben `error`-Text wie `/api/flash`.

Zurück in die Anwendung kommt das Ziel danach durch einen Reset (RUN-Taster oder
Aus/Ein) oder eben durch das nächste `POST /api/flash`. Der Flash-Inhalt bleibt vom
Touch unberührt.

### `GET /api/snippet` **[live]**

Die fertigen `platformio.ini`-Zeilen für das gerade angesteckte Ziel, als
`text/plain`. Gedacht für Skripte und KI-Agenten: ein Aufruf genügt, um die passende
Konfiguration zu bekommen, ohne die Ports und Baudraten selbst zu kennen.

```
; OpenKNX DebugProbe - openknx-probe-xxxx.local
; Ziel: Raspberry Pi Pico (USB-CDC) (2E8A:000A)
monitor_port  = socket://openknx-probe-xxxx.local:2323
monitor_speed = 115200
upload_protocol = custom
upload_command  = curl --fail-with-body --data-binary "@$BUILD_DIR/${PROGNAME}.uf2" http://openknx-probe-xxxx.local/api/flash
```

Welcher Upload-Weg erscheint, hängt am erkannten Ziel: **VID `0x2E8A`** (RP2040) bekommt
den UF2-Weg über `/api/flash`, alles andere `upload_port = rfc2217://…:4000` mit
`upload_speed = 460800`. Ohne angestecktes Ziel bleiben nur die beiden
Konsolenzeilen übrig, mit `; Ziel: kein Zielgeraet angesteckt` als Kommentar.

Dieselben Strings stehen unter `snippet` in `/api/status` und füllen im Web-UI die zwei
Kopierfelder. Gebaut werden sie **in der Firmware** (`buildSnippet()` in `main.cpp`) —
so können Endpunkt und Anzeige nicht auseinanderlaufen, wenn sich ein Port ändert.

## PlatformIO-Integration im Zielprojekt **[live]**

```ini
[env:openknx_remote]
monitor_port    = socket://openknx-probe-xxxx.local:2323
upload_protocol = custom
upload_command  = curl --fail-with-body --data-binary "@$BUILD_DIR/${PROGNAME}.uf2" http://openknx-probe-xxxx.local/api/flash
```

Vollständig kommentiert in `tools/platformio-snippet.ini`, dort auch die Variante mit
`rfc2217://` für ESP32-Ziele.

Bequemer ist beides zu holen, statt es abzuschreiben — die Probe kennt ihren eigenen
Namen und das angesteckte Ziel:

```powershell
curl.exe http://openknx-probe-xxxx.local/api/snippet
```

Im Web-UI stehen dieselben Zeilen in zwei Kopierfeldern.
