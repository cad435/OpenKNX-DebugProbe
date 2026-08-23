#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "ClientActivity.hpp"
#include "RingBuffer.hpp"
#include "UsbTarget.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Telnet/RFC2217-Server auf die Konsole des Zielgeraets.
 *
 * Der Unterschied zum rohen Port 2323: hier kommen **Baudrate und
 * Steuerleitungen** mit durch. Damit koennen Flash-Werkzeuge weiter auf dem PC
 * laufen und nur durch die Probe hindurchreden — inklusive des DTR/RTS-Wippens,
 * mit dem esptool einen ESP32 in den Bootloader zwingt:
 *
 *     esptool --port rfc2217://openknx-probe-xxxx.local:4000 write_flash ...
 *
 * Damit muss die Probe kein einziges Bootloader-Protokoll selbst sprechen.
 *
 * Teilt sich den Mitschnittpuffer mit der rohen Bruecke; beide Server haben
 * einen eigenen Lesecursor und koennen gleichzeitig verbunden sein.
 */
class Rfc2217Server
{
public:
    Rfc2217Server() = default;
    ~Rfc2217Server();

    Rfc2217Server(const Rfc2217Server&)            = delete;
    Rfc2217Server& operator=(const Rfc2217Server&) = delete;

    esp_err_t begin(UsbTarget& target, uint16_t port = 4000, size_t bufferSize = 4096);

    /**
     * Nimmt Daten des Zielgeraets entgegen.
     *
     * Waehrend eine Flash-Sitzung laeuft, leitet SerialBridge den Strom
     * hierher um statt in den Konsolen-Mitschnitt — sonst stuende dort nach
     * jedem Upload binaerer SLIP-Verkehr statt lesbarer Ausgabe.
     */
    void feedFromTarget(const uint8_t* data, size_t length);

    bool     hasClient() const { return m_clientSocket >= 0; }
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

    uint16_t port() const { return m_port; }

private:
    /// Zustand des Telnet-Parsers im Empfangsstrom.
    enum class Parse
    {
        Data,
        Iac,
        Option,
        Subneg,
        SubnegIac,
    };

    static void serverTask(void* arg);
    void        run();
    void        serveClient(int socket);

    void feedFromClient(int socket, const uint8_t* data, size_t length);
    void handleNegotiation(int socket, uint8_t command, uint8_t option);
    void handleSubnegotiation(int socket);
    void applyLineCoding();
    void applyControlLines();

    void sendCommand(int socket, uint8_t command, uint8_t option);
    void sendSubnegotiation(int socket, const uint8_t* payload, size_t length);
    bool sendEscaped(int socket, const uint8_t* data, size_t length);

    UsbTarget*   m_target {nullptr};
    RingBuffer   m_ring;  ///< eigener Puffer, getrennt vom Konsolen-Mitschnitt
    TaskHandle_t m_task {nullptr};
    uint16_t     m_port {4000};

    std::atomic<int> m_clientSocket {-1};

    Parse   m_parse {Parse::Data};
    uint8_t m_pendingCommand {0};
    uint8_t m_subneg[64] {};
    size_t  m_subnegLength {0};

    // Zuletzt vom Client gewuenschte Leitungsparameter.
    uint32_t m_baudRate {115200};
    uint8_t  m_dataBits {8};
    uint8_t  m_parity {1};    ///< RFC2217-Zaehlweise: 1 = keine
    uint8_t  m_stopBits {1};  ///< RFC2217-Zaehlweise: 1 = ein Stoppbit
    bool     m_dtr {false};
    bool     m_rts {false};

    /**
     * DTR und RTS werden gesammelt und erst kurz verzoegert gesetzt.
     *
     * Grund: die Auto-Reset-Schaltung auf ESP32-Boards ist so ausgelegt, dass
     * *beide* Leitungen gleichzeitig aktiv keinen Effekt haben — damit ein
     * Terminal das Board nicht versehentlich resettet. esptool setzt DTR und
     * RTS unmittelbar nacheinander; ueber RFC2217 sind das zwei getrennte
     * Runden ueber WLAN. Im Fenster dazwischen waeren kurz beide aktiv, EN
     * wuerde zu frueh freigegeben und der Chip startet normal statt im
     * Bootloader. Das Zusammenfassen macht daraus einen einzigen USB-Transfer.
     */
    int64_t m_controlDeadlineUs {0};

    ClientActivityHook m_activityHook {nullptr};
    void*              m_activityCtx {nullptr};
};
