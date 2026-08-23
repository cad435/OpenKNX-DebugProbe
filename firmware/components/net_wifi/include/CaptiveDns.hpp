#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Minimal DNS responder that answers every A query with one fixed address.
 *
 * This is what makes phones and Windows pop up the "sign in to network" dialog
 * while the probe is in provisioning mode. Only active during the portal.
 */
class CaptiveDns
{
public:
    CaptiveDns() = default;
    ~CaptiveDns();

    CaptiveDns(const CaptiveDns&)            = delete;
    CaptiveDns& operator=(const CaptiveDns&) = delete;

    /// @param ipv4 address in network byte order (as in esp_netif_ip_info_t::ip.addr)
    esp_err_t start(uint32_t ipv4);
    void      stop();

    bool isRunning() const { return m_task != nullptr; }

private:
    static void taskEntry(void* arg);
    void        run();

    uint32_t      m_addr {0};
    int           m_sock {-1};
    TaskHandle_t  m_task {nullptr};
    volatile bool m_stop {false};
};
