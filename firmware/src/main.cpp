/**
 * OpenKNX DebugProbe — application entry point.
 *
 * Stufe 1: Probe ins Netz bringen und selbst aktualisierbar machen.
 * Stufe 2: USB-Host aktivieren und das angesteckte Zielgerät erkennen.
 *
 * Reihenfolge ist Absicht: WLAN und HTTP-Server stehen, bevor der USB-Host
 * angefasst wird. Hängt der USB-Stack, bleibt die Probe trotzdem per OTA
 * erreichbar. Stürzt er ab, greift der Rollback — deshalb wird das Image erst
 * NACH dem USB-Start bestätigt.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "OtaUpdater.hpp"
#include "Rfc2217Server.hpp"
#include "SerialBridge.hpp"
#include "Settings.hpp"
#include "StatusLed.hpp"
#include "Uf2Flasher.hpp"
#include "UsbTarget.hpp"
#include "AssetStore.hpp"
#include "B003Link.hpp"
#include "WebServer.hpp"
#include "WiFiManager.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* TAG = "main";

/// Seconds of healthy operation before a freshly flashed image is confirmed.
constexpr uint32_t PROBATION_SECONDS = 10;

/// Obergrenze fuer ein Kommando aus der Web-Konsole. Reichlich fuer alles, was
/// man tippt, und schuetzt davor, dass ein versehentlicher Datei-Upload auf
/// diesem Endpunkt landet.
constexpr size_t MAX_COMMAND_BYTES = 512;

struct AppContext
{
    Settings*     settings;
    WiFiManager*  wifi;
    OtaUpdater*   ota;
    UsbTarget*    usb;
    SerialBridge*  bridge;
    Rfc2217Server* rfc2217;
    StatusLed*     led;
};

/// Standardpin der RGB-LED auf den SuperMini-Varianten. Nur eine Vermutung —
/// deshalb in NVS ueberschreibbar, siehe handleLed().
constexpr int DEFAULT_LED_GPIO = 48;

/// NVS-Schluessel fuer den gefundenen Pin.
constexpr const char* KEY_LED_GPIO = "led_gpio";

/**
 * Solange ein solches Objekt lebt, blinkt die LED orange.
 *
 * RAII, weil handleFlash() ein halbes Dutzend Ausstiegspunkte hat. Ein
 * vergessenes Zuruecksetzen liesse die Warnung "nicht abstecken" fuer immer
 * stehen — und genau dann glaubt sie irgendwann niemand mehr.
 */
class FlashingIndicator
{
public:
    explicit FlashingIndicator(StatusLed* led) : m_led(led)
    {
        if (m_led != nullptr) m_led->setMode(StatusLed::Mode::Flashing);
    }
    ~FlashingIndicator()
    {
        // Zurueck auf einen aus dem Zustand abgeleiteten Modus kuemmert sich
        // die Aufsichtsschleife; hier genuegt es, die Warnung zu beenden.
        if (m_led != nullptr) m_led->setMode(StatusLed::Mode::Idle);
    }

    FlashingIndicator(const FlashingIndicator&)            = delete;
    FlashingIndicator& operator=(const FlashingIndicator&) = delete;

private:
    StatusLed* m_led;
};

AppContext g_ctx {};

std::string jsonString(const std::string& value)
{
    std::string out = "\"";
    for (const char c : value)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::string hex16(uint16_t value)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%04X", value);
    return std::string("\"") + buf + "\"";
}

std::string hex8(uint8_t value)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%02X", value);
    return std::string("\"") + buf + "\"";
}

/// Escapt beliebige Bytes fuer ein JSON-String-Literal. Die Konsole des Ziels
/// enthaelt Steuerzeichen und ANSI-Sequenzen; die duerfen den JSON-Rahmen nicht
/// zerlegen.
std::string jsonEscapeBytes(const uint8_t* data, size_t length)
{
    static const char* HEX = "0123456789abcdef";

    std::string out;
    out.reserve(length + length / 4);

    for (size_t i = 0; i < length; ++i)
    {
        const uint8_t c = data[i];
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20 || c == 0x7F)
                {
                    out += "\\u00";
                    out += HEX[(c >> 4) & 0x0F];
                    out += HEX[c & 0x0F];
                }
                else
                {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

std::string hex32(uint32_t value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(value));
    return std::string("\"") + buf + "\"";
}

/**
 * Die fertigen Zeilen für die platformio.ini des Zielprojekts.
 *
 * Bewusst hier gebaut und nicht im Browser: `/api/snippet` und das Web-UI
 * sollen garantiert dasselbe liefern. Welcher Upload-Weg gilt, haengt am Ziel —
 * ein RP2040 wird per UF2 ueber HTTP geflasht, alles andere per esptool ueber
 * RFC2217.
 */
struct Snippet
{
    std::string comment;
    std::string monitor;
    std::string upload;
};

Snippet buildSnippet(const std::string& hostname, const UsbTarget& usb)
{
    const std::string host = hostname + ".local";

    Snippet snippet;
    snippet.monitor = "monitor_port  = socket://" + host + ":2323\n"
                      "monitor_speed = 115200";

    if (usb.state() != UsbTarget::State::Connected)
    {
        snippet.comment = "kein Zielgeraet angesteckt";
        return snippet;
    }

    const UsbTarget::DeviceInfo info = usb.device();

    char ids[24];
    snprintf(ids, sizeof(ids), "%04X:%04X", info.vid, info.pid);
    snippet.comment = info.description + " (" + ids + ")";

    if (info.vid == 0x2E8A)
    {
        // Ausdruecklich NICHT $SOURCE: PlatformIO loest das bei
        // upload_protocol = custom auf die firmware.bin auf, und die lehnt
        // /api/flash zu Recht ab. Die Klammern um ${PROGNAME} sind Pflicht --
        // "$PROGNAME.uf2" liest SCons als Attributzugriff und bricht ab.
        // --fail-with-body statt -f, damit der Grund einer Ablehnung im
        // PlatformIO-Log steht und nicht nur "curl: (22)".
        snippet.upload = "upload_protocol = custom\n"
                         "upload_command  = curl --fail-with-body --data-binary "
                         "\"@$BUILD_DIR/${PROGNAME}.uf2\" http://" +
                         host + "/api/flash";
    }
    else
    {
        // 460800 ist am Geraet mehrfach reproduziert; 921600 lief einmal
        // schneller und brach danach einmal ab.
        snippet.upload = "upload_port  = rfc2217://" + host + ":4000\n"
                         "upload_speed = 460800";
    }
    return snippet;
}

std::string snippetJson(const Snippet& s)
{
    auto asJson = [](const std::string& v) {
        return "\"" + jsonEscapeBytes(reinterpret_cast<const uint8_t*>(v.data()), v.size()) + "\"";
    };

    std::string json = "{";
    json += "\"comment\":" + asJson(s.comment);
    json += ",\"monitor\":" + asJson(s.monitor);
    json += ",\"upload\":" + asJson(s.upload);
    json += "}";
    return json;
}

/**
 * Warum die Probe das letzte Mal gestartet ist.
 *
 * Der Grund ueberlebt den Neustart im RTC-Speicher, und genau das macht ihn
 * wertvoll: ein Zielgeraet mit hohem Einschaltstrom kann die Probe per
 * Brownout zuruecksetzen, und hinterher ist nicht mehr zu sehen, ob es der
 * Strom war oder ein Absturz. `brownout` beantwortet das, ohne ein Bauteil und
 * ohne einen Blick auf die serielle Konsole.
 */
const char* resetReasonName()
{
    switch (esp_reset_reason())
    {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_SW:        return "software";      // z. B. nach einem OTA
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        case ESP_RST_EXT:       return "extern";
        default:                return "unbekannt";
    }
}

std::string targetJson(const UsbTarget& usb)
{
    std::string json = "{";
    json += "\"state\":" + jsonString(usb.stateName());
    json += ",\"since_s\":" + std::to_string(usb.secondsInState());

    /*
     * Was fuer dieses Ziel moeglich ist, entscheidet die Probe - nicht das
     * Web-UI. Sonst muesste die Oberflaeche die Tabelle aus UsbTarget.hpp
     * nachbauen und liefe bei jedem neuen Geraetetyp hinterher. `*_how`
     * traegt bei `can_* == false` den Grund und steht im UI als Hinweis.
     */
    const UsbTarget::ResetSupport run  = usb.resetSupport(UsbTarget::ResetMode::Run);
    const UsbTarget::ResetSupport boot = usb.resetSupport(UsbTarget::ResetMode::Bootloader);
    json += ",\"can_reset\":" + std::string(run.possible ? "true" : "false");
    json += ",\"reset_how\":" + jsonString(run.how);
    json += ",\"can_bootmode\":" + std::string(boot.possible ? "true" : "false");
    json += ",\"bootmode_how\":" + jsonString(boot.how);

    if (usb.state() == UsbTarget::State::Connected)
    {
        const UsbTarget::DeviceInfo info = usb.device();
        json += ",\"description\":" + jsonString(info.description);
        json += ",\"vid\":" + hex16(info.vid);
        json += ",\"pid\":" + hex16(info.pid);
        json += ",\"kind\":" + jsonString(UsbTarget::kindName(info.kind));
        json += ",\"driver\":" + jsonString(UsbTarget::driverName(info.driver));
        json += ",\"manufacturer\":" + jsonString(info.manufacturer);
        json += ",\"product\":" + jsonString(info.product);
        json += ",\"serial\":" + jsonString(info.serial);
        json += ",\"speed\":" + jsonString(info.speed);
        json += ",\"class\":" + hex8(info.deviceClass);
        json += ",\"interface_class\":" + hex8(info.interfaceClass);
        json += ",\"serial_open\":" + std::string(usb.isSerialOpen() ? "true" : "false");
        json += ",\"baud\":" + std::to_string(usb.baudRate());
        // Steuert, ob das Web-UI den UF2-Bereich ueberhaupt anzeigt: nur ein
        // RP2040 laesst sich per UF2 flashen, egal ob er gerade laeuft oder
        // schon im BOOTSEL steht.
        json += ",\"uf2_capable\":" + std::string((info.vid == 0x2E8A) ? "true" : "false");
        json += ",\"msc_ready\":" + std::string(usb.isMscReady() ? "true" : "false");
        json += ",\"picoboot\":" + std::string(usb.hasPicoboot() ? "true" : "false");
        if (usb.isMscReady())
        {
            json += ",\"msc_kb\":" +
                    std::to_string((static_cast<uint64_t>(usb.mscSectorCount()) *
                                    usb.mscSectorSize()) / 1024);
        }
    }
    json += "}";
    return json;
}

std::string consoleJson(const SerialBridge& bridge, const Rfc2217Server& rfc)
{
    std::string json = "{";
    json += "\"port\":" + std::to_string(bridge.port());
    json += ",\"client\":" + jsonString(bridge.clientAddress());
    json += ",\"buffer\":" + std::to_string(bridge.bufferSize());
    json += ",\"rx\":" + std::to_string(bridge.bytesFromTarget());
    json += ",\"tx\":" + std::to_string(bridge.bytesToTarget());
    json += ",\"rfc2217_port\":" + std::to_string(rfc.port());
    json += ",\"rfc2217_client\":" + std::string(rfc.hasClient() ? "true" : "false");
    json += "}";
    return json;
}

esp_err_t handleStatus(httpd_req_t* req)
{
    const auto* ctx = static_cast<const AppContext*>(req->user_ctx);

    std::string json = "{";
    json += "\"name\":" + jsonString(ctx->wifi->hostname()) + ",";
    json += "\"version\":" + jsonString(ctx->ota->version()) + ",";
    json += "\"build\":" + jsonString(ctx->ota->buildTimestamp()) + ",";
    json += "\"partition\":" + jsonString(ctx->ota->runningPartition()) + ",";
    json += "\"pending_verify\":" + std::string(ctx->ota->isPendingVerify() ? "true" : "false") + ",";
    json += "\"reset_reason\":" + jsonString(resetReasonName()) + ",";
    json += "\"wifi\":" + jsonString(ctx->wifi->stateName()) + ",";
    json += "\"ssid\":" + jsonString(ctx->wifi->ssid()) + ",";
    json += "\"rssi\":" + std::to_string(ctx->wifi->rssi()) + ",";
    json += "\"ip\":" + jsonString(ctx->wifi->ip()) + ",";
    json += "\"led\":{\"gpio\":" + std::to_string(static_cast<int>(ctx->led->pin())) +
            ",\"mode\":" + jsonString(StatusLed::modeName(ctx->led->mode())) + "},";
    json += "\"radio_awake\":" +
            std::string(ctx->wifi->isRadioAwake() ? "true" : "false") + ",";
    json += "\"target\":" + targetJson(*ctx->usb) + ",";
    json += "\"console\":" + consoleJson(*ctx->bridge, *ctx->rfc2217) + ",";
    json += "\"snippet\":" +
            snippetJson(buildSnippet(ctx->wifi->hostname(), *ctx->usb)) + ",";
    json += "\"uptime_s\":" + std::to_string(esp_timer_get_time() / 1000000) + ",";
    json += "\"heap_free\":" + std::to_string(esp_get_free_heap_size());
    json += "}";

    return WebServer::sendJson(req, json);
}

/**
 * POST /api/flash — UF2 für das Zielgerät.
 *
 * Der komplette Inhalt wird erst im PSRAM gepuffert und vollständig geprüft,
 * bevor der erste Sektor ans Ziel geht. Bei 4 MB Flash auf der Probe gibt es
 * keine Staging-Partition, dafür 2 MB PSRAM — eine typische RP2040-UF2 von
 * ~1,4 MB passt hinein.
 */
esp_err_t handleFlash(httpd_req_t* req)
{
    auto* ctx = static_cast<AppContext*>(req->user_ctx);

    char  query[64] = {};
    char  value[16] = {};
    bool  validateOnly = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "validate_only", value, sizeof(value)) == ESP_OK)
    {
        validateOnly = (value[0] == '1' || value[0] == 't');
    }

    const size_t total = static_cast<size_t>(req->content_len);
    if (total == 0)
    {
        return WebServer::sendStatus(req, "400 Bad Request",
                                     R"({"error":"leerer Rumpf, keine Datei angekommen"})");
    }

    // Erst den ersten Block lesen und beurteilen, dann Speicher holen. Das
    // kostet nichts und macht die haeufigste Fehlbedienung sichtbar: eine
    // falsch konfigurierte platformio.ini schickt die firmware.bin hierher.
    // Die Groessenpruefung kommt bewusst erst danach, siehe checkPrologue().
    uint8_t      head[Uf2Flasher::BLOCK_SIZE];
    const size_t headWanted = (total < sizeof(head)) ? total : sizeof(head);
    size_t       headGot    = 0;
    while (headGot < headWanted)
    {
        const int got = httpd_req_recv(req, reinterpret_cast<char*>(head) + headGot,
                                       headWanted - headGot);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0)
        {
            return WebServer::sendStatus(req, "400 Bad Request",
                                         R"({"error":"Uebertragung abgebrochen"})");
        }
        headGot += static_cast<size_t>(got);
    }

    std::string error;
    if (!Uf2Flasher::checkPrologue(head, headGot, total, error))
    {
        ESP_LOGE(TAG, "UF2 abgelehnt: %s", error.c_str());
        return WebServer::sendStatus(req, "400 Bad Request",
                                     R"({"error":)" + jsonString(error) + "}");
    }

    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM));
    if (buffer == nullptr) buffer = static_cast<uint8_t*>(heap_caps_malloc(total, MALLOC_CAP_DEFAULT));
    if (buffer == nullptr)
    {
        return WebServer::sendStatus(req, "413 Payload Too Large",
                                     R"({"error":"kein Speicher fuer die UF2"})");
    }

    std::memcpy(buffer, head, headGot);

    size_t received = headGot;
    while (received < total)
    {
        const int got = httpd_req_recv(req, reinterpret_cast<char*>(buffer) + received,
                                       total - received);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0)
        {
            heap_caps_free(buffer);
            return WebServer::sendStatus(req, "400 Bad Request",
                                         R"({"error":"Uebertragung abgebrochen"})");
        }
        received += static_cast<size_t>(got);
    }

    Uf2Flasher::Info info;
    if (!Uf2Flasher::validate(buffer, total, info, error))
    {
        ESP_LOGE(TAG, "UF2 abgelehnt: %s", error.c_str());
        heap_caps_free(buffer);
        return WebServer::sendStatus(req, "400 Bad Request",
                                     R"({"error":)" + jsonString(error) + "}");
    }

    std::string summary = "{";
    summary += "\"blocks\":" + std::to_string(info.blocks);
    summary += ",\"payload_bytes\":" + std::to_string(info.payloadBytes);
    summary += ",\"first_address\":" + hex32(info.firstAddress);
    summary += ",\"family\":" + hex32(info.familyId);

    if (validateOnly)
    {
        heap_caps_free(buffer);
        return WebServer::sendJson(req, summary + R"(,"status":"valid"})");
    }

    // Schritt 3 des Ablaufs aus docs/architecture.md: das Ziel selbst nach
    // BOOTSEL schicken. Erst wenn der Touch nicht greift, muss jemand an die
    // Taster. Bewusst NACH der Validierung — eine kaputte Datei darf das Ziel
    // nicht aus seiner Anwendung reissen.
    if (!ctx->usb->isMscReady())
    {
        ctx->bridge->note("1200-Baud-Touch, Ziel soll nach BOOTSEL");

        if (ctx->usb->enterBootsel(error) != ESP_OK)
        {
            ESP_LOGE(TAG, "BOOTSEL nicht erreicht: %s", error.c_str());
            ctx->bridge->note("BOOTSEL nicht erreicht, Flashen abgebrochen");
            heap_caps_free(buffer);
            return WebServer::sendStatus(req, "409 Conflict",
                                         R"({"error":)" + jsonString(error) + "}");
        }
    }

    // Ab hier darf niemand das Kabel ziehen — die LED sagt es.
    FlashingIndicator warn(ctx->led);
    ctx->bridge->note("Flashen gestartet");

    uint32_t        blocks = 0;
    const esp_err_t err    = Uf2Flasher::flash(*ctx->usb, buffer, total, blocks, error);
    heap_caps_free(buffer);

    if (err != ESP_OK)
    {
        ctx->bridge->note("Flashen fehlgeschlagen");
        return WebServer::sendStatus(req, "502 Bad Gateway",
                                     R"({"error":)" + jsonString(error) + "}");
    }

    ctx->bridge->note("Flashen fertig, Ziel startet neu");
    return WebServer::sendJson(req, summary + ",\"written\":" + std::to_string(blocks) +
                                        R"(,"status":"ok"})");
}

/**
 * Gemeinsamer Rumpf von POST /api/reset und POST /api/bootmode.
 *
 * Beides ist derselbe Vorgang mit unterschiedlichem Ziel, und welcher Griff
 * dafuer noetig ist, weiss `UsbTarget` — ein RP2040 braucht den
 * 1200-Baud-Touch, ein ESP32 hinter einem Bridge-Chip eine DTR/RTS-Folge.
 * Deshalb steht hier keine Fallunterscheidung, sondern nur die HTTP-Huelle.
 *
 * `method` in der Antwort nennt das tatsaechlich verwendete Verfahren. Das ist
 * kein Schmuck: beim Bootmodus sieht man daran, ob getouched wurde oder ob das
 * Ziel ohnehin schon im BOOTSEL stand.
 */
esp_err_t handleTargetReset(httpd_req_t* req, UsbTarget::ResetMode mode)
{
    auto* ctx = static_cast<AppContext*>(req->user_ctx);

    const bool isRun = (mode == UsbTarget::ResetMode::Run);
    const char* what = isRun ? "Neustart" : "Bootmodus";

    // Vor dem Umschalten abfragen: danach meldet ein RP2040 im BOOTSEL
    // "steht bereits dort" und die Antwort verschwiege, was gerade passiert ist.
    const UsbTarget::ResetSupport support = ctx->usb->resetSupport(mode);

    /*
     * Die Marke geht VOR den Eingriff in den Mitschnitt.
     *
     * resetTarget() haelt die Steuerleitungen einige hundert Millisekunden —
     * das Ziel resettet und schreibt seinen ROM-Boot-Log noch waehrend des
     * Aufrufs in den Ringpuffer. Eine Marke danach stuende im Protokoll hinter
     * ihrer eigenen Wirkung und liesse sich nicht mehr als Ursache lesen.
     * handleFlash() macht es mit "Flashen gestartet" genauso.
     *
     * Nur bei `possible`: sonst traegt `how` nicht das Verfahren, sondern den
     * Grund, warum es nicht geht — als Ankuendigung waere das irrefuehrend.
     */
    if (support.possible)
    {
        ESP_LOGI(TAG, "%s: %s", what, support.how.c_str());
        const std::string line =
            (isRun ? "starte Ziel neu: " : "schicke Ziel in den Bootmodus: ") + support.how;
        ctx->bridge->note(line.c_str());
    }

    std::string error;

    /*
     * Sonderfall CH32V003 im rv003usb-Bootloader: der Weg zurueck in die
     * Anwendung laeuft ueber eine hochgeladene Routine, also ueber
     * `ch32_flasher`. `UsbTarget` kann das nicht selbst erledigen — die
     * Komponente haengt an ihm, nicht umgekehrt.
     */
    if (isRun && ctx->usb->device().kind == UsbTarget::Kind::HidBootloader)
    {
        B003Link link(*ctx->usb);
        if (link.begin(error) == ESP_OK && link.bootUserCode(error) == ESP_OK)
        {
            ctx->bridge->note("Ziel: Anwendungscode gestartet");
            return WebServer::sendJson(
                req, R"({"status":"reset","method":"rv003usb: Anwendungscode gestartet"})");
        }
        ESP_LOGE(TAG, "rv003usb-Boot fehlgeschlagen: %s", error.c_str());
        return WebServer::sendStatus(req, "502 Bad Gateway",
                                     R"({"error":)" + jsonString(error) + "}");
    }

    if (ctx->usb->resetTarget(mode, error) != ESP_OK)
    {
        ESP_LOGE(TAG, "%s nicht moeglich: %s", what, error.c_str());

        // Angekuendigt und dann doch gescheitert — das muss im Mitschnitt
        // stehen, sonst deutet die Marke oben auf einen Neustart, den es nie
        // gab. note() kuerzt lange Begruendungen; vollstaendig stehen sie in
        // der HTTP-Antwort.
        if (support.possible)
        {
            const std::string bad =
                std::string(isRun ? "Neustart" : "Bootmodus") + " fehlgeschlagen: " + error;
            ctx->bridge->note(bad.c_str());
        }

        return WebServer::sendStatus(req, "409 Conflict",
                                     R"({"error":)" + jsonString(error) + "}");
    }

    // Kein Erfolgsvermerk: die Marke oben steht schon da, und der Boot-Log des
    // Ziels direkt darunter ist der eigentliche Beweis.

    std::string json = "{\"status\":";
    json += isRun ? R"("reset")" : R"("bootmode")";
    json += ",\"method\":" + jsonString(support.how);
    json += ",\"msc_ready\":" + std::string(ctx->usb->isMscReady() ? "true" : "false");
    if (ctx->usb->isMscReady())
    {
        json += ",\"msc_kb\":" +
                std::to_string(static_cast<uint64_t>(ctx->usb->mscSectorCount()) *
                               ctx->usb->mscSectorSize() / 1024);
    }
    json += "}";
    return WebServer::sendJson(req, json);
}

/**
 * GET /api/ch32 — Chip-Kennung eines CH32V003 im rv003usb-Bootloader.
 *
 * Erster Schritt des portierten minichlink-Protokolls: Transportschicht und
 * Speicherlesen ueber eine im Ziel ausgefuehrte RISC-V-Routine. Flashen kommt
 * darauf auf.
 */
esp_err_t handleCh32(httpd_req_t* req)
{
    auto* ctx = static_cast<AppContext*>(req->user_ctx);

    B003Link    link(*ctx->usb);
    std::string error;

    if (link.begin(error) != ESP_OK)
    {
        return WebServer::sendStatus(req, "409 Conflict",
                                     R"({"error":)" + jsonString(error) + "}");
    }

    B003Link::ChipInfo info;
    if (link.identify(info, error) != ESP_OK)
    {
        return WebServer::sendStatus(req, "502 Bad Gateway",
                                     R"({"error":)" + jsonString(error) + "}");
    }

    // Diagnose: die rohen Antwortbytes des letzten Lesevorgangs. Solange der
    // Port nicht verifiziert ist, ist das die einzige ehrliche Auskunft.
    std::string dump;
    {
        const std::vector<uint8_t>& r = link.lastResponse();
        char b[4];
        for (size_t i = 0; i < r.size() && i < 72; ++i)
        {
            snprintf(b, sizeof(b), "%02X", r[i]);
            dump += b;
        }
    }

    std::string json = "{\"chip\":" + jsonString(info.name);
    json += ",\"raw\":" + jsonString(dump);
    json += ",\"id_low\":" + hex32(info.idLow);
    json += ",\"id_high\":" + hex32(info.idHigh);
    json += ",\"read_protected\":" + std::string(info.readProtected ? "true" : "false");
    json += ",\"scratchpad\":" + std::to_string(link.scratchpadSize());
    json += "}";
    return WebServer::sendJson(req, json);
}

/**
 * POST /api/ch32/flash — Abbild in den Flash eines CH32V003 schreiben.
 *
 * Body ist die rohe `.bin`, Startadresse `?addr=` (Standard 0x08000000).
 * Das Ziel muss im rv003usb-Bootloader stehen; `/api/bootmode` bringt es
 * dorthin.
 */
esp_err_t handleCh32Flash(httpd_req_t* req)
{
    auto* ctx = static_cast<AppContext*>(req->user_ctx);

    uint32_t address = 0x08000000u;
    char     query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        char value[24];
        if (httpd_query_key_value(query, "addr", value, sizeof(value)) == ESP_OK)
        {
            address = static_cast<uint32_t>(strtoul(value, nullptr, 0));
        }
    }

    const size_t total = static_cast<size_t>(req->content_len);
    if (total == 0)
    {
        return WebServer::sendStatus(req, "400 Bad Request", R"({"error":"leerer Body"})");
    }
    // 16 KB Flash beim CH32V003; mehr kann nicht gemeint sein.
    if (total > 16 * 1024)
    {
        return WebServer::sendStatus(req, "413 Payload Too Large",
                                     R"({"error":"groesser als der Flash des CH32V003"})");
    }

    std::vector<uint8_t> image(total);
    size_t               got = 0;
    while (got < total)
    {
        const int r = httpd_req_recv(req, reinterpret_cast<char*>(image.data() + got),
                                     total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0)
        {
            return WebServer::sendStatus(req, "400 Bad Request",
                                         R"({"error":"Uebertragung abgebrochen"})");
        }
        got += static_cast<size_t>(r);
    }

    B003Link    link(*ctx->usb);
    std::string error;
    if (link.begin(error) != ESP_OK)
    {
        return WebServer::sendStatus(req, "409 Conflict",
                                     R"({"error":)" + jsonString(error) + "}");
    }

    ctx->bridge->note("CH32V003: Flashen gestartet");

    size_t          written = 0;
    const esp_err_t err     = link.flashImage(address, image.data(), image.size(), error, written);
    if (err != ESP_OK)
    {
        ctx->bridge->note("CH32V003: Flashen fehlgeschlagen");
        ESP_LOGE(TAG, "CH32-Flash fehlgeschlagen nach %u Byte: %s",
                 static_cast<unsigned>(written), error.c_str());
        return WebServer::sendStatus(req, "502 Bad Gateway",
                                     R"({"error":)" + jsonString(error) +
                                         ",\"written\":" + std::to_string(written) + "}");
    }

    ctx->bridge->note("CH32V003: Flashen fertig");
    std::string json = "{\"status\":\"ok\",\"written\":" + std::to_string(written);
    json += ",\"address\":" + hex32(address) + "}";
    return WebServer::sendJson(req, json);
}

/**
 * POST /api/reset — Ziel neu starten, Anwendung laeuft an.
 *
 * Geht nicht bei jedem Ziel. Ein RP2040 mit nativem USB hat keine
 * Reset-Leitung am Kabel; `/api/status` sagt unter `can_reset` vorher, ob der
 * Knopf ueberhaupt etwas bewirkt, und `reset_how` warum nicht.
 */
esp_err_t handleReset(httpd_req_t* req)
{
    return handleTargetReset(req, UsbTarget::ResetMode::Run);
}

/**
 * POST /api/bootmode — Ziel im Bootlader anhalten.
 *
 * Beim RP2040 ist das der 1200-Baud-Touch nach BOOTSEL, also dasselbe, was
 * `/api/flash` selbst tut, nur ohne zu schreiben — damit laesst sich der Touch
 * pruefen, ohne die Firmware des Ziels anzufassen.
 *
 * `/api/bootsel` bleibt als alter Name auf denselben Handler gelegt.
 */
esp_err_t handleBootmode(httpd_req_t* req)
{
    return handleTargetReset(req, UsbTarget::ResetMode::Bootloader);
}

/**
 * Leitet den LED-Modus aus dem Betriebszustand ab.
 *
 * Reihenfolge ist die Dringlichkeit: `Flashing` setzt handleFlash() selbst und
 * gewinnt gegen alles, weil dort ein Abziehen des Kabels Schaden anrichtet.
 *
 * Zu "Verbindung laeuft UND Funk wach": das ist bei dieser Firmware dieselbe
 * Bedingung. Der Funk bleibt genau dann wach, wenn ein TCP-Client auf 2323
 * oder 4000 haengt — siehe ClientActivity.hpp. Gelb heisst also "Client dran".
 */
void ledSupervisorTask(void* arg)
{
    auto* ctx = static_cast<AppContext*>(arg);

    while (true)
    {
        if (ctx->led->mode() != StatusLed::Mode::Flashing)
        {
            StatusLed::Mode next = StatusLed::Mode::Idle;

            // Reihenfolge = Dringlichkeit. BatteryLow fehlt hier bewusst:
            // das Board hat keine Akkumessung, der Modus existiert schon,
            // wird aber von nichts gesetzt (siehe CLAUDE.md, offener Punkt
            // zum Teiler auf dem PCB).
            if (ctx->wifi->state() != WiFiManager::State::Connected)
                next = StatusLed::Mode::NoWifi;
            else if (ctx->bridge->hasClient() || ctx->rfc2217->hasClient())
                next = StatusLed::Mode::Busy;
            else if (ctx->usb->state() == UsbTarget::State::Connected)
                next = StatusLed::Mode::TargetReady;

            ctx->led->setMode(next);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/**
 * Ein Client kommt oder geht — der Funk richtet sich danach.
 *
 * Bewusst hier verdrahtet und nicht in der Bruecke: die Server sollen nichts
 * vom WLAN wissen (siehe ClientActivity.hpp).
 */
void onClientActivity(bool busy, void* ctx)
{
    auto* wifi = static_cast<WiFiManager*>(ctx);
    if (wifi == nullptr) return;

    if (busy) wifi->acquireLowLatency();
    else      wifi->releaseLowLatency();
}

/**
 * POST /api/console — Kommando an das Zielgeraet schicken.
 *
 * Body ist der rohe Text, ohne Zeilenende. Welches Zeilenende angehaengt wird,
 * sagt `?eol=cr|lf|crlf|none`; Standard ist CR.
 *
 * Warum das konfigurierbar ist: die OpenKNX-Konsole reagiert auf **einzelne
 * Zeichen** (ein Tastendruck, eine Aktion), waehrend andere Firmware auf eine
 * abgeschlossene Zeile wartet. Ein festes Zeilenende wuerde den einen Fall
 * kaputtmachen, um den anderen zu bedienen. Leerer Body plus CR schickt nur
 * das Zeilenende — praktisch, um den Prompt neu zeichnen zu lassen.
 */
esp_err_t handleConsoleSend(httpd_req_t* req)
{
    auto* ctx = static_cast<AppContext*>(req->user_ctx);

    if (!ctx->usb->isSerialOpen())
    {
        return WebServer::sendStatus(
            req, "409 Conflict",
            R"({"error":"keine serielle Sitzung zum Ziel - haengt ein Geraet am USB-C?"})");
    }

    const char* eol = "\r";
    char        query[64] = {};
    char        value[8]  = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "eol", value, sizeof(value)) == ESP_OK)
    {
        if (strcmp(value, "none") == 0)      eol = "";
        else if (strcmp(value, "lf") == 0)   eol = "\n";
        else if (strcmp(value, "crlf") == 0) eol = "\r\n";
        else if (strcmp(value, "cr") == 0)   eol = "\r";
        else
        {
            return WebServer::sendStatus(
                req, "400 Bad Request",
                R"({"error":"eol muss cr, lf, crlf oder none sein"})");
        }
    }

    const size_t total = static_cast<size_t>(req->content_len);
    if (total > MAX_COMMAND_BYTES)
    {
        return WebServer::sendStatus(req, "413 Payload Too Large",
                                     R"({"error":"Kommando zu lang"})");
    }

    std::string text;
    text.resize(total);

    size_t received = 0;
    while (received < total)
    {
        const int got = httpd_req_recv(req, &text[received], total - received);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0)
        {
            return WebServer::sendStatus(req, "400 Bad Request",
                                         R"({"error":"Uebertragung abgebrochen"})");
        }
        received += static_cast<size_t>(got);
    }

    text += eol;
    if (text.empty())
    {
        return WebServer::sendStatus(req, "400 Bad Request",
                                     R"({"error":"nichts zu senden"})");
    }

    const esp_err_t err =
        ctx->bridge->sendToTarget(reinterpret_cast<const uint8_t*>(text.data()), text.size());

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Kommando konnte nicht gesendet werden: %s", esp_err_to_name(err));
        return WebServer::sendStatus(req, "502 Bad Gateway",
                                     R"({"error":"Schreiben aufs Ziel fehlgeschlagen: )" +
                                         std::string(esp_err_to_name(err)) + "\"}");
    }

    return WebServer::sendJson(req, "{\"status\":\"sent\",\"bytes\":" +
                                        std::to_string(text.size()) + "}");
}

/**
 * POST /api/led?gpio=N — Pin der RGB-LED setzen und in NVS merken.
 *
 * Welcher Pin die LED traegt, unterscheidet sich zwischen den
 * SuperMini-Varianten und steht in keinem Datenblatt, das vorliegt. Statt zu
 * raten und neu zu flashen, laesst sich der Pin hier durchprobieren: setzen,
 * hinschauen, und wenn es leuchtet, bleibt er gemerkt.
 */
esp_err_t handleLed(httpd_req_t* req)
{
    auto* ctx = static_cast<AppContext*>(req->user_ctx);

    char query[48] = {};
    char value[8]  = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "gpio", value, sizeof(value)) != ESP_OK)
    {
        return WebServer::sendStatus(req, "400 Bad Request",
                                     R"({"error":"Parameter gpio fehlt"})");
    }

    const int gpio = atoi(value);
    if (gpio < 0 || gpio > 48)
    {
        return WebServer::sendStatus(req, "400 Bad Request",
                                     R"({"error":"gpio muss zwischen 0 und 48 liegen"})");
    }

    const esp_err_t err = ctx->led->setPin(static_cast<gpio_num_t>(gpio));
    if (err != ESP_OK)
    {
        return WebServer::sendStatus(req, "500 Internal Server Error",
                                     R"({"error":"LED liess sich auf diesem Pin nicht aufbauen"})");
    }

    ctx->settings->setU32(KEY_LED_GPIO, static_cast<uint32_t>(gpio));

    return WebServer::sendJson(req, "{\"gpio\":" + std::to_string(gpio) +
                                        ",\"mode\":\"" +
                                        StatusLed::modeName(ctx->led->mode()) + "\"}");
}

/**
 * GET /api/snippet — die platformio.ini-Zeilen als reiner Text.
 *
 * Gedacht zum Abholen durch ein Werkzeug oder einen KI-Agenten: ein Aufruf, und
 * man hat die passenden Zeilen fuer das gerade angesteckte Ziel, ohne die
 * Unterscheidung UF2 gegen RFC2217 selbst treffen zu muessen.
 */
esp_err_t handleSnippet(httpd_req_t* req)
{
    auto* ctx = static_cast<AppContext*>(req->user_ctx);
    const Snippet s = buildSnippet(ctx->wifi->hostname(), *ctx->usb);

    std::string text = "; OpenKNX DebugProbe - " + ctx->wifi->hostname() + ".local\n";
    text += "; Ziel: " + s.comment + "\n";
    text += s.monitor + "\n";
    if (!s.upload.empty()) text += s.upload + "\n";

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, text.c_str(), text.size());
}

/**
 * GET /api/console[?cursor=N] — Mitschnitt fuer die Konsole im Browser.
 *
 * Ohne Cursor liefert sie den gesamten vorhandenen Puffer, mit Cursor nur das
 * seither Hinzugekommene. Der Browser merkt sich den zurueckgegebenen Wert.
 * Bewusst als Abfrage statt WebSocket: der Ringpuffer traegt die Zustandslosig-
 * keit ohnehin, und es kommt kein weiterer Serverzustand dazu.
 */
esp_err_t handleConsole(httpd_req_t* req)
{
    auto* ctx = static_cast<AppContext*>(req->user_ctx);

    uint64_t cursor = ctx->bridge->oldestByte();

    char query[64] = {};
    char value[24] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "cursor", value, sizeof(value)) == ESP_OK)
    {
        cursor = strtoull(value, nullptr, 10);
    }

    static constexpr size_t MAX_CHUNK = 4096;
    auto* buffer = static_cast<uint8_t*>(malloc(MAX_CHUNK));
    if (buffer == nullptr)
    {
        return WebServer::sendStatus(req, "500 Internal Server Error",
                                     R"({"error":"kein Speicher"})");
    }

    size_t       skipped = 0;
    const size_t count   = ctx->bridge->readFrom(cursor, buffer, MAX_CHUNK, skipped);

    std::string json = "{";
    json += "\"cursor\":" + std::to_string(cursor);
    json += ",\"skipped\":" + std::to_string(skipped);
    json += ",\"more\":" + std::string((cursor < ctx->bridge->newestByte()) ? "true" : "false");
    json += ",\"data\":\"" + jsonEscapeBytes(buffer, count) + "\"";
    json += "}";

    free(buffer);
    return WebServer::sendJson(req, json);
}

}  // namespace

extern "C" void app_main(void)
{
    static Settings    settings("probe");
    static WebServer   web;
    static AssetStore  assets;
    static OtaUpdater  ota;
    static WiFiManager  wifi(settings);
    static UsbTarget    usb;
    static SerialBridge  bridge;
    static Rfc2217Server rfc2217;
    static StatusLed     led;

    ESP_ERROR_CHECK(settings.begin());

    // Die LED so frueh wie moeglich: sie ist die einzige Rueckmeldung, solange
    // WLAN und Konsole noch nicht stehen.
    const auto ledGpio = static_cast<gpio_num_t>(
        settings.getU32(KEY_LED_GPIO, DEFAULT_LED_GPIO));
    const esp_err_t ledErr = led.begin(ledGpio);
    if (ledErr != ESP_OK)
    {
        ESP_LOGW(TAG, "status LED not available on GPIO%d: %s",
                 static_cast<int>(ledGpio), esp_err_to_name(ledErr));
    }

    ESP_LOGI(TAG, "OpenKNX DebugProbe %s (%s), running from '%s'",
             ota.version().c_str(), ota.buildTimestamp().c_str(), ota.runningPartition().c_str());
    if (ota.isPendingVerify())
    {
        ESP_LOGW(TAG, "image is on probation, will be confirmed in %u s", PROBATION_SECONDS);
    }

    WiFiManager::Config cfg;  // Defaults: 3 Versuche, sonst Notfall-Portal
    ESP_ERROR_CHECK(wifi.init(cfg));

    ESP_ERROR_CHECK(web.begin(80, 20));

    // Die Oberflaeche liegt im LittleFS, nicht mehr im Firmware-Image.
    // Ein Fehlschlag ist nicht fatal: AssetStore liefert dann die
    // eingebaute Notfallseite aus.
    const esp_err_t assetsErr = assets.mount();
    if (assetsErr != ESP_OK)
    {
        ESP_LOGW(TAG, "Web-Assets nicht verfuegbar: %s", esp_err_to_name(assetsErr));
    }

    g_ctx = AppContext{&settings, &wifi, &ota, &usb, &bridge, &rfc2217, &led};
    ESP_ERROR_CHECK(web.on("/api/status", HTTP_GET, &handleStatus, &g_ctx));
    ESP_ERROR_CHECK(web.on("/api/flash", HTTP_POST, &handleFlash, &g_ctx));
    ESP_ERROR_CHECK(web.on("/api/reset", HTTP_POST, &handleReset, &g_ctx));
    ESP_ERROR_CHECK(web.on("/api/bootmode", HTTP_POST, &handleBootmode, &g_ctx));
    // Alter Name aus der Zeit, als es nur den RP2040-Touch gab.
    ESP_ERROR_CHECK(web.on("/api/bootsel", HTTP_POST, &handleBootmode, &g_ctx));
    ESP_ERROR_CHECK(web.on("/api/led", HTTP_POST, &handleLed, &g_ctx));
    ESP_ERROR_CHECK(web.on("/api/console", HTTP_POST, &handleConsoleSend, &g_ctx));
    ESP_ERROR_CHECK(web.on("/api/console", HTTP_GET, &handleConsole, &g_ctx));
    ESP_ERROR_CHECK(web.on("/api/snippet", HTTP_GET, &handleSnippet, &g_ctx));
    ESP_ERROR_CHECK(web.on("/api/ch32", HTTP_GET, &handleCh32, &g_ctx));
    ESP_ERROR_CHECK(web.on("/api/ch32/flash", HTTP_POST, &handleCh32Flash, &g_ctx));
    ota.registerRoutes(web);

    // Decides between joining the stored network and opening the setup portal;
    // registerRoutes() needs to know the outcome.
    ESP_ERROR_CHECK(wifi.start());
    wifi.registerRoutes(web);

    if (wifi.state() != WiFiManager::State::Portal)
    {
        // ZULETZT: die Wildcard-Route "/*" wuerde sonst die /api-Routen
        // verschlucken — esp_http_server nimmt den ersten passenden
        // Handler in Registrierungsreihenfolge.
        ESP_ERROR_CHECK(assets.registerRoutes(web));
        ESP_LOGI(TAG, "ready: http://%s.local/", wifi.hostname().c_str());
    }

    // Erst jetzt den USB-Host starten: OTA ist ab hier erreichbar, und ein
    // Absturz hier führt zum Rollback statt zu einer toten Probe.
    const esp_err_t usbErr = usb.begin();
    if (usbErr != ESP_OK)
    {
        ESP_LOGE(TAG, "USB host not available: %s", esp_err_to_name(usbErr));
    }
    else
    {
        const esp_err_t bridgeErr = bridge.begin(usb, 2323, 16384);
        if (bridgeErr != ESP_OK)
        {
            ESP_LOGE(TAG, "serial bridge not available: %s", esp_err_to_name(bridgeErr));
        }
        else
        {
            ESP_LOGI(TAG, "console: socket://%s.local:2323", wifi.hostname().c_str());

            // Solange einer der beiden Server einen Client hat, bleibt der Funk
            // wach; danach dost er wieder. Das kostet im Leerlauf rund 100 mA
            // weniger und haelt den S3 merklich kuehler.
            bridge.setActivityHook(&onClientActivity, &wifi);
            rfc2217.setActivityHook(&onClientActivity, &wifi);

            bridge.setRfc2217(&rfc2217);
            const esp_err_t rfcErr = rfc2217.begin(usb, 4000);
            if (rfcErr != ESP_OK)
            {
                ESP_LOGE(TAG, "RFC2217 not available: %s", esp_err_to_name(rfcErr));
            }
            else
            {
                ESP_LOGI(TAG, "control: rfc2217://%s.local:4000", wifi.hostname().c_str());
            }
        }
    }

    // Erst jetzt starten: der Task liest bridge und rfc2217.
    if (led.isRunning())
    {
        xTaskCreatePinnedToCore(&ledSupervisorTask, "led_sup", 2560, &g_ctx, 2, nullptr, 0);
    }

    // Reaching a stable state — online or portal — counts as a healthy boot.
    // The rollback guards against crash loops, not against a missing access point.
    vTaskDelay(pdMS_TO_TICKS(PROBATION_SECONDS * 1000));
    ota.confirmRunningImage();
}
