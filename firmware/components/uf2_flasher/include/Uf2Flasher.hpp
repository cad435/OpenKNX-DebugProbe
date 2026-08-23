#pragma once

#include <cstdint>
#include <string>

#include "UsbTarget.hpp"
#include "esp_err.h"

/**
 * Prüft eine UF2-Datei und schreibt sie auf ein RP2040 im BOOTSEL-Modus.
 *
 * Reihenfolge ist bewusst: **erst vollständig validieren, dann schreiben.**
 * Ohne SWD ist der einzige Weg zurück ein Tastendruck am Gerät; eine kaputte
 * Datei darf deshalb gar nicht erst losgeschrieben werden.
 *
 * Geschrieben wird als rohe 512-Byte-Sektoren. Das Boot-ROM durchsucht jeden
 * eingehenden Sektor nach dem UF2-Magic und ignoriert die FAT-Struktur — ein
 * Dateisystem auf der Probe ist nicht nötig.
 */
class Uf2Flasher
{
public:
    static constexpr uint32_t MAGIC_START0   = 0x0A324655;  ///< "UF2\n"
    static constexpr uint32_t MAGIC_START1   = 0x9E5D5157;
    static constexpr uint32_t MAGIC_END      = 0x0AB16F30;
    static constexpr uint32_t FLAG_FAMILY_ID = 0x00002000;
    static constexpr uint32_t FAMILY_RP2040  = 0xE48BFF56;
    static constexpr size_t   BLOCK_SIZE     = 512;
    static constexpr size_t   MAX_PAYLOAD    = 476;

    struct Info
    {
        uint32_t blocks {0};
        uint32_t familyId {0};
        uint32_t firstAddress {0};
        uint32_t lastAddress {0};
        uint32_t payloadBytes {0};
    };

    /**
     * Urteil über den Dateianfang, sobald der erste Block da ist.
     *
     * Getrennt von `validate()`, damit der HTTP-Handler schon nach 512 Byte
     * ablehnen kann — vor allem aber, weil die häufigste Fehlbedienung eine
     * versehentlich geschickte `firmware.bin` ist. Die soll auch als solche
     * gemeldet werden und nicht als „krumme Grösse": PlatformIO löst `$SOURCE`
     * bei `upload_protocol = custom` auf die `.bin` auf, nicht auf die `.uf2`.
     *
     * @param head      Anfang der Datei, für das Magic reichen 4 Byte.
     * @param headSize  wie viel von `head` tatsächlich gefüllt ist.
     * @param totalSize angekündigte Gesamtgrösse (`Content-Length`).
     */
    static bool checkPrologue(const uint8_t* head,
                              size_t         headSize,
                              size_t         totalSize,
                              std::string&   error);

    /// Vollständige Prüfung: Magics, Blockkette, numBlocks, Family-ID.
    static bool validate(const uint8_t* data, size_t size, Info& info, std::string& error);

    /**
     * Schreibt die bereits validierte Datei aufs Ziel.
     *
     * Das Boot-ROM startet neu, sobald es den letzten Block hat — ein Fehler
     * beim allerletzten Sektor ist deshalb der Normalfall und wird als Erfolg
     * gewertet.
     */
    static esp_err_t flash(UsbTarget&     target,
                           const uint8_t* data,
                           size_t         size,
                           uint32_t&      blocksWritten,
                           std::string&   error);
};
