#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"

class UsbTarget;

/**
 * Das Protokoll von cnlohrs rv003usb-Bootloader (`1209:B003`), portiert aus
 * `pgm-b003fun.c` des ch32fun-Projekts.
 *
 * **Warum das hier nachgebaut wird und nicht minichlink weitergereicht:**
 * minichlink kann seinen Programmer nicht ueber Netz ansprechen. Sein
 * `cmdserver` auf Port 4444 ist ein *lauschender* Server fuer fremde Werkzeuge,
 * keine Gegenstelle. Entweder patcht man minichlink, oder die Probe spricht das
 * Protokoll selbst — und dann flasht man aus dem Browser, ohne Werkzeug auf dem
 * PC.
 *
 * **Wie der Bootloader arbeitet.** Er hat nur 1920 Byte und kennt deshalb keine
 * Kommandos wie „lies Speicher" oder „loesche Flash". Er kennt genau ein
 * Primitiv: *fuehre diesen Scratchpad aus*. Alles andere liefert die Gegenseite
 * als kleine RISC-V-Routine mit, die in den Scratchpad geschrieben und dort
 * ausgefuehrt wird (siehe `bootloader.c`, `scratchexec()`).
 *
 * Der Scratchpad geht als **HID-Feature-Report** hin und zurueck:
 *
 *     [0]      Report-ID, zugleich Groessenkennung (0xAA + Groesse/1024)
 *     [1..]    Code der auszufuehrenden Routine
 *     danach   Parameter der Routine
 *     Ende-4   Magic 0x1234ABCD — startet die Ausfuehrung
 *
 * Danach wird per GET_REPORT gepollt, bis Byte 1 auf 0xFF steht: fertig.
 */
class B003Link
{
public:
    /// Standardgroessen aus `TryInit_B003Fun()`. Groessere Bootloader melden
    /// mehr, das wird erst beim Verhandeln interessant.
    static constexpr size_t DEFAULT_SCRATCHPAD = 128;
    static constexpr size_t DEFAULT_DATA_SIZE  = 64;

    /// Groesster Scratchpad, den das Protokoll kennt.
    static constexpr size_t MAX_SCRATCHPAD = 6272;

    explicit B003Link(UsbTarget& usb) : m_usb(usb) {}

    /// Prueft, ob ein rv003usb-Bootloader am USB haengt, und richtet die
    /// Puffer ein.
    esp_err_t begin(std::string& error);

    /// Liest @p size Bytes ab @p address aus dem Adressraum des Ziels.
    esp_err_t readBlob(uint32_t address, size_t size, uint8_t* out, std::string& error);

    esp_err_t readWord(uint32_t address, uint32_t& value, std::string& error);

    /**
     * Chip-Kennung des Ziels.
     *
     * Bewusst nur die CH32V-Familie: die CH5xx-Zweige aus `pgm-b003fun.c`
     * bleiben draussen, die brauchen wir nicht und sie machen mehr als die
     * Haelfte der Datei aus.
     */
    struct ChipInfo
    {
        uint32_t    idLow {0};    ///< 0x1FFFF7C4
        uint32_t    idHigh {0};   ///< 0x1FFFF704
        bool        readProtected {false};
        std::string name;
    };

    esp_err_t identify(ChipInfo& info, std::string& error);

    /**
     * Startet den Anwendungscode des Ziels.
     *
     * Der Bootloader kennt kein eigenes „boot"-Kommando — auch das kommt als
     * Routine. `run_app_blob` setzt die Option-Bytes zurueck und springt in den
     * Anwendungsbereich.
     *
     * Danach ist das Geraet vom Bus weg, deshalb wird **nicht** auf eine
     * Quittung gewartet: minichlink setzt hier `no_get_report`, und genau das
     * macht diese Funktion auch.
     */
    esp_err_t bootUserCode(std::string& error);

    const std::vector<uint8_t>& lastResponse() const { return m_resp; }

    size_t scratchpadSize() const { return m_scratchpad; }
    size_t dataSize() const { return m_dataSize; }

private:
    // --- Scratchpad zusammenbauen (ResetOp / WriteOp4 / WriteOpArb) --------
    void resetOp();
    void writeOp4(uint32_t value);
    void writeOpArb(const uint8_t* data, size_t length);

    /**
     * Schickt den Scratchpad ab und wartet, bis die Routine fertig ist.
     *
     * @param sendLen  zusaetzliche Nutzdaten, die mitgehen
     * @param recvLen  erwartete Rueckdaten; 0 nutzt die kurze Statusabfrage
     */
    esp_err_t commit(size_t sendLen, size_t recvLen, std::string& error, bool poll = true);

    /// Rundet auf die naechste vom Bootloader erwartete Reportgroesse.
    static size_t padSize(size_t needed);

    UsbTarget&           m_usb;
    std::vector<uint8_t> m_cmd;
    std::vector<uint8_t> m_resp;
    size_t               m_place {0};
    size_t               m_scratchpad {DEFAULT_SCRATCHPAD};
    size_t               m_dataSize {DEFAULT_DATA_SIZE};
};
