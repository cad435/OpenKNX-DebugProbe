# Components

| Component | Status | Verantwortung |
|---|---|---|
| `settings` | **implementiert** | RAII-Wrapper um einen NVS-Namespace |
| `web_api` | **implementiert** | Der eine HTTP-Server der Probe; alle Features registrieren ihre Routen hier |
| `net_wifi` | **implementiert** | WLAN-Verbindung, Captive-Portal-Provisioning, mDNS, dynamisches Stromsparen |
| `ota_update` | **implementiert** | Selbst-Update über HTTP mit A/B-Slots und Rollback |
| `usb_target` | **implementiert** | USB-Host: Enumeration, Identifikation, CDC/VCP-Sitzung, MSC, 1200-Baud-Touch nach BOOTSEL |
| `serial_bridge` | **implementiert** | TCP 2323 ⟷ CDC des Ziels mit Ringpuffer, dazu RFC2217 auf 4000 |
| `uf2_flasher` | **implementiert** | UF2 vollständig validieren, dann als rohe 512-Byte-Sektoren schreiben |
| `target_power` | Stub | VBUS-Load-Switch, Strommessung — optional, siehe `hardware/README.md` |

Geschrieben wird **ohne Dateisystem**: das RP2040-Boot-ROM durchsucht jeden eingehenden
Sektor nach dem UF2-Magic und ignoriert die FAT-Struktur des `RPI-RP2`-Volumes. FatFs auf
der Probe entfällt damit komplett.

`target_power` ist eine leere ESP-IDF-Component (`idf_component_register()` ohne Quellen),
damit der Build grün bleibt, solange sie nicht gebraucht wird. Bei busgespeisten Zielen ist
der Load-Switch nur noch optional — ein Power-Cycle über VBUS erzwingt dort ohnehin keinen
Reset, sondern nur ein Re-Enumerieren.
