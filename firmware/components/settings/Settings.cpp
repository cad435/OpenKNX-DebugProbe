#include "Settings.hpp"

#include <vector>

#include "esp_log.h"
#include "nvs_flash.h"

namespace {
constexpr const char* TAG        = "settings";
bool                  s_nvsReady = false;
}  // namespace

Settings::Settings(const char* nsName) : m_namespace(nsName) {}

Settings::~Settings()
{
    if (m_open)
    {
        nvs_close(m_handle);
        m_open = false;
    }
}

esp_err_t Settings::begin()
{
    if (!s_nvsReady)
    {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_LOGW(TAG, "NVS unusable (%s), erasing", esp_err_to_name(err));
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_nvsReady = true;
    }

    if (m_open) return ESP_OK;

    const esp_err_t err = nvs_open(m_namespace, NVS_READWRITE, &m_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open('%s') failed: %s", m_namespace, esp_err_to_name(err));
        return err;
    }
    m_open = true;
    return ESP_OK;
}

std::string Settings::getString(const char* key, const std::string& fallback) const
{
    if (!m_open) return fallback;

    size_t    len = 0;
    esp_err_t err = nvs_get_str(m_handle, key, nullptr, &len);
    if (err != ESP_OK || len == 0) return fallback;

    std::vector<char> buf(len);
    err = nvs_get_str(m_handle, key, buf.data(), &len);
    if (err != ESP_OK) return fallback;

    return std::string(buf.data());
}

esp_err_t Settings::setString(const char* key, const std::string& value)
{
    if (!m_open) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = nvs_set_str(m_handle, key, value.c_str());
    return (err == ESP_OK) ? commit() : err;
}

uint32_t Settings::getU32(const char* key, uint32_t fallback) const
{
    if (!m_open) return fallback;
    uint32_t value = 0;
    return (nvs_get_u32(m_handle, key, &value) == ESP_OK) ? value : fallback;
}

esp_err_t Settings::setU32(const char* key, uint32_t value)
{
    if (!m_open) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = nvs_set_u32(m_handle, key, value);
    return (err == ESP_OK) ? commit() : err;
}

bool Settings::has(const char* key) const
{
    if (!m_open) return false;
    size_t len = 0;
    return nvs_get_str(m_handle, key, nullptr, &len) == ESP_OK;
}

esp_err_t Settings::erase(const char* key)
{
    if (!m_open) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = nvs_erase_key(m_handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    return (err == ESP_OK) ? commit() : err;
}

esp_err_t Settings::commit()
{
    if (!m_open) return ESP_ERR_INVALID_STATE;
    return nvs_commit(m_handle);
}
