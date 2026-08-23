#include "OtaUpdater.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG        = "ota";
constexpr size_t      CHUNK_SIZE = 4096;

/// Offset of esp_app_desc_t inside an application image.
constexpr size_t APP_DESC_OFFSET = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);

std::string jsonError(const std::string& message)
{
    return R"({"error":")" + message + R"("})";
}
}  // namespace

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

std::string OtaUpdater::runningPartition() const
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    return (running != nullptr) ? std::string(running->label) : std::string("?");
}

bool OtaUpdater::isPendingVerify() const
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) return false;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

std::string OtaUpdater::version() const
{
    const esp_app_desc_t* desc = esp_app_get_description();
    return (desc != nullptr) ? std::string(desc->version) : std::string("?");
}

std::string OtaUpdater::buildTimestamp() const
{
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc == nullptr) return "?";
    return std::string(desc->date) + " " + desc->time;
}

void OtaUpdater::confirmRunningImage()
{
    if (!isPendingVerify()) return;

    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "image confirmed, rollback cancelled");
    }
    else
    {
        ESP_LOGE(TAG, "could not confirm image: %s", esp_err_to_name(err));
    }
}

// ---------------------------------------------------------------------------
// Upload
// ---------------------------------------------------------------------------

void OtaUpdater::registerRoutes(WebServer& server)
{
    server.on("/api/update", HTTP_POST, &OtaUpdater::handleUpdate, this);
}

esp_err_t OtaUpdater::validateHeader(const uint8_t* data, size_t length, std::string& reason)
{
    if (length < APP_DESC_OFFSET + sizeof(esp_app_desc_t))
    {
        reason = "image too short";
        return ESP_ERR_INVALID_SIZE;
    }
    if (data[0] != ESP_IMAGE_HEADER_MAGIC)
    {
        // The most likely mistake is uploading the UF2 meant for the RP2040.
        reason = "not an ESP32 application image";
        return ESP_ERR_INVALID_ARG;
    }

    esp_app_desc_t incoming = {};
    std::memcpy(&incoming, data + APP_DESC_OFFSET, sizeof(incoming));

    if (incoming.magic_word != ESP_APP_DESC_MAGIC_WORD)
    {
        reason = "missing application descriptor";
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t* running = esp_app_get_description();
    if (running != nullptr &&
        std::strncmp(incoming.project_name, running->project_name, sizeof(incoming.project_name)) != 0)
    {
        reason = std::string("image belongs to project '") + incoming.project_name + "'";
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "incoming image: %s %s (%s %s)",
             incoming.project_name, incoming.version, incoming.date, incoming.time);
    return ESP_OK;
}

esp_err_t OtaUpdater::handleUpdate(httpd_req_t* req)
{
    auto* self = static_cast<OtaUpdater*>(req->user_ctx);

    bool expected = false;
    if (!self->m_busy.compare_exchange_strong(expected, true))
    {
        return WebServer::sendStatus(req, "409 Conflict", jsonError("update already running"));
    }

    struct BusyGuard
    {
        std::atomic<bool>& flag;
        ~BusyGuard() { flag.store(false); }
    } guard{self->m_busy};

    const size_t total = static_cast<size_t>(req->content_len);
    if (total == 0)
    {
        return WebServer::sendStatus(req, "400 Bad Request", jsonError("empty body"));
    }

    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr)
    {
        return WebServer::sendStatus(req, "500 Internal Server Error",
                                     jsonError("no OTA partition available"));
    }
    if (total > target->size)
    {
        return WebServer::sendStatus(req, "413 Payload Too Large",
                                     jsonError("image larger than OTA slot"));
    }

    ESP_LOGI(TAG, "receiving %u bytes into '%s'", static_cast<unsigned>(total), target->label);

    esp_ota_handle_t handle = 0;
    esp_err_t        err    = esp_ota_begin(target, total, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return WebServer::sendStatus(req, "500 Internal Server Error",
                                     jsonError(esp_err_to_name(err)));
    }

    std::vector<uint8_t> buf(CHUNK_SIZE);
    size_t               received = 0;
    bool                 checked  = false;

    while (received < total)
    {
        const size_t want = std::min(CHUNK_SIZE, total - received);
        const int    got  = httpd_req_recv(req, reinterpret_cast<char*>(buf.data()), want);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0)
        {
            ESP_LOGE(TAG, "receive aborted after %u bytes", static_cast<unsigned>(received));
            esp_ota_abort(handle);
            return WebServer::sendStatus(req, "400 Bad Request", jsonError("transfer aborted"));
        }

        if (!checked)
        {
            std::string reason;
            if (validateHeader(buf.data(), static_cast<size_t>(got), reason) != ESP_OK)
            {
                ESP_LOGE(TAG, "rejected: %s", reason.c_str());
                esp_ota_abort(handle);
                return WebServer::sendStatus(req, "400 Bad Request", jsonError(reason));
            }
            checked = true;
        }

        err = esp_ota_write(handle, buf.data(), static_cast<size_t>(got));
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            return WebServer::sendStatus(req, "500 Internal Server Error",
                                         jsonError(esp_err_to_name(err)));
        }
        received += static_cast<size_t>(got);
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK)
    {
        // ESP_ERR_OTA_VALIDATE_FAILED means the image checksum does not match.
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return WebServer::sendStatus(req, "400 Bad Request", jsonError(esp_err_to_name(err)));
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return WebServer::sendStatus(req, "500 Internal Server Error",
                                     jsonError(esp_err_to_name(err)));
    }

    ESP_LOGW(TAG, "update written to '%s', restarting", target->label);
    WebServer::sendJson(req, R"({"status":"ok","partition":")" + std::string(target->label) +
                                 R"(","restarting":true})");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
