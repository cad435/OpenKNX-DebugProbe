# Architektur

## Überblick

```
   PC                          WLAN                    Schaltschrank
+---------+                                   +-------------------------+
| pio     |--- TCP  :2323 ------------------->| serial_bridge           |
| monitor |                                   |        ^                |
|         |                                   |        v                |
| curl /  |--- HTTP :80  ------------------->| web_api -> uf2_flasher  |
| Browser |                                   |        ^                |
+---------+                                   |        v                |
                                              | usb_target (USB Host)   |
                                              | target_power (VBUS)     |
                                              +-----------+-------------+
                                                          | USB-C
                                                          v
                                                 OpenKNX RP2040
```

## Task- und Core-Aufteilung

| Task | Core | Aufgabe |
|---|---|---|
| `usb_host_lib` (IDF) | 1 | USB-Host-Eventschleife, muss möglichst jitterarm laufen |
| `usb_target` | 1 | Zustandsmaschine, CDC-Transfers, MSC-Transfers |
| `serial_bridge` | 0 | TCP-Server, Ringpuffer |
| `web_api` (httpd) | 0 | HTTP-Server, Upload-Streaming |
| WiFi/lwIP (IDF) | 0 | Funk |

Die strikte Trennung USB (Core 1) / Netz (Core 0) ist der Hauptgrund für den ESP32-S3:
auf einem Einkern-S2 konkurrieren USB-Host-Stack und WiFi um dieselbe CPU, und genau das
erzeugt sporadische Aussetzer, die sich später kaum reproduzieren lassen.

## Zustandsmaschine `usb_target`

```
        +-----------------+
        |  NO_TARGET      |  kein Gerät / VBUS aus
        +--------+--------+
                 | Device connect
                 v
        +-----------------+
        |  ENUMERATING    |
        +--+-----------+--+
           |           |
   CDC-ACM |           | MSC (VID 2e8a / PID 0003)
           v           v
   +-------------+  +-------------+
   |  CONSOLE    |  | BOOTSEL_MSC |
   +------+------+  +------+------+
          |                |
   flash  | 1200-Baud-     | UF2 validiert
   request| Touch          v
          |         +-------------+
          +-------->|  FLASHING   |
                    +------+------+
                           | letzter Block geschrieben
                           v
                    +-------------+
                    | POST_FLASH  |  Ziel rebootet selbst
                    +------+------+
                           | CDC wieder da
                           v
                        CONSOLE
```

Fehlerpfade:

- `ENUMERATING` -> Timeout (5 s) -> `NO_TARGET`, VBUS kurz aus/ein, ein Retry.
- Touch abgesetzt, aber kein MSC innerhalb 5 s -> ein Retry, danach `ERROR`. Am Endpunkt
  ist das ein `409` mit Begründung; vorher werden Baudrate und Steuerleitungen in den
  Konsolen-Ruhezustand zurückgestellt, damit ein gescheiterter Touch das Ziel nicht mit
  1200 Baud und abgefallenem DTR zurücklässt. Der BOOTSEL/RUN-Pigtail ist verworfen
  (Taster sitzen an der Frontblende), es bleibt also beim manuellen Griff ans Gerät.
- `POST_FLASH` ohne CDC innerhalb 15 s -> `ERROR` mit Hinweis "Firmware startet nicht".

## Flash-Ablauf im Detail

1. **Upload.** `POST /api/flash` puffert die UF2 im **PSRAM**. Eine Staging-Partition im
   SPI-Flash war der ursprüngliche Plan und ist verworfen: bei 4 MB Flash ist für ~1,4 MB
   UF2 kein Platz, die 2 MB PSRAM reichen dafür. Preis: der Puffer überlebt keinen Reboot,
   ein Re-Flash braucht also einen neuen Upload. Beurteilt wird schon nach den ersten
   512 Byte, **bevor** Speicher angefordert wird — siehe `Uf2Flasher::checkPrologue()`.
2. **Validierung.** Erst vollständig prüfen, dann handeln: Magic Start `0x0A324655` /
   `0x9E5D5157`, Magic End `0x0AB16F30`, Family-ID `0xe48bff56` (RP2040), `numBlocks`
   konsistent, Blockkette lückenlos. Schlägt das fehl, wird der Touch **nie** ausgelöst.
3. **Touch.** CDC-Line-Coding auf 1200 Baud setzen, DTR aktivieren, kurz halten, wieder
   fallen lassen — genau die Bussequenz, die ein Öffnen und Schließen des Ports am PC
   erzeugt (das meint PlatformIO mit „Forcing reset using 1200bps open/close"). Das Ziel
   rebootet nach BOOTSEL. Implementiert in `UsbTarget::enterBootsel()`; die 1200 werden
   **nicht** in `m_baudRate` übernommen, damit die Konsole nach dem Flashen wieder mit
   115200 aufgesetzt wird. Ein Ziel, das schon im BOOTSEL steht, wird nicht angefasst.
4. **Schreiben.** Das Ziel meldet sich als MSC mit VID `0x2e8a` / PID `0x0003`.
   Die UF2-Blöcke werden als **rohe 512-Byte-Sektoren** per SCSI `WRITE(10)` geschrieben —
   das Boot-ROM durchsucht eingehende Sektoren nach dem UF2-Magic und ignoriert die
   FAT-Struktur. Damit entfällt FatFs auf der Probe komplett.
   *(Am Gerät bestätigt: 296 Blöcke in 1,7 s, Ziel rebootet und meldet sich zurück.)*
5. **Selbst-Reset.** Sobald das Boot-ROM alle Blöcke hat, rebootet es sofort. Der letzte
   Schreibzugriff bzw. das folgende `TEST UNIT READY` **schlägt planmäßig fehl** — das ist
   Erfolg, kein Fehler. Diese Stelle ist die häufigste Fehlerquelle in eigenen Flashern.

## Serieller Pfad

- `usb_host_cdc_acm` liest kontinuierlich, schreibt in einen Ringpuffer (Vorschlag 16 KB).
- Der TCP-Server gibt bei Verbindungsaufbau den Puffer-Inhalt aus, damit Boot-Meldungen
  nicht verloren gehen, die vor dem Verbinden aufliefen.
- Kein Client verbunden: weiterlesen und puffern, nicht blockieren.
- Ziel verschwindet (Reset/Flash): TCP-Verbindung **offen halten**, optional eine
  Statuszeile einblenden, bei Rückkehr nahtlos weiterliefern.
- Optional RFC2217 auf zweitem Port, wenn Baudraten-Umschaltung vom PC gebraucht wird.
