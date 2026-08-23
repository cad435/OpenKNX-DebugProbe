#include "WiFiManager.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "generated/portal_html.hpp"
#include "mdns.h"

namespace {
constexpr const char* TAG = "wifi";

constexpr EventBits_t BIT_CONNECTED = BIT0;
constexpr EventBits_t BIT_FAILED    = BIT1;

/// Nach so vielen erfolglosen Verbindungsversuchen geht die Probe ins Notfall-Portal.
constexpr uint8_t MAX_CONNECT_ATTEMPTS = 3;

/// Steht das Portal nur als Notfall (Zugangsdaten sind vorhanden), wird nach dieser
/// Zeit ohne Neukonfiguration neu gestartet und der Verbindungsaufbau erneut versucht.
/// Damit ist der Notfall-AP nicht persistent: ein kurzzeitig weggewesenes WLAN holt die
/// Probe von selbst wieder ein, ohne dass jemand hinfahren muss.
constexpr uint64_t PORTAL_RETRY_AFTER_US = 300ULL * 1000 * 1000;  // 5 min

WiFiManager* s_instance = nullptr;

std::string jsonEscape(const char* in, size_t maxLen)
{
    std::string out;
    for (size_t i = 0; i < maxLen && in[i] != '\0'; ++i)
    {
        const char c = in[i];
        if (c == '"' || c == '\\')
            out += {'\\', c};
        else if (static_cast<unsigned char>(c) < 0x20)
            continue;
        else
            out += c;
    }
    return out;
}
}  // namespace

WiFiManager::WiFiManager(Settings& settings) : m_settings(settings)
{
    s_instance = this;
}

WiFiManager::~WiFiManager()
{
    m_dns.stop();
    if (m_events != nullptr) vEventGroupDelete(m_events);
    if (m_psMutex != nullptr) vSemaphoreDelete(m_psMutex);
    if (s_instance == this) s_instance = nullptr;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

esp_err_t WiFiManager::init(const Config& cfg)
{
    m_cfg    = cfg;
    m_events = xEventGroupCreate();
    if (m_events == nullptr) return ESP_ERR_NO_MEM;

    m_psMutex = xSemaphoreCreateMutex();
    if (m_psMutex == nullptr) return ESP_ERR_NO_MEM;

    m_hostname = m_settings.getString(KEY_HOSTNAME, "");
    if (m_hostname.empty()) m_hostname = cfg.hostname;
    if (m_hostname.empty()) m_hostname = defaultHostname();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    m_staNetif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&initCfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiManager::eventHandler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WiFiManager::eventHandler, this, nullptr));

    // Credentials live in our own NVS namespace, not in the driver's.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_LOGI(TAG, "hostname '%s'", m_hostname.c_str());
    return ESP_OK;
}

esp_err_t WiFiManager::start()
{
    if (!hasCredentials())
    {
        ESP_LOGW(TAG, "no stored credentials, opening portal");
        return startPortal();
    }

    if (connectSta() == ESP_OK) return ESP_OK;

    ESP_LOGW(TAG, "could not join stored network, opening portal");
    return startPortal();
}

esp_err_t WiFiManager::connectSta()
{
    const std::string ssid = m_settings.getString(KEY_SSID);
    const std::string pass = m_settings.getString(KEY_PASS);

    wifi_config_t wc = {};
    std::strncpy(reinterpret_cast<char*>(wc.sta.ssid), ssid.c_str(), sizeof(wc.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wc.sta.password), pass.c_str(), sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Startzustand: sparen. Sobald sich ein Client auf 2323 oder 4000 meldet,
    // hebt acquireLowLatency() den Funk wieder auf Dauerbetrieb. Fuer HTTP und
    // mDNS reicht MIN_MODEM, dort kostet die Beacon-Latenz nichts Fuehlbares.
    m_awakeHolders.store(0);
    m_psNoneActive.store(true);   // erzwingt beim naechsten Aufruf das Setzen
    applyPowerSave();

    m_state    = State::Connecting;
    m_attempts = 0;
    xEventGroupClearBits(m_events, BIT_CONNECTED | BIT_FAILED);

    ESP_LOGI(TAG, "joining '%s' (max %u Versuche)", ssid.c_str(), MAX_CONNECT_ATTEMPTS);

    const EventBits_t bits = xEventGroupWaitBits(m_events,
                                                 BIT_CONNECTED | BIT_FAILED,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(m_cfg.connectTimeoutMs));

    if ((bits & BIT_CONNECTED) != 0)
    {
        startMdns();
        return ESP_OK;
    }

    esp_wifi_stop();
    m_state = State::Idle;
    return ESP_FAIL;
}

// ---------------------------------------------------------------------------
// Dynamisches Stromsparen
// ---------------------------------------------------------------------------

void WiFiManager::acquireLowLatency()
{
    m_awakeHolders.fetch_add(1);
    applyPowerSave();
}

void WiFiManager::releaseLowLatency()
{
    if (m_awakeHolders.fetch_sub(1) <= 0)
    {
        // Sollte nie passieren; lieber zurueckdrehen als in den Minusbereich
        // laufen, sonst bleibt der Funk fuer immer im Sparmodus haengen.
        m_awakeHolders.store(0);
        ESP_LOGW(TAG, "releaseLowLatency ohne passendes acquire");
    }
    applyPowerSave();
}

void WiFiManager::applyPowerSave()
{
    // Im Portal laeuft ein AP; dort waere Stromsparen sinnlos bis schaedlich,
    // weil die Gegenstelle auf unsere Beacons wartet.
    if (m_psMutex == nullptr || m_state == State::Portal) return;

    xSemaphoreTake(m_psMutex, portMAX_DELAY);

    const bool wantAwake = (m_awakeHolders.load() > 0);
    if (wantAwake != m_psNoneActive.load())
    {
        const wifi_ps_type_t mode = wantAwake ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM;
        const esp_err_t      err  = esp_wifi_set_ps(mode);

        if (err == ESP_OK)
        {
            m_psNoneActive.store(wantAwake);
            ESP_LOGI(TAG, "Funk %s (%d Halter)",
                     wantAwake ? "dauerhaft wach" : "im Sparmodus",
                     m_awakeHolders.load());
        }
        else
        {
            ESP_LOGW(TAG, "esp_wifi_set_ps failed: %s", esp_err_to_name(err));
        }
    }

    xSemaphoreGive(m_psMutex);
}

esp_err_t WiFiManager::startPortal()
{
    esp_wifi_stop();

    if (m_apNetif == nullptr) m_apNetif = esp_netif_create_default_wifi_ap();

    wifi_config_t wc = {};
    const std::string apSsid = m_hostname;
    std::strncpy(reinterpret_cast<char*>(wc.ap.ssid), apSsid.c_str(), sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len       = static_cast<uint8_t>(std::strlen(reinterpret_cast<char*>(wc.ap.ssid)));
    wc.ap.channel        = 1;
    wc.ap.max_connection = 2;

    if (m_cfg.portalPassword.empty())
    {
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }
    else
    {
        std::strncpy(reinterpret_cast<char*>(wc.ap.password),
                     m_cfg.portalPassword.c_str(),
                     sizeof(wc.ap.password) - 1);
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    // APSTA so the portal can scan for networks while the AP stays up.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip = {};
    esp_netif_get_ip_info(m_apNetif, &ip);
    m_dns.start(ip.ip.addr);

    m_state = State::Portal;
    ESP_LOGW(TAG, "portal up: SSID '%s', http://" IPSTR "/", apSsid.c_str(), IP2STR(&ip.ip));

    // Notfall-Portal (Zugangsdaten sind vorhanden, das WLAN war nur nicht da):
    // nach einer Weile ohne Neukonfiguration neu starten und wieder verbinden.
    // Beim Erstsetup (keine Zugangsdaten) bleibt das Portal dagegen offen.
    if (hasCredentials())
    {
        const esp_timer_create_args_t timerArgs = {
            .callback = [](void*) {
                ESP_LOGW(TAG, "Notfall-Portal ohne Neukonfiguration, erneuter Verbindungsversuch");
                esp_restart();
            },
            .arg                   = this,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "portal_retry",
            .skip_unhandled_events = false,
        };
        if (esp_timer_create(&timerArgs, &m_portalRetry) == ESP_OK)
        {
            esp_timer_start_once(m_portalRetry, PORTAL_RETRY_AFTER_US);
            ESP_LOGW(TAG, "Notfall-Portal: Neuversuch in %llu s", PORTAL_RETRY_AFTER_US / 1000000);
        }
    }
    return ESP_OK;
}

esp_err_t WiFiManager::startMdns()
{
    if (m_mdnsUp) return ESP_OK;

    esp_err_t err = mdns_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }
    mdns_hostname_set(m_hostname.c_str());
    mdns_instance_name_set("OpenKNX DebugProbe");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);

    m_mdnsUp = true;
    ESP_LOGI(TAG, "mDNS: http://%s.local/", m_hostname.c_str());
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void WiFiManager::eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    auto* self = static_cast<WiFiManager*>(arg);

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_netif_set_hostname(self->m_staNetif, self->m_hostname.c_str());
        self->m_attempts = 1;
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (self->m_state == State::Connected)
        {
            // Already been online once: keep trying forever, do not fall back
            // to the portal just because an access point rebooted.
            ESP_LOGW(TAG, "link lost, reconnecting");
            self->m_state = State::Connecting;
            esp_wifi_connect();
        }
        else if (self->m_state == State::Connecting)
        {
            if (self->m_attempts < MAX_CONNECT_ATTEMPTS)
            {
                ++self->m_attempts;
                ESP_LOGW(TAG, "Verbindung fehlgeschlagen, Versuch %u/%u",
                         self->m_attempts, MAX_CONNECT_ATTEMPTS);
                esp_wifi_connect();
            }
            else
            {
                ESP_LOGE(TAG, "%u Versuche erfolglos", MAX_CONNECT_ATTEMPTS);
                xEventGroupSetBits(self->m_events, BIT_FAILED);
            }
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        const auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
        self->m_attempts = 0;
        self->m_state    = State::Connected;
        self->startMdns();
        xEventGroupSetBits(self->m_events, BIT_CONNECTED);
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const char* WiFiManager::stateName() const
{
    switch (m_state)
    {
        case State::Idle:       return "idle";
        case State::Connecting: return "connecting";
        case State::Connected:  return "connected";
        case State::Portal:     return "portal";
    }
    return "unknown";
}

std::string WiFiManager::ssid() const
{
    if (m_state == State::Portal) return {};
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return {};
    return std::string(reinterpret_cast<const char*>(ap.ssid));
}

std::string WiFiManager::ip() const
{
    esp_netif_t* netif = (m_state == State::Portal) ? m_apNetif : m_staNetif;
    if (netif == nullptr) return {};

    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return {};

    char buf[16];
    std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    return std::string(buf);
}

int8_t WiFiManager::rssi() const
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

bool WiFiManager::hasCredentials() const
{
    return !m_settings.getString(KEY_SSID).empty();
}

void WiFiManager::forgetCredentials()
{
    m_settings.erase(KEY_SSID);
    m_settings.erase(KEY_PASS);
}

std::string WiFiManager::defaultHostname() const
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "openknx-probe-%02x%02x", mac[4], mac[5]);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

void WiFiManager::registerRoutes(WebServer& server)
{
    server.on("/api/scan", HTTP_GET, &WiFiManager::handleScan, this);
    server.on("/api/wifi/connect", HTTP_POST, &WiFiManager::handleConnect, this);
    server.on("/api/wifi/forget", HTTP_POST, &WiFiManager::handleForget, this);

    if (m_state == State::Portal)
    {
        server.on("/", HTTP_GET, &WiFiManager::handlePortalRoot, this);
        // Anything else in portal mode redirects to the form, which is what
        // triggers the captive-portal popup on phones.
        server.setNotFoundHandler(&WiFiManager::handleCaptiveRedirect);
    }
}

esp_err_t WiFiManager::handlePortalRoot(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORTAL_HTML, PORTAL_HTML_LEN);
}

esp_err_t WiFiManager::handleCaptiveRedirect(httpd_req_t* req, httpd_err_code_t)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t WiFiManager::handleScan(httpd_req_t* req)
{
    wifi_scan_config_t scanCfg = {};
    scanCfg.show_hidden        = false;

    const esp_err_t err = esp_wifi_scan_start(&scanCfg, true);
    if (err != ESP_OK)
    {
        return WebServer::sendStatus(req, "503 Service Unavailable",
                                     R"({"error":"scan failed"})");
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 20) count = 20;

    auto* records = static_cast<wifi_ap_record_t*>(calloc(count, sizeof(wifi_ap_record_t)));
    if (records == nullptr)
    {
        esp_wifi_clear_ap_list();
        return WebServer::sendStatus(req, "500 Internal Server Error",
                                     R"({"error":"out of memory"})");
    }
    esp_wifi_scan_get_ap_records(&count, records);

    std::string json = "[";
    for (uint16_t i = 0; i < count; ++i)
    {
        if (i != 0) json += ',';
        json += R"({"ssid":")";
        json += jsonEscape(reinterpret_cast<const char*>(records[i].ssid), 32);
        json += R"(","rssi":)";
        json += std::to_string(records[i].rssi);
        json += R"(,"open":)";
        json += (records[i].authmode == WIFI_AUTH_OPEN) ? "true" : "false";
        json += '}';
    }
    json += ']';

    free(records);
    return WebServer::sendJson(req, json);
}

esp_err_t WiFiManager::handleConnect(httpd_req_t* req)
{
    auto* self = static_cast<WiFiManager*>(req->user_ctx);

    std::string body;
    if (WebServer::readBody(req, body, 1024) != ESP_OK)
    {
        return WebServer::sendStatus(req, "400 Bad Request", R"({"error":"body too large"})");
    }

    const std::string ssid = WebServer::formValue(body, "ssid");
    const std::string pass = WebServer::formValue(body, "pass");
    const std::string name = WebServer::formValue(body, "name");

    if (ssid.empty())
    {
        return WebServer::sendStatus(req, "400 Bad Request", R"({"error":"ssid missing"})");
    }

    self->m_settings.setString(KEY_SSID, ssid);
    self->m_settings.setString(KEY_PASS, pass);
    if (!name.empty()) self->m_settings.setString(KEY_HOSTNAME, name);

    ESP_LOGI(TAG, "credentials for '%s' stored, restarting", ssid.c_str());

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
                       "<!doctype html><meta charset=utf-8>"
                       "<body style='font:16px system-ui;padding:2rem'>"
                       "<h2>Gespeichert</h2>"
                       "<p>Die Probe startet neu und verbindet sich. "
                       "Danach ist sie unter ihrem Namen im WLAN erreichbar.</p>");

    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

esp_err_t WiFiManager::handleForget(httpd_req_t* req)
{
    auto* self = static_cast<WiFiManager*>(req->user_ctx);
    self->forgetCredentials();

    ESP_LOGW(TAG, "credentials cleared, restarting into portal");
    WebServer::sendJson(req, R"({"status":"cleared","restarting":true})");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
