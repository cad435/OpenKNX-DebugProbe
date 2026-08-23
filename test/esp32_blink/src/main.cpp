/**
 * Blink-Testfirmware fuer das ESP32-Zielgeraet der DebugProbe.
 *
 * Blinkt die Onboard-LED und schreibt zusaetzlich im Sekundentakt eine Zeile.
 * Die serielle Ausgabe ist Absicht: sie belegt aus der Ferne, dass die Firmware
 * laeuft, auch wenn der LED-Pin auf dem konkreten Board ein anderer ist.
 */

#include <Arduino.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

namespace {
constexpr uint8_t LED_PIN  = LED_BUILTIN;
uint32_t          g_ticks  = 0;
}

void setup()
{
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    delay(200);
    Serial.println();
    Serial.println("[esp32] Blink-Testfirmware der OpenKNX DebugProbe");
    Serial.print("[esp32] Build ");
    Serial.print(__DATE__);
    Serial.print(' ');
    Serial.println(__TIME__);
    Serial.print("[esp32] LED an GPIO ");
    Serial.println(LED_PIN);
    Serial.println("[esp32] ueber WLAN geflasht, kein USB-Kabel am PC");
}

void loop()
{
    digitalWrite(LED_PIN, HIGH);
    delay(500);
    digitalWrite(LED_PIN, LOW);
    delay(500);

    Serial.print("[esp32] blink ");
    Serial.print(g_ticks++);
    Serial.print(", uptime ");
    Serial.print(millis() / 1000);
    Serial.println(" s");
}
