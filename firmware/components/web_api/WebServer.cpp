#include "WebServer.hpp"

#include <algorithm>
#include <cstring>

#include "esp_log.h"

namespace {
constexpr const char* TAG = "web";

int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '+')
        {
            out.push_back(' ');
        }
        else if (in[i] == '%' && i + 2 < in.size())
        {
            const int hi = hexValue(in[i + 1]);
            const int lo = hexValue(in[i + 2]);
            if (hi >= 0 && lo >= 0)
            {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            }
            else
            {
                out.push_back(in[i]);
            }
        }
        else
        {
            out.push_back(in[i]);
        }
    }
    return out;
}
}  // namespace

WebServer::~WebServer()
{
    stop();
}

esp_err_t WebServer::begin(uint16_t port, uint16_t maxHandlers)
{
    if (m_handle != nullptr) return ESP_OK;

    httpd_config_t config   = HTTPD_DEFAULT_CONFIG();
    config.server_port      = port;
    config.max_uri_handlers = maxHandlers;
    config.max_open_sockets = 5;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout  = 15;
    config.stack_size         = 8192;
    config.lru_purge_enable   = true;
    // Needed for the captive-portal catch-all and for later wildcard routes.
    config.uri_match_fn = httpd_uri_match_wildcard;

    const esp_err_t err = httpd_start(&m_handle, &config);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        m_handle = nullptr;
        return err;
    }

    ESP_LOGI(TAG, "HTTP server listening on port %u", port);
    return ESP_OK;
}

void WebServer::stop()
{
    if (m_handle == nullptr) return;
    httpd_stop(m_handle);
    m_handle = nullptr;
    m_blobs.clear();
}

esp_err_t WebServer::on(const char*    uri,
                        httpd_method_t method,
                        esp_err_t      (*handler)(httpd_req_t*),
                        void*          ctx)
{
    if (m_handle == nullptr) return ESP_ERR_INVALID_STATE;

    httpd_uri_t entry = {};
    entry.uri         = uri;
    entry.method      = method;
    entry.handler     = handler;
    entry.user_ctx    = ctx;

    const esp_err_t err = httpd_register_uri_handler(m_handle, &entry);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "register '%s' failed: %s", uri, esp_err_to_name(err));
    }
    return err;
}

esp_err_t WebServer::serveStatic(const char* uri,
                                 const char* body,
                                 size_t      length,
                                 const char* contentType)
{
    m_blobs.push_back(StaticBlob{body, length, contentType});
    return on(uri, HTTP_GET, &WebServer::staticHandler, &m_blobs.back());
}

esp_err_t WebServer::staticHandler(httpd_req_t* req)
{
    const auto* blob = static_cast<const StaticBlob*>(req->user_ctx);
    if (blob == nullptr) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, blob->contentType);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, blob->body, blob->length);
}

esp_err_t WebServer::setNotFoundHandler(httpd_err_handler_func_t fn)
{
    if (m_handle == nullptr) return ESP_ERR_INVALID_STATE;
    return httpd_register_err_handler(m_handle, HTTPD_404_NOT_FOUND, fn);
}

esp_err_t WebServer::sendJson(httpd_req_t* req, const std::string& json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t WebServer::sendStatus(httpd_req_t* req, const char* status, const std::string& body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t WebServer::readBody(httpd_req_t* req, std::string& out, size_t maxLength)
{
    const size_t total = static_cast<size_t>(req->content_len);
    if (total > maxLength) return ESP_ERR_INVALID_SIZE;

    out.clear();
    out.reserve(total);

    char   buf[256];
    size_t received = 0;
    while (received < total)
    {
        const size_t want = std::min(sizeof(buf), total - received);
        const int    got  = httpd_req_recv(req, buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return ESP_FAIL;
        out.append(buf, static_cast<size_t>(got));
        received += static_cast<size_t>(got);
    }
    return ESP_OK;
}

std::string WebServer::formValue(const std::string& body, const char* key)
{
    const std::string needle = std::string(key) + "=";
    size_t            pos    = 0;

    while (pos <= body.size())
    {
        const size_t end   = body.find('&', pos);
        const size_t count = (end == std::string::npos) ? std::string::npos : end - pos;
        const std::string pair = body.substr(pos, count);

        if (pair.compare(0, needle.size(), needle) == 0)
        {
            return urlDecode(pair.substr(needle.size()));
        }
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return {};
}
