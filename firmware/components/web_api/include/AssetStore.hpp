#pragma once

#include <atomic>
#include <string>

#include "esp_err.h"
#include "esp_http_server.h"

class WebServer;

/**
 * Die Web-Oberflaeche, ausgelagert aus dem Firmware-Image in die
 * LittleFS-Partition `storage`.
 *
 * Vorher lag `index.html` per tools/embed_web.py als Raw-String im Binary und
 * belegte damit einen der beiden 1,875-MB-App-Slots — jede UI-Aenderung kostete
 * ein komplettes OTA. Jetzt liegen die Assets in einem eigenen Dateisystem und
 * lassen sich unabhaengig von der Firmware austauschen.
 *
 * LittleFS und nicht SPIFFS, weil die Probe ihre Versorgung aus dem Pruefaufbau
 * zieht: LittleFS ist copy-on-write und uebersteht einen Reset mitten im
 * Schreibvorgang, ohne das Dateisystem zu zerlegen.
 *
 * Die Klasse besitzt den Mount: der Destruktor haengt ihn wieder aus.
 */
class AssetStore
{
public:
    AssetStore() = default;
    ~AssetStore();

    AssetStore(const AssetStore&)            = delete;
    AssetStore& operator=(const AssetStore&) = delete;

    /**
     * Haengt die Partition ein. Schlaegt der Mount fehl (fabrikneue oder durch
     * einen Subtype-Wechsel ungueltig gewordene Partition), wird formatiert:
     * eine leere UI ist wiederherstellbar, eine nicht startende Probe nicht.
     */
    esp_err_t mount(const char* partitionLabel = "storage",
                    const char* basePath       = "/www");

    void unmount();

    bool isMounted() const { return m_mounted; }

    /// Belegung der Partition in Bytes. Nur gueltig, wenn isMounted().
    esp_err_t usage(size_t& totalBytes, size_t& usedBytes) const;

    /// true, wenn eine auslieferbare index.html im Dateisystem liegt.
    bool hasIndex() const;

    /**
     * Registriert `POST /api/fs`, "/" und die Wildcard-Route fuer alle
     * uebrigen Dateien.
     *
     * MUSS als LETZTES registriert werden: esp_http_server nimmt den ersten
     * passenden Handler in Registrierungsreihenfolge, und "/*" wuerde sonst die
     * /api-Routen verschlucken. (Die Wildcard steht auf GET, ein POST auf
     * /api/fs liefe ihr also ohnehin nicht ins Netz — die Reihenfolge hier ist
     * trotzdem richtig herum, damit das nicht von einem Detail abhaengt.)
     */
    esp_err_t registerRoutes(WebServer& web);

private:
    static esp_err_t fileHandler(httpd_req_t* req);

    /**
     * POST /api/fs — das LittleFS-Image in die Partition schreiben.
     *
     * Das Gegenstueck zu `/api/update`, nur fuer die Oberflaeche statt fuer die
     * Firmware. Ohne diesen Endpunkt braeuchte jede UI-Aenderung ein serielles
     * `uploadfs` — bei einer Probe, deren USB-C am Pruefling haengt, heisst das
     * Kabel umstecken. Genau das soll dieses Projekt abschaffen.
     *
     * Haengt das Dateisystem vorher aus und danach wieder ein. Bricht der
     * Upload ab, bleibt die Partition unvollstaendig — die Probe laeuft
     * weiter und liefert die Notfallseite aus, der Endpunkt bleibt erreichbar,
     * ein zweiter Versuch repariert es.
     */
    static esp_err_t uploadHandler(httpd_req_t* req);

    /// Uebersetzt eine URI in einen Pfad unterhalb des Mountpoints.
    /// Liefert false bei Directory-Traversal oder zu langen Pfaden.
    bool resolve(const char* uri, std::string& outPath) const;

    esp_err_t sendFile(httpd_req_t* req, const std::string& path, bool gzipped) const;

    static const char* contentTypeFor(const std::string& path);

    std::string       m_basePath;
    std::string       m_label;
    bool              m_mounted {false};
    std::atomic<bool> m_busy {false};  ///< ein Upload zur Zeit
};
