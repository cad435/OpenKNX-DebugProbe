#include "B003Link.hpp"

#include <algorithm>
#include <cstring>

#include "UsbTarget.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "b003";

/// Startet die Ausfuehrung der Routine im Scratchpad. Steht in den letzten
/// vier Bytes des Reports (siehe `bootloader.c`, Scratchpad-Struktur).
constexpr uint32_t MAGIC_GO = 0x1234ABCDu;

/// Basis der Report-ID. Die Groessenklasse wird aufaddiert.
constexpr uint8_t REPORT_BASE = 0xAA;

/// Kurze Statusabfrage ohne Nutzdaten.
constexpr uint8_t REPORT_STATUS_SHORT = 0xA8;

/// Wie oft auf "Routine fertig" gewartet wird, bevor aufgegeben wird.
constexpr int POLL_LIMIT = 40;

/*
 * Die RISC-V-Routinen, die im Ziel ausgefuehrt werden. Uebernommen aus
 * `pgm-b003fun.c` bzw. den Stub-Quellen unter `stubs/b003` des ch32fun-Projekts.
 *
 * Sie laufen NICHT auf der Probe — sie sind Nutzlast. Der Bootloader legt sie
 * in seinen Scratchpad und springt hinein. Beispiel `word_wise_read_blob`,
 * disassembliert: Zieladresse und Laenge aus den Parametern holen, dann
 * wortweise aus dem Speicher in den Scratchpad kopieren und "fertig" melden.
 */
const uint8_t WORD_WISE_READ_BLOB[] = {
    0x23, 0xa0, 0x05, 0x00, 0x13, 0x07, 0x45, 0x03, 0x0c, 0x43, 0x50, 0x43,
    0x2e, 0x96, 0x21, 0x07, 0x94, 0x41, 0x14, 0xc3, 0x91, 0x05, 0x11, 0x07,
    0xe3, 0xcc, 0xc5, 0xfe, 0x93, 0x06, 0xf0, 0xff, 0x14, 0xc1, 0x82, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

const uint8_t BYTE_WISE_READ_BLOB[] = {
    0x23, 0xa0, 0x05, 0x00, 0x13, 0x07, 0x45, 0x03, 0x0c, 0x43, 0x50, 0x43,
    0x2e, 0x96, 0x21, 0x07, 0x94, 0x21, 0x14, 0xa3, 0x85, 0x05, 0x05, 0x07,
    0xe3, 0xcc, 0xc5, 0xfe, 0x93, 0x06, 0xf0, 0xff, 0x14, 0xc1, 0x82, 0x80,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

/// Adressen der Kennungsregister der CH32V-Familie (aus `B003DetermineChipType`).
constexpr uint32_t ADDR_FLASH_STATR = 0x4002200Cu;
constexpr uint32_t ADDR_OBR         = 0x4002201Cu;
constexpr uint32_t ADDR_WRPR        = 0x40022020u;
constexpr uint32_t ADDR_CHIPID_LOW  = 0x1FFFF7C4u;
constexpr uint32_t ADDR_CHIPID_HIGH = 0x1FFFF704u;

}  // namespace

// ---------------------------------------------------------------------------
// Scratchpad zusammenbauen
// ---------------------------------------------------------------------------

void B003Link::resetOp()
{
    std::fill(m_cmd.begin(), m_cmd.end(), 0);
    // Die ersten vier Bytes: Report-ID (wird in commit() gesetzt) und das
    // Statusfeld, das die Routine im Ziel auf 0xFF setzt, wenn sie fertig ist.
    m_cmd[0] = REPORT_BASE;
    m_place  = 4;
}

void B003Link::writeOp4(uint32_t value)
{
    if (m_place + 4 < m_scratchpad)
    {
        std::memcpy(m_cmd.data() + m_place, &value, 4);
    }
    m_place += 4;
}

void B003Link::writeOpArb(const uint8_t* data, size_t length)
{
    if (m_place + length < m_scratchpad)
    {
        std::memcpy(m_cmd.data() + m_place, data, length);
    }
    m_place += length;
}

size_t B003Link::padSize(size_t needed)
{
    // Stufen aus CommitOp(). Der Bootloader leitet aus der Groesse die
    // Report-ID ab, also darf hier nicht frei gerundet werden.
    if (needed > 5248) return 6272;
    if (needed > 4096) return 5248;
    if (needed > 3200) return 4096;
    if (needed > 2176) return 3200;
    if (needed > 1152) return 2176;
    if (needed > 128) return 1152;
    return 128;
}

esp_err_t B003Link::commit(size_t sendLen, size_t recvLen, std::string& error)
{
    const size_t needed = m_place + sendLen + 4;
    if (needed > m_scratchpad)
    {
        error = "Kommando passt nicht in den Scratchpad des Ziels";
        return ESP_ERR_INVALID_SIZE;
    }

    size_t  pad      = padSize(needed);
    uint8_t reportId = static_cast<uint8_t>(REPORT_BASE + (pad / 1024));

    std::memcpy(m_cmd.data() + pad - 4, &MAGIC_GO, 4);
    m_cmd[0] = reportId;

    esp_err_t err = m_usb.hidFeature(true, reportId, m_cmd.data(), pad, error);
    if (err != ESP_OK) return err;

    // Antwortgroesse: mit Rueckdaten so gross wie noetig, sonst die kurze Form.
    if (recvLen > 0)
    {
        const size_t rneeded = recvLen + m_place + 4;
        if (rneeded > m_scratchpad)
        {
            error = "Rueckgabe passt nicht in den Scratchpad des Ziels";
            return ESP_ERR_INVALID_SIZE;
        }
        pad      = padSize(rneeded);
        reportId = static_cast<uint8_t>(REPORT_BASE + (pad / 1024));
    }
    else
    {
        // `no_eight_byte` ist bei den kleinen Bootloadern gesetzt: sie koennen
        // die 8-Byte-Kurzform nicht und wollen den vollen Report.
        reportId = (m_scratchpad <= DEFAULT_SCRATCHPAD) ? REPORT_BASE : REPORT_STATUS_SHORT;
        pad      = (m_scratchpad <= DEFAULT_SCRATCHPAD) ? DEFAULT_SCRATCHPAD : 8;
    }

    /*
     * Pollen, bis die Routine fertig ist. Byte 1 auf 0xFF ist das Signal, das
     * der Stub am Ende setzt — siehe die Disassembly von word_wise_read_blob:
     * `li x13,-1; sw x13,0(x10)`.
     */
    for (int tries = 0; tries < POLL_LIMIT; ++tries)
    {
        m_resp[0] = reportId;
        err       = m_usb.hidFeature(false, reportId, m_resp.data(), pad, error);
        if (err != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (m_resp[1] == 0xFF) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    error = "Ziel meldet die Routine nicht als fertig (Zeitueberschreitung)";
    return ESP_ERR_TIMEOUT;
}

// ---------------------------------------------------------------------------
// Oeffentliche Schnittstelle
// ---------------------------------------------------------------------------

esp_err_t B003Link::begin(std::string& error)
{
    error.clear();

    if (!m_usb.hasHid())
    {
        error = "kein HID-Interface am Ziel - haengt ein rv003usb-Bootloader am USB-C?";
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Kleinster gemeinsamer Nenner. Groessere Bootloader melden mehr; das
    // Verhandeln kommt erst, wenn es ein Ziel dafuer gibt.
    m_scratchpad = DEFAULT_SCRATCHPAD;
    m_dataSize   = DEFAULT_DATA_SIZE;

    m_cmd.assign(m_scratchpad, 0);
    m_resp.assign(std::max<size_t>(m_scratchpad, 64), 0);

    ESP_LOGI(TAG, "B003-Link bereit, Scratchpad %u Byte",
             static_cast<unsigned>(m_scratchpad));
    return ESP_OK;
}

esp_err_t B003Link::readBlob(uint32_t address, size_t size, uint8_t* out, std::string& error)
{
    error.clear();
    if (size == 0) return ESP_OK;

    while (size > 0)
    {
        const bool   wordAligned = ((address & 3) == 0) && (size >= 4);
        const uint8_t* blob      = wordAligned ? WORD_WISE_READ_BLOB : BYTE_WISE_READ_BLOB;
        const size_t blobLen     = wordAligned ? sizeof(WORD_WISE_READ_BLOB)
                                               : sizeof(BYTE_WISE_READ_BLOB);

        size_t chunk = std::min(size, m_dataSize);
        if (wordAligned) chunk &= ~static_cast<size_t>(3);
        if (chunk == 0) chunk = 1;

        resetOp();
        writeOpArb(blob, blobLen);
        writeOp4(address);
        writeOp4(static_cast<uint32_t>(chunk));

        /*
         * Die gelesenen Daten liegen HINTER den beiden Parametern, also bei
         * m_place. Die Disassembly zeigt es: der Stub holt Adresse und Laenge
         * aus base+52 und base+56, macht dann `addi x14,x14,8` und schreibt ab
         * base+60.
         *
         * Nicht zu verwechseln mit dem CH5xx-Zweig in pgm-b003fun.c, der
         * `commandplace-8` nimmt — der benutzt einen anderen Stub mit anderem
         * Layout. Am Geraet nachgemessen (2026-09-12).
         */
        const size_t dataAt = m_place;

        const esp_err_t err = commit(0, chunk, error);
        if (err != ESP_OK) return err;

        std::memcpy(out, m_resp.data() + dataAt, chunk);

        out += chunk;
        address += static_cast<uint32_t>(chunk);
        size -= chunk;
    }

    return ESP_OK;
}

esp_err_t B003Link::readWord(uint32_t address, uint32_t& value, std::string& error)
{
    uint8_t buf[4] = {};
    const esp_err_t err = readBlob(address, sizeof(buf), buf, error);
    if (err != ESP_OK) return err;
    std::memcpy(&value, buf, 4);
    return ESP_OK;
}

esp_err_t B003Link::identify(ChipInfo& info, std::string& error)
{
    info = ChipInfo{};

    uint32_t obr = 0;
    uint32_t wrpr = 0;

    esp_err_t err = readWord(ADDR_OBR, obr, error);
    if (err != ESP_OK) return err;
    err = readWord(ADDR_WRPR, wrpr, error);
    if (err != ESP_OK) return err;

    // Aus B003DetermineChipType: Bit 1 im OBR oder ein nicht-volles WRPR
    // bedeuten Leseschutz.
    info.readProtected = ((obr & 2) != 0) || (wrpr != 0xFFFFFFFFu);

    err = readWord(ADDR_CHIPID_LOW, info.idLow, error);
    if (err != ESP_OK) return err;

    if (info.idLow == 0xFFFFFFFFu || info.idLow == 0xE339E339u)
    {
        err = readWord(ADDR_CHIPID_HIGH, info.idHigh, error);
        if (err != ESP_OK) return err;

        switch (info.idHigh >> 20)
        {
            case 0x002: info.name = "CH32V002"; break;
            case 0x004: info.name = "CH32V004"; break;
            case 0x005: info.name = "CH32V005"; break;
            case 0x006: info.name = "CH32V006"; break;
            case 0x007: info.name = "CH32V007"; break;
            case 0x033: info.name = "CH32X033"; break;
            default:    info.name = "unbekannt"; break;
        }
    }
    else
    {
        // Der CH32V003 traegt seine Kennung direkt in 0x1FFFF7C4.
        info.name = ((info.idLow >> 20) == 0x003) ? "CH32V003" : "CH32V (unbekannte Variante)";
    }

    ESP_LOGI(TAG, "Chip: %s (0x%08lX / 0x%08lX)%s", info.name.c_str(),
             static_cast<unsigned long>(info.idLow),
             static_cast<unsigned long>(info.idHigh),
             info.readProtected ? ", lesegeschuetzt" : "");
    return ESP_OK;
}
