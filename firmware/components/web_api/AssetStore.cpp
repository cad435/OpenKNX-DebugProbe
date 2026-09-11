#include "AssetStore.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "generated/fallback_html.hpp"
#include "WebServer.hpp"

namespace {
constexpr const char* TAG = "assets";

/// Haeppchengroesse beim Ausliefern. Liegt bewusst auf dem Heap und nicht auf
/// dem Stack: der httpd-Task hat nur 8 KB.
constexpr size_t CHUNK = 4096;

/// Laengster Pfad, den wir aus einer URI bilden. Deckt "/www/" + Dateiname ab.
constexpr size_t MAX_PATH = 128;

/// LittleFS traegt den Namen seines Dateisystems im Superblock, bei Offset 8
/// des ersten Blocks. Genau wie `/api/update` das ESP32-Image-Magic prueft,
/// haelt das hier die falsche Datei ab — eine firmware.bin in der
/// storage-Partition waere sonst eine stumme, leere Oberflaeche.
constexpr size_t      LFS_MAGIC_OFFSET = 8;
constexpr const char* LFS_MAGIC        = "littlefs";
constexpr size_t      LFS_MAGIC_LEN    = 8;
constexpr size_t      LFS_HEADER_MIN   = LFS_MAGIC_OFFSET + LFS_MAGIC_LEN;

/// Schreibhaeppchen. Ein Vielfaches der Flash-Sektorgroesse, damit am Ende nur
/// ein einziger unvollstaendiger Block uebrig bleiben kann.
constexpr size_t WRITE_CHUNK = 4096;

std::string jsonError(const std::string& text)
{
    std::string out = R"({"error":")";
    for (const char c : text)
    {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"}";
}

bool acceptsGzip(httpd_req_t* req)
{
    char header[64];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", header, sizeof(header)) != ESP_OK)
    {
        return false;
    }
    return strstr(header, "gzip") != nullptr;
}

bool fileExists(const std::string& path)
{
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
}  // namespace

AssetStore::~AssetStore()
{
    unmount();
}

esp_err_t AssetStore::mount(const char* partitionLabel, const char* basePath)
{
    if (m_mounted) return ESP_OK;

    m_label    = partitionLabel;
    m_basePath = basePath;

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path               = m_basePath.c_str();
    conf.partition_label         = m_label.c_str();
    // Eine leere UI ist per Upload reparierbar, eine nicht startende Probe
    // nicht. Nach dem Wechsel SPIFFS -> LittleFS greift genau dieser Zweig.
    conf.format_if_mount_failed = 1;
    conf.dont_mount             = 0;

    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "LittleFS '%s' nicht eingehaengt: %s", m_label.c_str(),
                 esp_err_to_name(err));
        return err;
    }

    m_mounted = true;

    size_t total = 0;
    size_t used  = 0;
    if (usage(total, used) == ESP_OK)
    {
        ESP_LOGI(TAG, "LittleFS '%s' auf %s: %u KB belegt von %u KB",
                 m_label.c_str(), m_basePath.c_str(),
                 static_cast<unsigned>(used / 1024), static_cast<unsigned>(total / 1024));
    }

    if (!hasIndex())
    {
        ESP_LOGW(TAG, "keine index.html im Dateisystem — es wird die "
                      "eingebaute Notfallseite ausgeliefert");
    }

    return ESP_OK;
}

void AssetStore::unmount()
{
    if (!m_mounted) return;
    esp_vfs_littlefs_unregister(m_label.c_str());
    m_mounted = false;
}

esp_err_t AssetStore::usage(size_t& totalBytes, size_t& usedBytes) const
{
    if (!m_mounted) return ESP_ERR_INVALID_STATE;
    return esp_littlefs_info(m_label.c_str(), &totalBytes, &usedBytes);
}

bool AssetStore::hasIndex() const
{
    if (!m_mounted) return false;
    return fileExists(m_basePath + "/index.html");
}

bool AssetStore::resolve(const char* uri, std::string& outPath) const
{
    std::string path(uri);

    // Query-String und Fragment gehoeren nicht zum Dateinamen.
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) path.erase(cut);

    if (path.empty() || path == "/") path = "/index.html";

    // Directory-Traversal: ".." in jeder Form abweisen, statt zu versuchen, den
    // Pfad zu normalisieren. Die Probe liefert ein flaches Asset-Verzeichnis
    // aus, hier gibt es nichts zu retten.
    if (path.find("..") != std::string::npos) return false;
    if (path.front() != '/') return false;

    outPath = m_basePath + path;
    return outPath.size() < MAX_PATH;
}

const char* AssetStore::contentTypeFor(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";

    const std::string ext = path.substr(dot);

    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".css")  return "text/css; charset=utf-8";
    if (ext == ".js")   return "text/javascript; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".svg")  return "image/svg+xml";
    if (ext == ".png")  return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".ico")  return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".txt")  return "text/plain; charset=utf-8";

    return "application/octet-stream";
}

esp_err_t AssetStore::sendFile(httpd_req_t* req, const std::string& path, bool gzipped) const
{
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) return ESP_ERR_NOT_FOUND;

    // Der Content-Type richtet sich nach dem Namen OHNE ".gz" — sonst laedt der
    // Browser die Seite als Archiv herunter, statt sie darzustellen.
    const std::string logical = gzipped ? path.substr(0, path.size() - 3) : path;

    httpd_resp_set_type(req, contentTypeFor(logical));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (gzipped) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    std::vector<char> buffer(CHUNK);
    esp_err_t         result = ESP_OK;

    while (true)
    {
        const size_t read = fread(buffer.data(), 1, buffer.size(), file);
        if (read == 0) break;

        if (httpd_resp_send_chunk(req, buffer.data(), read) != ESP_OK)
        {
            // Abbruch durch den Client: die Antwort ist verloren, aber der
            // Server bleibt gesund. Kein send_chunk(nullptr) mehr hinterher.
            ESP_LOGW(TAG, "Uebertragung von %s abgebrochen", path.c_str());
            result = ESP_FAIL;
            break;
        }
    }

    fclose(file);

    if (result == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);
    return result;
}

esp_err_t AssetStore::fileHandler(httpd_req_t* req)
{
    auto* self = static_cast<AssetStore*>(req->user_ctx);
    if (self == nullptr) return httpd_resp_send_500(req);

    std::string path;
    if (self->m_mounted && self->resolve(req->uri, path))
    {
        // Vorkomprimierte Variante bevorzugen, wenn der Browser sie annimmt.
        if (acceptsGzip(req))
        {
            const std::string packed = path + ".gz";
            if (fileExists(packed) && self->sendFile(req, packed, true) == ESP_OK)
            {
                return ESP_OK;
            }
        }

        if (fileExists(path) && self->sendFile(req, path, false) == ESP_OK)
        {
            return ESP_OK;
        }
    }

    // Kein Dateisystem oder keine index.html: die eingebaute Notfallseite. Sie
    // ist der Grund, warum eine leere storage-Partition die Probe nicht
    // unbedienbar macht.
    if (strcmp(req->uri, "/") == 0)
    {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, FALLBACK_HTML, FALLBACK_HTML_LEN);
    }

    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "not found", HTTPD_RESP_USE_STRLEN);
}

esp_err_t AssetStore::uploadHandler(httpd_req_t* req)
{
    auto* self = static_cast<AssetStore*>(req->user_ctx);
    if (self == nullptr) return httpd_resp_send_500(req);

    bool expected = false;
    if (!self->m_busy.compare_exchange_strong(expected, true))
    {
        return WebServer::sendStatus(req, "409 Conflict",
                                     jsonError("es laeuft bereits ein Upload"));
    }

    struct BusyGuard
    {
        std::atomic<bool>& flag;
        ~BusyGuard() { flag.store(false); }
    } guard{self->m_busy};

    const size_t total = static_cast<size_t>(req->content_len);
    if (total == 0)
    {
        return WebServer::sendStatus(req, "400 Bad Request", jsonError("leerer Body"));
    }
    if (total < LFS_HEADER_MIN)
    {
        return WebServer::sendStatus(req, "400 Bad Request",
                                     jsonError("zu kurz fuer ein LittleFS-Image"));
    }

    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, self->m_label.c_str());
    if (part == nullptr)
    {
        return WebServer::sendStatus(req, "500 Internal Server Error",
                                     jsonError("Partition '" + self->m_label + "' nicht gefunden"));
    }
    if (total > part->size)
    {
        return WebServer::sendStatus(req, "413 Payload Too Large",
                                     jsonError("Image groesser als die Partition"));
    }

    ESP_LOGI(TAG, "empfange %u Bytes fuer '%s' (%u Bytes gross)",
             static_cast<unsigned>(total), part->label,
             static_cast<unsigned>(part->size));

    std::vector<uint8_t> buffer(WRITE_CHUNK);
    size_t               received = 0;   ///< vom Client geholt
    size_t               pending  = 0;   ///< im Puffer, noch nicht geschrieben
    size_t               written  = 0;   ///< in den Flash geschrieben

    /*
     * Zuerst den Anfang holen und pruefen, ERST DANN loeschen.
     *
     * Die Reihenfolge ist der ganze Punkt: wuerde erst geloescht und dann
     * geprueft, haette eine versehentlich hierher geschickte firmware.bin die
     * vorhandene Oberflaeche schon mitgenommen, obwohl der Endpunkt sie
     * ablehnt. So bleibt das Dateisystem bei einer falschen Datei unberuehrt.
     */
    while (pending < LFS_HEADER_MIN)
    {
        const size_t want = std::min(WRITE_CHUNK - pending, total - received);
        const int    got  = httpd_req_recv(
            req, reinterpret_cast<char*>(buffer.data() + pending), want);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0)
        {
            ESP_LOGE(TAG, "Abbruch nach %u Bytes", static_cast<unsigned>(received));
            return WebServer::sendStatus(req, "400 Bad Request",
                                         jsonError("Uebertragung abgebrochen"));
        }
        received += static_cast<size_t>(got);
        pending  += static_cast<size_t>(got);
    }

    if (std::memcmp(buffer.data() + LFS_MAGIC_OFFSET, LFS_MAGIC, LFS_MAGIC_LEN) != 0)
    {
        ESP_LOGE(TAG, "kein LittleFS-Image — Dateisystem bleibt unberuehrt");
        return WebServer::sendStatus(
            req, "400 Bad Request",
            jsonError("kein LittleFS-Image - die firmware.bin gehoert nach /api/update"));
    }

    /*
     * Kopien, bevor ausgehaengt wird: mount() weist m_label und m_basePath neu
     * zu. Uebergibt man ihm m_label.c_str(), zeigt das Argument in genau den
     * Puffer, der dabei ueberschrieben wird.
     */
    const std::string label     = self->m_label;
    const std::string basePath  = self->m_basePath;
    const bool        wasMounted = self->m_mounted;

    auto restore = [&]() {
        if (wasMounted) self->mount(label.c_str(), basePath.c_str());
    };

    auto fail = [&](const char* status, const std::string& text) {
        restore();
        return WebServer::sendStatus(req, status, jsonError(text));
    };

    /*
     * Aushaengen, bevor unter dem laufenden Dateisystem geschrieben wird. Der
     * fileHandler faellt dadurch auf die Notfallseite zurueck — das ist der
     * gewollte Zustand fuer die Dauer des Uploads.
     */
    self->unmount();

    /*
     * Die ganze Partition loeschen, nicht nur den beschriebenen Teil: ein
     * kleineres Image liesse sonst alte Bloecke stehen, und LittleFS findet
     * beim Mounten unter Umstaenden den aelteren der beiden Superbloecke.
     */
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Loeschen fehlgeschlagen: %s", esp_err_to_name(err));
        return fail("500 Internal Server Error", esp_err_to_name(err));
    }

    while (true)
    {
        if (pending == WRITE_CHUNK || received == total)
        {
            if (pending == 0) break;

            // Flash will Vielfache von 4 Byte. Der Rest bleibt geloescht (0xFF).
            const size_t chunk = (pending + 3) & ~static_cast<size_t>(3);
            std::memset(buffer.data() + pending, 0xFF, chunk - pending);

            err = esp_partition_write(part, written, buffer.data(), chunk);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Schreiben fehlgeschlagen: %s", esp_err_to_name(err));
                return fail("500 Internal Server Error", esp_err_to_name(err));
            }
            written += pending;
            pending = 0;

            if (received == total) break;
        }

        const size_t want = std::min(WRITE_CHUNK - pending, total - received);
        const int    got  = httpd_req_recv(
            req, reinterpret_cast<char*>(buffer.data() + pending), want);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0)
        {
            ESP_LOGE(TAG, "Abbruch nach %u Bytes", static_cast<unsigned>(received));
            return fail("400 Bad Request", "Uebertragung abgebrochen");
        }

        received += static_cast<size_t>(got);
        pending  += static_cast<size_t>(got);
    }

    ESP_LOGI(TAG, "%u Bytes geschrieben, haenge wieder ein",
             static_cast<unsigned>(written));

    const esp_err_t mountErr = self->mount(label.c_str(), basePath.c_str());
    if (mountErr != ESP_OK)
    {
        return WebServer::sendStatus(
            req, "500 Internal Server Error",
            jsonError(std::string("geschrieben, aber nicht einhaengbar: ") +
                      esp_err_to_name(mountErr)));
    }

    size_t fsTotal = 0;
    size_t fsUsed  = 0;
    self->usage(fsTotal, fsUsed);

    std::string json = "{\"status\":\"ok\",\"written\":" + std::to_string(written);
    json += ",\"total_kb\":" + std::to_string(fsTotal / 1024);
    json += ",\"used_kb\":" + std::to_string(fsUsed / 1024);
    json += ",\"index\":" + std::string(self->hasIndex() ? "true" : "false");
    json += "}";
    return WebServer::sendJson(req, json);
}

esp_err_t AssetStore::registerRoutes(WebServer& web)
{
    // Vor der Wildcard: siehe Kommentar an registerRoutes() im Header.
    const esp_err_t fsErr = web.on("/api/fs", HTTP_POST, &AssetStore::uploadHandler, this);
    if (fsErr != ESP_OK) return fsErr;

    const esp_err_t rootErr = web.on("/", HTTP_GET, &AssetStore::fileHandler, this);
    if (rootErr != ESP_OK) return rootErr;

    return web.on("/*", HTTP_GET, &AssetStore::fileHandler, this);
}
