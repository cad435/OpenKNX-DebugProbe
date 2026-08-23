/**
 * Testfirmware fuer das RP2040-Zielgeraet der DebugProbe.
 *
 * Tut absichtlich nur zwei Dinge: LED blinken und im Sekundentakt eine Zeile
 * ueber USB-CDC schreiben. Damit laesst sich pruefen, ob
 *
 *   1. die Probe eine UF2 korrekt aufs Ziel schreibt (Stufe 4) und
 *   2. die Konsole des Ziels ueber TCP durchkommt (Stufe 3).
 *
 * Nach dem Flashen sollte auf `socket://<probe>.local:2323` im Sekundentakt
 * eine Zeile erscheinen.
 */

#include <Arduino.h>

namespace {
uint32_t g_counter = 0;
}

void setup()
{
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);

    // Etwas Vorlauf, damit die ersten Zeilen im Mitschnittpuffer der Probe
    // landen — genau der Fall, den ein direktes USB-Kabel nicht mitbekommt.
    Serial.println();
    Serial.println("[pico] OpenKNX DebugProbe - Testfirmware gestartet");
    Serial.print("[pico] Build ");
    Serial.print(__DATE__);
    Serial.print(' ');
    Serial.println(__TIME__);
}

void loop()
{
    digitalWrite(LED_BUILTIN, (g_counter & 1) ? HIGH : LOW);

    // Bewusst ohne printf: je nach Arduino-Core ist Serial eine UART-Klasse
    // ohne printf().
    Serial.print("[pico] heartbeat ");
    Serial.print(g_counter++);
    Serial.print(", uptime ");
    Serial.print(millis());
    Serial.println(" ms");

    delay(1000);
}
