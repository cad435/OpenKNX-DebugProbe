#pragma once

#include <atomic>
#include <string>

#include "WebServer.hpp"
#include "esp_err.h"

/**
 * Self-update of the probe over HTTP.
 *
 * The image is pushed to POST /api/update as a raw binary body:
 *
 *     curl --data-binary @build/openknx_debugprobe.bin http://probe.local/api/update
 *
 * Writes go to the inactive OTA slot. Together with
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE the new image only becomes permanent
 * once confirmRunningImage() is called after a healthy start; otherwise the
 * bootloader reverts to the previous slot on the next boot.
 */
class OtaUpdater
{
public:
    OtaUpdater() = default;

    OtaUpdater(const OtaUpdater&)            = delete;
    OtaUpdater& operator=(const OtaUpdater&) = delete;

    void registerRoutes(WebServer& server);

    /// Cancels a pending rollback. Call only after the probe is known healthy.
    void confirmRunningImage();

    /// Label of the OTA slot the probe is currently running from.
    std::string runningPartition() const;

    /// True while the running image is still on probation.
    bool isPendingVerify() const;

    std::string version() const;
    std::string buildTimestamp() const;

private:
    static esp_err_t handleUpdate(httpd_req_t* req);

    /// Rejects anything that is not an ESP32 application image for this project.
    static esp_err_t validateHeader(const uint8_t* data, size_t length, std::string& reason);

    std::atomic<bool> m_busy {false};
};
