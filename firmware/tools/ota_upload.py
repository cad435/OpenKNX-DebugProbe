"""
Upload ueber WLAN statt ueber die serielle Schnittstelle.

Warum das hier steht und nicht einfach als `upload_command` in der
platformio.ini: PlatformIO benutzt fuer `upload` und `uploadfs` **denselben**
Befehl und tauscht nur `$SOURCE` aus. Die beiden Images gehoeren aber an
verschiedene Endpunkte der Probe — die firmware.bin nach `/api/update` (dort
laeuft das OTA mit Rollback), die littlefs.bin nach `/api/fs` (dort wird die
Partition `storage` geschrieben). Ein einzelner statischer Befehl kann das
nicht unterscheiden, dieses Skript schon.

Gesteuert wird es ueber `custom_ota_host` in der platformio.ini; der Rueckweg
auf seriell steht als Kommentar dort.
"""

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")  # noqa: F821 — von PlatformIO bereitgestellt


# `uploadfs` schiebt das Dateisystem, `upload` das Firmware-Image.
IS_FILESYSTEM = "uploadfs" in COMMAND_LINE_TARGETS
WANTS_UPLOAD = IS_FILESYSTEM or "upload" in COMMAND_LINE_TARGETS

host = env.GetProjectOption("custom_ota_host", "").strip()

# Nur meckern, wenn wirklich hochgeladen werden soll — ein blosses `pio run`
# darf daran nicht scheitern.
if WANTS_UPLOAD and not host:
    raise SystemExit(
        "\n[ota_upload] custom_ota_host ist nicht gesetzt.\n"
        "  Trag in der platformio.ini den Namen der Probe ein, z. B.\n"
        "      custom_ota_host = openknx-probe-xxxx.local\n"
        "  oder nimm den seriellen Weg (siehe Kommentar dort).\n"
    )

if WANTS_UPLOAD:
    endpoint = "/api/fs" if IS_FILESYSTEM else "/api/update"
    url = "http://{}{}".format(host, endpoint)

    # --fail-with-body: curl soll bei 4xx/5xx mit einem Fehlercode aussteigen,
    # die Begruendung der Probe aber trotzdem ausgeben. Ohne das quittiert ein
    # abgelehnter Upload mit "Erfolg" und einer JSON-Zeile.
    env.Replace(
        UPLOADCMD='curl --fail-with-body --progress-bar --data-binary "@$SOURCE" ' + url
    )

    print("ota_upload: {} -> {}".format(
        "Web-Oberflaeche" if IS_FILESYSTEM else "Firmware", url))
