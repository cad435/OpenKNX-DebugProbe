#include "Uf2Flasher.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* TAG = "uf2";

/// Ab diesem Sektor werden die Blöcke abgelegt. Die Lage ist beliebig, das
/// Boot-ROM prüft jeden geschriebenen Sektor; Sektor 0 lassen wir trotzdem aus.
constexpr uint32_t FIRST_SECTOR = 1;

#pragma pack(push, 1)
struct Uf2Block
{
    uint32_t magicStart0;
    uint32_t magicStart1;
    uint32_t flags;
    uint32_t targetAddr;
    uint32_t payloadSize;
    uint32_t blockNo;
    uint32_t numBlocks;
    uint32_t fileSizeOrFamilyId;
    uint8_t  data[476];
    uint32_t magicEnd;
};
#pragma pack(pop)

static_assert(sizeof(Uf2Block) == 512, "UF2-Block muss 512 Byte gross sein");

std::string describe(const char* format, ...) __attribute__((format(printf, 1, 2)));

std::string describe(const char* format, ...)
{
    char    buffer[320];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return std::string(buffer);
}

}  // namespace

bool Uf2Flasher::checkPrologue(const uint8_t* head,
                               size_t         headSize,
                               size_t         totalSize,
                               std::string&   error)
{
    if (head == nullptr || totalSize == 0)
    {
        error = "leere Datei";
        return false;
    }
    if (headSize < sizeof(uint32_t))
    {
        error = describe("nur %u Byte empfangen, das reicht nicht fuer einen UF2-Block",
                         static_cast<unsigned>(headSize));
        return false;
    }

    uint32_t magic = 0;
    std::memcpy(&magic, head, sizeof(magic));

    // Zuerst das Magic, dann die Groesse. Umgekehrt bekaeme der haeufigste
    // Fall — eine versehentlich geschickte firmware.bin — die Meldung
    // "kein Vielfaches von 512", und die zeigt in die falsche Richtung.
    if (magic != MAGIC_START0)
    {
        error = describe("kein UF2-Magic (0x%08lX) am Dateianfang, sondern 0x%08lX, "
                         "%u Byte empfangen - das sieht nach einer .bin aus. In der "
                         "platformio.ini \"@$BUILD_DIR/${PROGNAME}.uf2\" statt \"@$SOURCE\" "
                         "verwenden.",
                         static_cast<unsigned long>(MAGIC_START0),
                         static_cast<unsigned long>(magic),
                         static_cast<unsigned>(totalSize));
        return false;
    }
    if (totalSize % BLOCK_SIZE != 0)
    {
        error = describe("UF2-Magic ist da, aber die Groesse %u ist kein Vielfaches von 512",
                         static_cast<unsigned>(totalSize));
        return false;
    }

    error.clear();
    return true;
}

bool Uf2Flasher::validate(const uint8_t* data, size_t size, Info& info, std::string& error)
{
    info = Info{};

    if (!checkPrologue(data, size, size, error)) return false;

    const uint32_t total = static_cast<uint32_t>(size / BLOCK_SIZE);

    for (uint32_t index = 0; index < total; ++index)
    {
        Uf2Block block;
        std::memcpy(&block, data + static_cast<size_t>(index) * BLOCK_SIZE, sizeof(block));

        if (block.magicStart0 != MAGIC_START0 || block.magicStart1 != MAGIC_START1 ||
            block.magicEnd != MAGIC_END)
        {
            error = describe("Block %lu: kein gueltiger UF2-Block",
                             static_cast<unsigned long>(index));
            return false;
        }
        if (block.payloadSize > MAX_PAYLOAD)
        {
            error = describe("Block %lu: payloadSize %lu zu gross",
                             static_cast<unsigned long>(index),
                             static_cast<unsigned long>(block.payloadSize));
            return false;
        }
        if (block.numBlocks != total)
        {
            error = describe("Block %lu meldet %lu Bloecke, Datei hat %lu",
                             static_cast<unsigned long>(index),
                             static_cast<unsigned long>(block.numBlocks),
                             static_cast<unsigned long>(total));
            return false;
        }
        if (block.blockNo != index)
        {
            error = describe("Blockkette unterbrochen: erwartet %lu, gelesen %lu",
                             static_cast<unsigned long>(index),
                             static_cast<unsigned long>(block.blockNo));
            return false;
        }

        const uint32_t family =
            ((block.flags & FLAG_FAMILY_ID) != 0) ? block.fileSizeOrFamilyId : 0;

        if (index == 0)
        {
            info.familyId     = family;
            info.firstAddress = block.targetAddr;
        }
        else if (family != info.familyId)
        {
            error = "Family-ID wechselt innerhalb der Datei";
            return false;
        }

        info.lastAddress = block.targetAddr + block.payloadSize;
        info.payloadBytes += block.payloadSize;
    }

    if (info.familyId != FAMILY_RP2040)
    {
        error = describe("Family-ID 0x%08lX ist nicht RP2040 (0x%08lX)",
                         static_cast<unsigned long>(info.familyId),
                         static_cast<unsigned long>(FAMILY_RP2040));
        return false;
    }

    info.blocks = total;
    error.clear();
    return true;
}

esp_err_t Uf2Flasher::flash(UsbTarget&     target,
                            const uint8_t* data,
                            size_t         size,
                            uint32_t&      blocksWritten,
                            std::string&   error)
{
    blocksWritten = 0;

    if (!target.isMscReady())
    {
        error = "Ziel ist nicht im BOOTSEL-Modus";
        return ESP_ERR_INVALID_STATE;
    }
    if (target.mscSectorSize() != BLOCK_SIZE)
    {
        error = describe("Sektorgroesse %lu statt 512",
                         static_cast<unsigned long>(target.mscSectorSize()));
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t total = static_cast<uint32_t>(size / BLOCK_SIZE);
    ESP_LOGI(TAG, "writing %lu blocks", static_cast<unsigned long>(total));

    for (uint32_t index = 0; index < total; ++index)
    {
        const uint8_t* block = data + static_cast<size_t>(index) * BLOCK_SIZE;
        const esp_err_t err  = target.writeSector(FIRST_SECTOR + index, block, BLOCK_SIZE);

        if (err != ESP_OK)
        {
            // Nach dem letzten Block startet das Boot-ROM sofort neu — dass der
            // Transfer dabei abbricht, ist Erfolg und kein Fehler.
            if (index + 1 >= total)
            {
                ESP_LOGI(TAG, "target rebooted on last block (expected)");
                break;
            }
            error = describe("Block %lu von %lu fehlgeschlagen: %s",
                             static_cast<unsigned long>(index),
                             static_cast<unsigned long>(total), esp_err_to_name(err));
            ESP_LOGE(TAG, "%s", error.c_str());
            return err;
        }

        blocksWritten = index + 1;

        if ((index % 256) == 0)
        {
            ESP_LOGI(TAG, "  %lu/%lu", static_cast<unsigned long>(index),
                     static_cast<unsigned long>(total));
        }
    }

    error.clear();
    ESP_LOGI(TAG, "done, %lu blocks written", static_cast<unsigned long>(blocksWritten));
    return ESP_OK;
}
