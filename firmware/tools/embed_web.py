"""Wandelt components/<name>/web/*.html in C++-Header mit Raw-String-Literalen um.

Hintergrund: PlatformIO baut die CMake-Konfiguration von ESP-IDF in SCons nach und
unterstuetzt dabei EMBED_TXTFILES/EMBED_FILES nicht — die von CMake erzeugte
.S-Datei entsteht im PlatformIO-Build nicht ("Source portal.html.S not found").

Statt die HTML-Seiten in den Quellcode zu kopieren, erzeugt dieses Skript vor
jedem Build je einen Header:

    components/net_wifi/web/portal.html
      -> components/net_wifi/include/generated/portal_html.hpp
         inline constexpr char PORTAL_HTML[]     = R"WEBASSET(...)WEBASSET";
         inline constexpr unsigned PORTAL_HTML_LEN = ...;

Die HTML-Dateien bleiben damit die Quelle der Wahrheit und normal editierbar.
"""

import os
import re

Import("env")  # noqa: F821  (von PlatformIO injiziert)

DELIMITER = "WEBASSET"

HEADER_TEMPLATE = """// Automatisch erzeugt aus {source} — nicht von Hand editieren.
// Erzeugt von tools/embed_web.py, siehe platformio.ini (extra_scripts).
#pragma once

inline constexpr char {symbol}[] = R"{delim}({content}){delim}";
inline constexpr unsigned {symbol}_LEN = sizeof({symbol}) - 1;
"""


def symbol_for(filename):
    return re.sub(r"[^A-Za-z0-9]", "_", filename).upper()


def write_if_changed(path, content):
    """Nur schreiben, wenn sich etwas geaendert hat — sonst rebuildet alles."""
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8", newline="") as handle:
            if handle.read() == content:
                return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(content)
    return True


def main():
    project_dir = env["PROJECT_DIR"]  # noqa: F821
    components_dir = os.path.join(project_dir, "components")
    if not os.path.isdir(components_dir):
        return

    generated = 0
    for component in sorted(os.listdir(components_dir)):
        web_dir = os.path.join(components_dir, component, "web")
        if not os.path.isdir(web_dir):
            continue

        for filename in sorted(os.listdir(web_dir)):
            if not filename.endswith((".html", ".css", ".js", ".svg")):
                continue

            source_path = os.path.join(web_dir, filename)
            with open(source_path, "r", encoding="utf-8") as handle:
                content = handle.read()

            closing = ")" + DELIMITER + '"'
            if closing in content:
                raise SystemExit(
                    "embed_web.py: %s enthaelt die Zeichenfolge %s und laesst sich "
                    "nicht als Raw-String einbetten." % (source_path, closing)
                )

            symbol = symbol_for(filename)
            out_path = os.path.join(
                components_dir, component, "include", "generated",
                filename.replace(".", "_") + ".hpp",
            )
            header = HEADER_TEMPLATE.format(
                source=os.path.join("web", filename).replace("\\", "/"),
                symbol=symbol,
                delim=DELIMITER,
                content=content,
            )
            if write_if_changed(out_path, header):
                generated += 1
                print("embed_web: %s/%s -> %s" % (component, filename, symbol))

    if generated == 0:
        print("embed_web: alle Web-Assets aktuell")


main()
