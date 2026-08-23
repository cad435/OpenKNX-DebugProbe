#pragma once

#include <string>

#include "esp_err.h"
#include "nvs.h"

/**
 * RAII wrapper around a single NVS namespace.
 *
 * Owns the NVS handle for its lifetime; the underlying NVS partition is
 * initialised lazily on the first begin() call of any instance.
 */
class Settings
{
public:
    explicit Settings(const char* nsName = "probe");
    ~Settings();

    Settings(const Settings&)            = delete;
    Settings& operator=(const Settings&) = delete;

    /// Initialises NVS (once, process wide) and opens the namespace.
    esp_err_t begin();

    bool        isOpen() const { return m_open; }

    std::string getString(const char* key, const std::string& fallback = {}) const;
    esp_err_t   setString(const char* key, const std::string& value);

    uint32_t    getU32(const char* key, uint32_t fallback = 0) const;
    esp_err_t   setU32(const char* key, uint32_t value);

    bool        has(const char* key) const;
    esp_err_t   erase(const char* key);
    esp_err_t   commit();

private:
    const char*  m_namespace;
    nvs_handle_t m_handle {0};
    bool         m_open   {false};
};
