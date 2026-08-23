#pragma once

#include <list>
#include <string>

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Thin RAII wrapper around esp_http_server.
 *
 * One instance owns the single HTTP server of the probe; every feature
 * (provisioning portal, OTA, later the USB/flash API) registers its routes here
 * instead of starting a server of its own.
 */
class WebServer
{
public:
    WebServer() = default;
    ~WebServer();

    WebServer(const WebServer&)            = delete;
    WebServer& operator=(const WebServer&) = delete;

    esp_err_t begin(uint16_t port = 80, uint16_t maxHandlers = 16);
    void      stop();

    bool           isRunning() const { return m_handle != nullptr; }
    httpd_handle_t handle() const { return m_handle; }

    /// Registers a route. @p ctx is passed through as httpd_req_t::user_ctx.
    esp_err_t on(const char*                    uri,
                 httpd_method_t                 method,
                 esp_err_t                      (*handler)(httpd_req_t*),
                 void*                          ctx = nullptr);

    /// Serves a blob that lives in flash (EMBED_TXTFILES / EMBED_FILES).
    esp_err_t serveStatic(const char* uri,
                          const char* body,
                          size_t      length,
                          const char* contentType = "text/html; charset=utf-8");

    /// Registers "/" with the built-in status/OTA page.
    esp_err_t serveDefaultIndex();

    esp_err_t setNotFoundHandler(httpd_err_handler_func_t fn);

    static esp_err_t sendJson(httpd_req_t* req, const std::string& json);
    static esp_err_t sendStatus(httpd_req_t* req, const char* status, const std::string& body);

    /// Reads the whole request body (bounded by @p maxLength) into @p out.
    static esp_err_t readBody(httpd_req_t* req, std::string& out, size_t maxLength = 4096);

    /// Extracts a field from an application/x-www-form-urlencoded body.
    static std::string formValue(const std::string& body, const char* key);

private:
    struct StaticBlob
    {
        const char* body;
        size_t      length;
        const char* contentType;
    };

    static esp_err_t staticHandler(httpd_req_t* req);

    httpd_handle_t        m_handle {nullptr};
    std::list<StaticBlob> m_blobs;  ///< stable addresses, used as user_ctx
};
