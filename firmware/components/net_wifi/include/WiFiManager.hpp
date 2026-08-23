#pragma once

#include <atomic>
#include <string>

#include "CaptiveDns.hpp"
#include "Settings.hpp"
#include "WebServer.hpp"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

/**
 * WiFi provisioning and connection management.
 *
 * Behaves like the well-known Arduino WiFiManager: if no credentials are stored
 * or the stored ones do not work, the probe opens its own access point with a
 * captive portal. Entering credentials there stores them and reboots.
 */
class WiFiManager
{
public:
    enum class State
    {
        Idle,
        Connecting,
        Connected,
        Portal
    };

    struct Config
    {
        std::string hostname;               ///< empty -> "openknx-probe-<mac>"
        std::string portalPassword;         ///< empty -> open access point
        /// Obergrenze fuer den gesamten Verbindungsaufbau. Normalerweise entscheidet
        /// vorher die Zahl der Versuche; das hier ist nur der Notnagel.
        uint32_t    connectTimeoutMs {30000};
    };

    explicit WiFiManager(Settings& settings);
    ~WiFiManager();

    WiFiManager(const WiFiManager&)            = delete;
    WiFiManager& operator=(const WiFiManager&) = delete;

    /// Brings up netif, the default event loop and the WiFi driver.
    esp_err_t init(const Config& cfg);

    /// Connects with stored credentials, or opens the provisioning portal.
    esp_err_t start();

    /// Adds the provisioning routes to the shared HTTP server.
    void registerRoutes(WebServer& server);

    State       state() const { return m_state; }
    const char* stateName() const;
    std::string hostname() const { return m_hostname; }
    std::string ssid() const;
    std::string ip() const;
    int8_t      rssi() const;
    bool        hasCredentials() const;
    void        forgetCredentials();

    // -----------------------------------------------------------------------
    // Dynamisches Stromsparen
    // -----------------------------------------------------------------------

    /**
     * Haelt den Funk wach, solange mindestens ein Halter angemeldet ist.
     *
     * Hintergrund: mit `WIFI_PS_NONE` laeuft der Empfaenger dauerhaft, was rund
     * 100 mA kostet und den S3 merklich waermer werden laesst. Gebraucht wird
     * das aber nur, solange wirklich jemand an der Leitung haengt — die Konsole
     * auf 2323 oder ein Flash-Werkzeug auf 4000. Sonst genuegt
     * `WIFI_PS_MIN_MODEM`, wo der Funk zwischen den Beacons dost.
     *
     * Gezaehlt statt geschaltet, weil beide Server unabhaengig voneinander
     * kommen und gehen: erst wenn der letzte weg ist, darf gedost werden.
     *
     * Beide Aufrufe sind aus jedem Task erlaubt.
     */
    void acquireLowLatency();
    void releaseLowLatency();

    /// true, wenn der Funk gerade dauerhaft wach ist (kein Stromsparen).
    bool isRadioAwake() const { return m_psNoneActive.load(); }

    static constexpr const char* KEY_SSID     = "wifi_ssid";
    static constexpr const char* KEY_PASS     = "wifi_pass";
    static constexpr const char* KEY_HOSTNAME = "hostname";

private:
    static void eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);

    /// Zieht den Sparmodus an den Zaehlerstand nach. Idempotent.
    void applyPowerSave();

    esp_err_t   connectSta();
    esp_err_t   startPortal();
    esp_err_t   startMdns();
    std::string defaultHostname() const;

    static esp_err_t handlePortalRoot(httpd_req_t* req);
    static esp_err_t handleScan(httpd_req_t* req);
    static esp_err_t handleConnect(httpd_req_t* req);
    static esp_err_t handleForget(httpd_req_t* req);
    static esp_err_t handleCaptiveRedirect(httpd_req_t* req, httpd_err_code_t error);

    Settings&        m_settings;
    Config           m_cfg {};
    std::string      m_hostname;
    State            m_state {State::Idle};
    esp_netif_t*     m_staNetif {nullptr};
    esp_netif_t*     m_apNetif {nullptr};
    EventGroupHandle_t m_events {nullptr};

    /// Serialisiert applyPowerSave(); der Zaehler wird INNERHALB des Locks
    /// gelesen, damit ein Acquire waehrend eines wartenden Release nicht
    /// verlorengeht.
    SemaphoreHandle_t  m_psMutex {nullptr};
    std::atomic<int>   m_awakeHolders {0};
    std::atomic<bool>  m_psNoneActive {false};
    CaptiveDns       m_dns;
    esp_timer_handle_t m_portalRetry {nullptr};
    uint8_t          m_attempts {0};
    bool             m_mdnsUp {false};
};
