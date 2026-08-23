#pragma once

#include <atomic>
#include <string>

#include "ClientActivity.hpp"
#include "RingBuffer.hpp"

class Rfc2217Server;
#include "UsbTarget.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Konsole des Zielgeräts über TCP.
 *
 * Ein Client gleichzeitig. Beim Verbinden bekommt er den Mitschnittpuffer
 * nachgeliefert, danach den Livestream. Verschwindet das Zielgerät — Reset,
 * Flash, Kabel raus — **bleibt die TCP-Verbindung bestehen**; sobald das Ziel
 * wieder da ist, läuft der Stream weiter. Genau das ist der Vorteil gegenüber
 * einem direkten USB-Kabel, an dem bei jedem Reset das Terminal aussteigt.
 *
 * PC-Seite: `monitor_port = socket://openknx-probe-xxxx.local:2323`
 */
class SerialBridge
{
public:
    SerialBridge() = default;
    ~SerialBridge();

    SerialBridge(const SerialBridge&)            = delete;
    SerialBridge& operator=(const SerialBridge&) = delete;

    esp_err_t begin(UsbTarget& target, uint16_t port = 2323, size_t bufferSize = 16384);

    bool        hasClient() const { return m_clientSocket >= 0; }
    std::string clientAddress() const;
    uint16_t    port() const { return m_port; }
    uint64_t    bytesFromTarget() const { return m_fromTarget; }
    uint64_t    bytesToTarget() const { return m_toTarget; }
    size_t      bufferSize() const { return m_ring.capacity(); }

    /**
     * Solange dieser Server einen Client hat, gehen die Daten des Zielgeraets
     * dorthin statt in den Konsolen-Mitschnitt. Damit bleibt die Historie
     * lesbar, statt nach jedem Upload voller SLIP-Frames zu stehen.
     */
    void setRfc2217(Rfc2217Server* server) { m_rfc = server; }


    /**
     * Wird gerufen, sobald ein Client kommt oder geht.
     *
     * Genutzt fuer das dynamische WLAN-Stromsparen; verdrahtet in `main.cpp`.
     */
    void setActivityHook(ClientActivityHook hook, void* ctx)
    {
        m_activityHook = hook;
        m_activityCtx  = ctx;
    }

    /// Schreibt eine Statuszeile in den Mitschnitt (z. B. "Ziel abgezogen").
    void note(const char* text);

    /**
     * Schickt Bytes ans Zielgeraet — derselbe Weg, den ein TCP-Client nimmt.
     *
     * Bewusst hier und nicht direkt ueber UsbTarget: so laeuft der Zaehler
     * `bytesToTarget()` fuer beide Wege mit, und /api/status zeigt weiterhin,
     * was wirklich zum Ziel gegangen ist.
     */
    esp_err_t sendToTarget(const uint8_t* data, size_t length);

    /**
     * Lesezugriff auf den Mitschnitt fuer die Web-Konsole.
     *
     * Nutzt denselben absoluten Cursor wie die TCP-Clients: der Browser merkt
     * sich die Position und holt beim naechsten Mal nur das Neue. Laeuft der
     * Puffer ueber, meldet @p skipped die verlorenen Bytes, statt sie still zu
     * unterschlagen.
     */
    size_t readFrom(uint64_t& cursor, uint8_t* out, size_t maxLength, size_t& skipped)
    {
        return m_ring.read(cursor, out, maxLength, skipped);
    }

    uint64_t oldestByte() const { return m_ring.oldest(); }
    uint64_t newestByte() const { return m_ring.newest(); }

private:
    static void serverTask(void* arg);
    static void onTargetData(const uint8_t* data, size_t length, void* ctx);

    void run();
    void serveClient(int socket);

    UsbTarget*        m_target {nullptr};
    Rfc2217Server*    m_rfc {nullptr};
    std::atomic<uint64_t> m_suppressed {0};
    bool              m_lastRfcActive {false};
    RingBuffer        m_ring;
    TaskHandle_t      m_task {nullptr};
    uint16_t          m_port {2323};
    std::atomic<int>  m_clientSocket {-1};
    std::atomic<bool> m_lastTargetOpen {false};
    std::atomic<uint64_t> m_fromTarget {0};
    std::atomic<uint64_t> m_toTarget {0};
    char              m_clientAddress[16] {};
    ClientActivityHook m_activityHook {nullptr};
    void*              m_activityCtx {nullptr};
};
