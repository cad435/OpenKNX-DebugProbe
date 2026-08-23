#include "SerialBridge.hpp"

#include "Rfc2217Server.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "lwip/sockets.h"

namespace {
constexpr const char* TAG = "bridge";

/// Wieviel pro Runde zum Client geschoben wird.
constexpr size_t CHUNK = 1024;
}  // namespace

SerialBridge::~SerialBridge()
{
    const int socket = m_clientSocket.exchange(-1);
    if (socket >= 0) close(socket);
}

esp_err_t SerialBridge::begin(UsbTarget& target, uint16_t port, size_t bufferSize)
{
    m_target = &target;
    m_port   = port;

    const esp_err_t err = m_ring.init(bufferSize);
    if (err != ESP_OK) return err;

    target.setRxCallback(&SerialBridge::onTargetData, this);

    if (xTaskCreatePinnedToCore(&SerialBridge::serverTask, "serial_bridge", 5120, this, 6,
                                &m_task, 0) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void SerialBridge::onTargetData(const uint8_t* data, size_t length, void* ctx)
{
    auto* self = static_cast<SerialBridge*>(ctx);
    self->m_fromTarget += length;

    // Weiche: waehrend einer Flash-Sitzung bekommt der RFC2217-Client den
    // Strom, und der Konsolen-Mitschnitt bleibt sauber.
    if (self->m_rfc != nullptr && self->m_rfc->hasClient())
    {
        self->m_rfc->feedFromTarget(data, length);
        self->m_suppressed += length;
        return;
    }
    self->m_ring.write(data, length);
}

esp_err_t SerialBridge::sendToTarget(const uint8_t* data, size_t length)
{
    if (m_target == nullptr) return ESP_ERR_INVALID_STATE;
    if (length == 0) return ESP_OK;

    const esp_err_t err = m_target->write(data, length);
    if (err == ESP_OK) m_toTarget += static_cast<uint64_t>(length);
    return err;
}

void SerialBridge::note(const char* text)
{
    char line[96];
    const int written = snprintf(line, sizeof(line), "\r\n[probe] %s\r\n", text);
    if (written > 0)
    {
        m_ring.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(written));
    }
}

std::string SerialBridge::clientAddress() const
{
    return (m_clientSocket >= 0) ? std::string(m_clientAddress) : std::string();
}

void SerialBridge::serverTask(void* arg)
{
    static_cast<SerialBridge*>(arg)->run();
}

void SerialBridge::run()
{
    while (true)
    {
        const int listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket < 0)
        {
            ESP_LOGE(TAG, "socket() failed: %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int enable = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

        sockaddr_in address = {};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port        = htons(m_port);

        if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
            listen(listenSocket, 1) < 0)
        {
            ESP_LOGE(TAG, "bind/listen on %u failed: %d", m_port, errno);
            close(listenSocket);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "console on tcp/%u", m_port);

        while (true)
        {
            sockaddr_in peer    = {};
            socklen_t   peerLen = sizeof(peer);
            const int   client  = accept(listenSocket, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (client < 0)
            {
                ESP_LOGE(TAG, "accept failed: %d", errno);
                break;
            }

            inet_ntoa_r(peer.sin_addr, m_clientAddress, sizeof(m_clientAddress) - 1);
            ESP_LOGI(TAG, "client %s connected", m_clientAddress);

            int noDelay = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

            m_clientSocket = client;
            serveClient(client);
            m_clientSocket = -1;

            close(client);
            ESP_LOGI(TAG, "client %s disconnected", m_clientAddress);
            m_clientAddress[0] = '\0';
        }

        close(listenSocket);
    }
}

void SerialBridge::serveClient(int socket)
{
    // Solange dieser Client haengt, bleibt der Funk wach — sonst kaeme der
    // Livestream der Konsole nur im Beacon-Takt an.
    ClientActivityScope awake(m_activityHook, m_activityCtx);

    // Kurze Statuszeile direkt an den Client — nicht in den Mitschnitt, damit
    // sie nicht bei jedem weiteren Verbinden erneut auftaucht. Ohne die sitzt
    // man sonst vor einem stummen Terminal und weiss nicht, ob ueberhaupt ein
    // Ziel angesteckt ist.
    {
        const UsbTarget::DeviceInfo info = m_target->device();
        char       banner[192];
        const bool attached = (m_target->state() == UsbTarget::State::Connected);

        const int written = snprintf(
            banner, sizeof(banner), "\r\n[probe] Konsole verbunden. Ziel: %s%s\r\n",
            attached ? info.description.c_str() : "keines angesteckt",
            (attached && !m_target->isSerialOpen()) ? " (keine serielle Sitzung)" : "");
        if (written > 0) send(socket, banner, static_cast<size_t>(written), 0);
    }

    // Danach der Mitschnitt — das sind die Boot-Meldungen, die vor dem
    // Verbinden aufgelaufen sind.
    uint64_t cursor = m_ring.oldest();

    uint8_t buffer[CHUNK];
    m_lastTargetOpen = m_target->isSerialOpen();

    while (true)
    {
        // Wechsel des Zielzustands als Zeile in den Stream einblenden, damit im
        // Terminal sichtbar ist, wo ein Reset lag.
        const bool open = m_target->isSerialOpen();
        if (open != m_lastTargetOpen)
        {
            m_lastTargetOpen = open;
            note(open ? "Ziel verbunden" : "Ziel abgezogen, Verbindung bleibt offen");
        }

        // Flash-Sitzung sichtbar machen, statt den Mitschnitt still abreissen
        // zu lassen.
        const bool rfcActive = (m_rfc != nullptr && m_rfc->hasClient());
        if (rfcActive != m_lastRfcActive)
        {
            m_lastRfcActive = rfcActive;
            if (rfcActive)
            {
                m_suppressed = 0;
                note("Flash-Sitzung laeuft, Konsole pausiert");
            }
            else
            {
                char text[80];
                snprintf(text, sizeof(text),
                         "Flash-Sitzung beendet, %llu Bytes ausgeblendet",
                         static_cast<unsigned long long>(m_suppressed.load()));
                note(text);
            }
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);

        timeval timeout = {};
        timeout.tv_sec  = 0;
        timeout.tv_usec = 20000;  // 20 ms

        const int ready = select(socket + 1, &readable, nullptr, nullptr, &timeout);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            return;
        }

        if (ready > 0 && FD_ISSET(socket, &readable))
        {
            const int received = recv(socket, buffer, sizeof(buffer), 0);
            if (received == 0) return;  // Client hat zugemacht
            if (received < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return;
            }
            if (m_target->isSerialOpen())
            {
                if (m_target->write(buffer, static_cast<size_t>(received)) == ESP_OK)
                {
                    m_toTarget += static_cast<uint64_t>(received);
                }
            }
        }

        // Alles, was seit dem letzten Durchlauf vom Ziel kam, an den Client.
        while (true)
        {
            size_t       skipped = 0;
            const size_t count   = m_ring.read(cursor, buffer, sizeof(buffer), skipped);
            if (skipped > 0)
            {
                ESP_LOGW(TAG, "client too slow, dropped %u bytes",
                         static_cast<unsigned>(skipped));
            }
            if (count == 0) break;

            size_t sent = 0;
            while (sent < count)
            {
                const int written = send(socket, buffer + sent, count - sent, 0);
                if (written <= 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        vTaskDelay(pdMS_TO_TICKS(5));
                        continue;
                    }
                    return;
                }
                sent += static_cast<size_t>(written);
            }
        }
    }
}
