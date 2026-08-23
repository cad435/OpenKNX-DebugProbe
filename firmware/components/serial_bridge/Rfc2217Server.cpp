#include "Rfc2217Server.hpp"

#include <cerrno>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

namespace {

constexpr const char* TAG = "rfc2217";

// --- Telnet ----------------------------------------------------------------
constexpr uint8_t IAC  = 255;
constexpr uint8_t DONT = 254;
constexpr uint8_t DO   = 253;
constexpr uint8_t WONT = 252;
constexpr uint8_t WILL = 251;
constexpr uint8_t SB   = 250;
constexpr uint8_t SE   = 240;

constexpr uint8_t OPT_BINARY   = 0;
constexpr uint8_t OPT_SGA      = 3;
constexpr uint8_t OPT_COM_PORT = 44;

// --- RFC2217: Client -> Server ---------------------------------------------
constexpr uint8_t SET_BAUDRATE        = 1;
constexpr uint8_t SET_DATASIZE        = 2;
constexpr uint8_t SET_PARITY          = 3;
constexpr uint8_t SET_STOPSIZE        = 4;
constexpr uint8_t SET_CONTROL         = 5;
constexpr uint8_t SET_LINESTATE_MASK  = 10;
constexpr uint8_t SET_MODEMSTATE_MASK = 11;
constexpr uint8_t PURGE_DATA          = 12;

/// Antworten des Servers tragen dieselbe Nummer plus 100.
constexpr uint8_t SERVER_OFFSET = 100;

// SET_CONTROL-Werte. Die geraden "REQ"-Codes sind Abfragen: darauf gehoert der
// *aktuelle Zustand* als Antwort, nicht der Anfragecode selbst. Wer das falsch
// beantwortet, laesst pyserial in seinen 3-Sekunden-Timeout laufen.
constexpr uint8_t CTL_REQ_FLOW  = 0;
constexpr uint8_t CTL_NO_FLOW   = 1;
constexpr uint8_t CTL_REQ_BREAK = 4;
constexpr uint8_t CTL_BREAK_ON  = 5;
constexpr uint8_t CTL_BREAK_OFF = 6;
constexpr uint8_t CTL_DTR_ON    = 8;
constexpr uint8_t CTL_DTR_OFF   = 9;
constexpr uint8_t CTL_RTS_ON    = 11;
constexpr uint8_t CTL_RTS_OFF   = 12;
constexpr uint8_t CTL_REQ_DTR   = 7;
constexpr uint8_t CTL_REQ_RTS   = 10;
constexpr uint8_t CTL_REQ_FLOW_IN = 13;
constexpr uint8_t CTL_NO_FLOW_IN  = 14;

constexpr size_t CHUNK = 512;

/// Zustand, auf den nach einer Flash-Sitzung zurueckgestellt wird. Ein
/// Flash-Werkzeug darf die Konsole nicht mit seiner Arbeitsbaudrate
/// hinterlassen — sonst liest der Monitor auf 2323 danach nur Muell.
constexpr uint32_t CONSOLE_DEFAULT_BAUD = 115200;

/**
 * Wie lange der Zustand "DTR und RTS gleichzeitig aktiv" zurueckgehalten wird.
 *
 * Auf der Auto-Reset-Schaltung von ESP32-Boards ist dieser Zustand definiert
 * wirkungslos (EN und IO0 beide high) — er existiert nur, damit ein Terminal
 * das Board nicht versehentlich resettet. Auf echter Hardware tritt er beim
 * Bootloader-Einstieg nie auf, weil der Treiber beide Bits in einem einzigen
 * Transfer wechselt.
 *
 * Ueber RFC2217 quittiert pyserial jede Leitung einzeln, wodurch rund 50 ms
 * dazwischen liegen. Der Zwischenzustand wuerde EN freigeben, waehrend IO0
 * noch high ist — der Chip startet normal statt im Bootloader. Deshalb wird
 * genau dieser eine Zustand verzoegert; jede andere Aenderung ueberholt ihn
 * und wird sofort gesetzt. Damit wird aus (0,1) -> (1,1) -> (1,0) effektiv
 * der atomare Uebergang (0,1) -> (1,0), den die Schaltung braucht.
 */
constexpr int64_t BOTH_ASSERTED_DEFER_US = 150000;

bool sendAll(int socket, const uint8_t* data, size_t length)
{
    size_t sent = 0;
    while (sent < length)
    {
        const int written = send(socket, data + sent, length - sent, 0);
        if (written <= 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(written);
    }
    return true;
}

}  // namespace

Rfc2217Server::~Rfc2217Server()
{
    const int socket = m_clientSocket.exchange(-1);
    if (socket >= 0) close(socket);
}

esp_err_t Rfc2217Server::begin(UsbTarget& target, uint16_t port, size_t bufferSize)
{
    m_target = &target;
    m_port   = port;

    const esp_err_t err = m_ring.init(bufferSize);
    if (err != ESP_OK) return err;

    if (xTaskCreatePinnedToCore(&Rfc2217Server::serverTask, "rfc2217", 5120, this, 6,
                                &m_task, 0) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void Rfc2217Server::feedFromTarget(const uint8_t* data, size_t length)
{
    m_ring.write(data, length);
}

void Rfc2217Server::serverTask(void* arg)
{
    static_cast<Rfc2217Server*>(arg)->run();
}

// ---------------------------------------------------------------------------
// Senden
// ---------------------------------------------------------------------------

void Rfc2217Server::sendCommand(int socket, uint8_t command, uint8_t option)
{
    const uint8_t frame[3] = {IAC, command, option};
    sendAll(socket, frame, sizeof(frame));
}

void Rfc2217Server::sendSubnegotiation(int socket, const uint8_t* payload, size_t length)
{
    uint8_t frame[80];
    size_t  index = 0;

    frame[index++] = IAC;
    frame[index++] = SB;
    frame[index++] = OPT_COM_PORT;

    for (size_t i = 0; i < length && index + 3 < sizeof(frame); ++i)
    {
        frame[index++] = payload[i];
        if (payload[i] == IAC) frame[index++] = IAC;
    }

    frame[index++] = IAC;
    frame[index++] = SE;
    sendAll(socket, frame, index);
}

bool Rfc2217Server::sendEscaped(int socket, const uint8_t* data, size_t length)
{
    // 0xFF im Nutzdatenstrom muss verdoppelt werden, sonst liest der Client es
    // als Beginn eines Telnet-Kommandos.
    uint8_t out[CHUNK * 2];
    size_t  index = 0;

    for (size_t i = 0; i < length && i < CHUNK; ++i)
    {
        out[index++] = data[i];
        if (data[i] == IAC) out[index++] = IAC;
    }
    return sendAll(socket, out, index);
}

// ---------------------------------------------------------------------------
// Telnet-Aushandlung
// ---------------------------------------------------------------------------

void Rfc2217Server::handleNegotiation(int socket, uint8_t command, uint8_t option)
{
    const bool supported =
        (option == OPT_COM_PORT || option == OPT_BINARY || option == OPT_SGA);

    switch (command)
    {
        case DO:
            sendCommand(socket, supported ? WILL : WONT, option);
            break;
        case WILL:
            sendCommand(socket, supported ? DO : DONT, option);
            break;
        case DONT:
        case WONT:
            // Bestaetigung der Gegenseite. Hier NICHT antworten, sonst
            // schaukeln sich beide Seiten endlos hoch.
            break;
        default:
            break;
    }

    if (option == OPT_COM_PORT && (command == DO || command == WILL))
    {
        ESP_LOGI(TAG, "client speaks RFC2217 (%s)", (command == DO) ? "DO" : "WILL");
    }
}

// ---------------------------------------------------------------------------
// RFC2217-Subnegotiation
// ---------------------------------------------------------------------------

void Rfc2217Server::applyLineCoding()
{
    if (m_target == nullptr || !m_target->isSerialOpen()) return;

    // RFC2217 zaehlt anders als USB-CDC.
    const uint8_t cdcParity = (m_parity >= 1) ? static_cast<uint8_t>(m_parity - 1) : 0;

    uint8_t cdcStop = 0;                    // 1 Stoppbit
    if (m_stopBits == 2) cdcStop = 2;       // 2 Stoppbits
    else if (m_stopBits == 3) cdcStop = 1;  // 1,5 Stoppbits

    m_target->setLineCoding(m_baudRate, m_dataBits, cdcParity, cdcStop);
}

void Rfc2217Server::applyControlLines()
{
    if (m_controlDeadlineUs == 0) return;
    if (esp_timer_get_time() < m_controlDeadlineUs) return;

    m_controlDeadlineUs = 0;
    if (m_target != nullptr)
    {
        m_target->setControlLines(m_dtr, m_rts);
        ESP_LOGI(TAG, "DTR=%d RTS=%d (verzoegert)", m_dtr ? 1 : 0, m_rts ? 1 : 0);
    }
}

void Rfc2217Server::handleSubnegotiation(int socket)
{
    if (m_subnegLength < 2 || m_subneg[0] != OPT_COM_PORT) return;

    const uint8_t  command = m_subneg[1];
    const uint8_t* value   = m_subneg + 2;
    const size_t   length  = m_subnegLength - 2;

    ESP_LOGD(TAG, "subneg cmd=%u len=%u", command, static_cast<unsigned>(length));

    uint8_t reply[8];
    size_t  replyLength = 0;

    switch (command)
    {
        case SET_BAUDRATE:
        {
            if (length >= 4)
            {
                const uint32_t requested = (static_cast<uint32_t>(value[0]) << 24) |
                                           (static_cast<uint32_t>(value[1]) << 16) |
                                           (static_cast<uint32_t>(value[2]) << 8) |
                                           static_cast<uint32_t>(value[3]);
                if (requested != 0)
                {
                    m_baudRate = requested;
                    applyLineCoding();
                    ESP_LOGI(TAG, "baud rate -> %lu",
                             static_cast<unsigned long>(m_baudRate));
                }
            }
            reply[replyLength++] = SET_BAUDRATE + SERVER_OFFSET;
            reply[replyLength++] = static_cast<uint8_t>(m_baudRate >> 24);
            reply[replyLength++] = static_cast<uint8_t>(m_baudRate >> 16);
            reply[replyLength++] = static_cast<uint8_t>(m_baudRate >> 8);
            reply[replyLength++] = static_cast<uint8_t>(m_baudRate);
            break;
        }

        case SET_DATASIZE:
            if (length >= 1 && value[0] >= 5 && value[0] <= 8)
            {
                m_dataBits = value[0];
                applyLineCoding();
            }
            reply[replyLength++] = SET_DATASIZE + SERVER_OFFSET;
            reply[replyLength++] = m_dataBits;
            break;

        case SET_PARITY:
            if (length >= 1 && value[0] >= 1 && value[0] <= 5)
            {
                m_parity = value[0];
                applyLineCoding();
            }
            reply[replyLength++] = SET_PARITY + SERVER_OFFSET;
            reply[replyLength++] = m_parity;
            break;

        case SET_STOPSIZE:
            if (length >= 1 && value[0] >= 1 && value[0] <= 3)
            {
                m_stopBits = value[0];
                applyLineCoding();
            }
            reply[replyLength++] = SET_STOPSIZE + SERVER_OFFSET;
            reply[replyLength++] = m_stopBits;
            break;

        case SET_CONTROL:
        {
            uint8_t answer = (length >= 1) ? value[0] : 0;
            if (length >= 1 && m_target != nullptr)
            {
                bool changed = true;
                switch (value[0])
                {
                    case CTL_DTR_ON:  m_dtr = true;  break;
                    case CTL_DTR_OFF: m_dtr = false; break;
                    case CTL_RTS_ON:  m_rts = true;  break;
                    case CTL_RTS_OFF: m_rts = false; break;

                    // Abfragen: aktuellen Zustand melden.
                    case CTL_REQ_DTR:
                        answer  = m_dtr ? CTL_DTR_ON : CTL_DTR_OFF;
                        changed = false;
                        break;
                    case CTL_REQ_RTS:
                        answer  = m_rts ? CTL_RTS_ON : CTL_RTS_OFF;
                        changed = false;
                        break;
                    case CTL_REQ_FLOW:
                        answer  = CTL_NO_FLOW;
                        changed = false;
                        break;
                    case CTL_REQ_FLOW_IN:
                        answer  = CTL_NO_FLOW_IN;
                        changed = false;
                        break;
                    case CTL_REQ_BREAK:
                        answer  = CTL_BREAK_OFF;
                        changed = false;
                        break;

                    case CTL_BREAK_ON:
                    case CTL_BREAK_OFF:
                        changed = false;  // Break wird noch nicht durchgereicht
                        break;
                    default:
                        changed = false;
                        break;
                }
                if (changed)
                {
                    if (m_dtr && m_rts)
                    {
                        // Wirkungsloser Zwischenzustand: zurueckhalten und
                        // abwarten, ob gleich die eigentliche Aenderung kommt.
                        m_controlDeadlineUs = esp_timer_get_time() + BOTH_ASSERTED_DEFER_US;
                    }
                    else
                    {
                        m_controlDeadlineUs = 0;
                        m_target->setControlLines(m_dtr, m_rts);
                        ESP_LOGI(TAG, "DTR=%d RTS=%d", m_dtr ? 1 : 0, m_rts ? 1 : 0);
                    }
                }
            }
            reply[replyLength++] = SET_CONTROL + SERVER_OFFSET;
            reply[replyLength++] = answer;
            break;
        }

        case SET_LINESTATE_MASK:
        case SET_MODEMSTATE_MASK:
        case PURGE_DATA:
            reply[replyLength++] = static_cast<uint8_t>(command + SERVER_OFFSET);
            reply[replyLength++] = (length >= 1) ? value[0] : 0;
            break;

        default:
            // Nicht stillschweigend verwerfen: der Client wartet sonst in
            // seinen Timeout hinein.
            ESP_LOGW(TAG, "unbeantwortete Subnegotiation cmd=%u", command);
            return;
    }

    if (replyLength > 0) sendSubnegotiation(socket, reply, replyLength);
}

// ---------------------------------------------------------------------------
// Empfang
// ---------------------------------------------------------------------------

void Rfc2217Server::feedFromClient(int socket, const uint8_t* data, size_t length)
{
    uint8_t payload[CHUNK];
    size_t  payloadLength = 0;

    auto flush = [&]() {
        if (payloadLength > 0 && m_target != nullptr && m_target->isSerialOpen())
        {
            m_target->write(payload, payloadLength);
        }
        payloadLength = 0;
    };

    for (size_t i = 0; i < length; ++i)
    {
        const uint8_t byte = data[i];

        switch (m_parse)
        {
            case Parse::Data:
                if (byte == IAC)
                {
                    m_parse = Parse::Iac;
                }
                else
                {
                    payload[payloadLength++] = byte;
                }
                break;

            case Parse::Iac:
                if (byte == IAC)
                {
                    payload[payloadLength++] = IAC;  // verdoppelt = Nutzdatum
                    m_parse                  = Parse::Data;
                }
                else if (byte == WILL || byte == WONT || byte == DO || byte == DONT)
                {
                    m_pendingCommand = byte;
                    m_parse          = Parse::Option;
                }
                else if (byte == SB)
                {
                    m_subnegLength = 0;
                    m_parse        = Parse::Subneg;
                }
                else
                {
                    m_parse = Parse::Data;  // sonstige Telnet-Kommandos ignorieren
                }
                break;

            case Parse::Option:
                flush();
                handleNegotiation(socket, m_pendingCommand, byte);
                m_parse = Parse::Data;
                break;

            case Parse::Subneg:
                if (byte == IAC)
                {
                    m_parse = Parse::SubnegIac;
                }
                else if (m_subnegLength < sizeof(m_subneg))
                {
                    m_subneg[m_subnegLength++] = byte;
                }
                break;

            case Parse::SubnegIac:
                if (byte == SE)
                {
                    flush();
                    handleSubnegotiation(socket);
                    m_parse = Parse::Data;
                }
                else
                {
                    if (m_subnegLength < sizeof(m_subneg)) m_subneg[m_subnegLength++] = byte;
                    m_parse = Parse::Subneg;
                }
                break;
        }

        if (payloadLength >= sizeof(payload)) flush();
    }

    flush();
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

void Rfc2217Server::run()
{
    while (true)
    {
        const int listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket < 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int enable = 1;
        setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

        sockaddr_in address    = {};
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

        ESP_LOGI(TAG, "RFC2217 on tcp/%u", m_port);

        while (true)
        {
            sockaddr_in peer    = {};
            socklen_t   peerLen = sizeof(peer);
            const int   client =
                accept(listenSocket, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (client < 0) break;

            char addressText[16] = {};
            inet_ntoa_r(peer.sin_addr, addressText, sizeof(addressText) - 1);
            ESP_LOGI(TAG, "client %s connected", addressText);

            int noDelay = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

            m_clientSocket = client;
            serveClient(client);
            m_clientSocket = -1;

            // Ein abgestuerztes Flash-Werkzeug darf das Ziel nicht im Reset
            // oder im Bootloader zuruecklassen — und auch nicht mit einer
            // fremden Baudrate.
            m_dtr      = false;
            m_rts      = false;
            m_dataBits = 8;
            m_parity   = 1;
            m_stopBits = 1;

            if (m_target != nullptr)
            {
                m_target->resetControlLines();
                if (m_baudRate != CONSOLE_DEFAULT_BAUD)
                {
                    m_baudRate = CONSOLE_DEFAULT_BAUD;
                    applyLineCoding();
                    ESP_LOGI(TAG, "Baudrate auf %lu zurueckgesetzt",
                             static_cast<unsigned long>(CONSOLE_DEFAULT_BAUD));
                }
            }

            close(client);
            ESP_LOGI(TAG, "client %s disconnected", addressText);
        }

        close(listenSocket);
    }
}

void Rfc2217Server::serveClient(int socket)
{
    // Ein Flash-Werkzeug ist der latenzkritischste Fall ueberhaupt: hier darf
    // der Funk auf keinen Fall dosen.
    ClientActivityScope awake(m_activityHook, m_activityCtx);

    m_parse             = Parse::Data;
    m_subnegLength      = 0;
    m_controlDeadlineUs = 0;

    // Wir bieten von uns aus an, was pyserial erwartet. Der Client bestaetigt
    // mit DO/WILL, danach laufen die Subnegotiationen.
    sendCommand(socket, WILL, OPT_COM_PORT);
    sendCommand(socket, DO, OPT_COM_PORT);
    sendCommand(socket, WILL, OPT_BINARY);
    sendCommand(socket, DO, OPT_BINARY);
    sendCommand(socket, WILL, OPT_SGA);
    sendCommand(socket, DO, OPT_SGA);

    // Nur Neues weiterreichen: ein Flash-Werkzeug will keinen Mitschnitt aus
    // der Vergangenheit im Protokollstrom haben.
    uint64_t cursor = m_ring.newest();

    uint8_t buffer[CHUNK];

    while (true)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);

        timeval timeout = {};
        timeout.tv_sec  = 0;
        timeout.tv_usec = (m_controlDeadlineUs != 0) ? 1000 : 20000;

        const int ready = select(socket + 1, &readable, nullptr, nullptr, &timeout);
        applyControlLines();
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            return;
        }

        if (ready > 0 && FD_ISSET(socket, &readable))
        {
            const int received = recv(socket, buffer, sizeof(buffer), 0);
            if (received == 0) return;
            if (received < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return;
            }
            feedFromClient(socket, buffer, static_cast<size_t>(received));
        }

        while (true)
        {
            size_t       skipped = 0;
            const size_t count   = m_ring.read(cursor, buffer, sizeof(buffer), skipped);
            if (count == 0) break;
            if (!sendEscaped(socket, buffer, count)) return;
        }
    }
}
