#include "CaptiveDns.hpp"

#include <cerrno>
#include <cstring>

#include "esp_log.h"
#include "lwip/sockets.h"

namespace {
constexpr const char* TAG      = "dns";
constexpr uint16_t    DNS_PORT = 53;
constexpr size_t      BUF_SIZE = 512;
}  // namespace

CaptiveDns::~CaptiveDns()
{
    stop();
}

esp_err_t CaptiveDns::start(uint32_t ipv4)
{
    if (m_task != nullptr) return ESP_OK;

    m_addr = ipv4;
    m_stop = false;

    if (xTaskCreate(&CaptiveDns::taskEntry, "captive_dns", 3072, this, 4, &m_task) != pdPASS)
    {
        m_task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void CaptiveDns::stop()
{
    if (m_task == nullptr) return;

    m_stop = true;
    if (m_sock >= 0)
    {
        shutdown(m_sock, SHUT_RDWR);
        close(m_sock);
        m_sock = -1;
    }
    // The task deletes itself once it observes m_stop / the closed socket.
    while (m_task != nullptr) vTaskDelay(pdMS_TO_TICKS(10));
}

void CaptiveDns::taskEntry(void* arg)
{
    static_cast<CaptiveDns*>(arg)->run();
}

void CaptiveDns::run()
{
    m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (m_sock < 0)
    {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        m_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in bindAddr = {};
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port        = htons(DNS_PORT);

    if (bind(m_sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0)
    {
        ESP_LOGE(TAG, "bind() failed: %d", errno);
        close(m_sock);
        m_sock = -1;
        m_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "captive DNS up");

    uint8_t buf[BUF_SIZE];
    while (!m_stop)
    {
        sockaddr_in from    = {};
        socklen_t   fromLen = sizeof(from);

        const int len = recvfrom(m_sock, buf, sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (len < 0) break;                 // socket closed by stop()
        if (len < 12) continue;             // shorter than a DNS header
        if (buf[2] & 0x80) continue;        // already a response
        if ((buf[4] << 8 | buf[5]) == 0) continue;  // no question

        // Walk the first QNAME.
        size_t idx = 12;
        while (idx < static_cast<size_t>(len) && buf[idx] != 0)
        {
            idx += buf[idx] + 1;
        }
        if (idx >= static_cast<size_t>(len)) continue;
        idx += 1;  // terminating zero label
        if (idx + 4 > static_cast<size_t>(len)) continue;

        const uint16_t qtype = static_cast<uint16_t>(buf[idx] << 8 | buf[idx + 1]);
        idx += 4;  // QTYPE + QCLASS

        // Turn the request into a response, keeping only the first question.
        buf[2] = 0x81;  // QR=1, RD=1
        buf[3] = 0x80;  // RA=1, RCODE=0
        buf[4] = 0x00;
        buf[5] = 0x01;                          // QDCOUNT = 1
        buf[6] = 0x00;
        buf[7] = (qtype == 1) ? 0x01 : 0x00;    // ANCOUNT (only A records)
        buf[8] = buf[9] = buf[10] = buf[11] = 0;

        size_t outLen = idx;
        if (qtype == 1)
        {
            const uint8_t answer[12] = {
                0xC0, 0x0C,              // name: pointer to offset 12
                0x00, 0x01,              // type A
                0x00, 0x01,              // class IN
                0x00, 0x00, 0x00, 0x3C,  // TTL 60 s
                0x00, 0x04               // RDLENGTH
            };
            if (outLen + sizeof(answer) + 4 > BUF_SIZE) continue;
            std::memcpy(buf + outLen, answer, sizeof(answer));
            outLen += sizeof(answer);
            std::memcpy(buf + outLen, &m_addr, 4);
            outLen += 4;
        }

        sendto(m_sock, buf, outLen, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
    }

    if (m_sock >= 0)
    {
        close(m_sock);
        m_sock = -1;
    }
    ESP_LOGI(TAG, "captive DNS down");

    m_task = nullptr;
    vTaskDelete(nullptr);
}
