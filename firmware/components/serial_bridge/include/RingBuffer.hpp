#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Mitschnittpuffer für die Konsole des Zielgeräts.
 *
 * Der Puffer läuft immer mit, auch ohne verbundenen Client. Beim Verbinden
 * bekommt der Client den vorhandenen Inhalt nachgeliefert — genau die
 * Boot-Meldungen, die bei einem direkten USB-Kabel verloren gehen, weil dort
 * der Port beim Reset neu enumeriert.
 *
 * Leser arbeiten mit einem absoluten Byte-Zähler als Cursor. Läuft der Puffer
 * über den Cursor hinweg, wird der Cursor vorgezogen und die Zahl der
 * verlorenen Bytes gemeldet, statt still Daten zu unterschlagen.
 */
class RingBuffer
{
public:
    RingBuffer() = default;
    ~RingBuffer();

    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    esp_err_t init(size_t capacity);

    void write(const uint8_t* data, size_t length);

    /**
     * @param[in,out] cursor  absoluter Byte-Index; wird weitergesetzt
     * @param[out]    skipped wie viele Bytes übersprungen wurden (Überlauf)
     * @return Anzahl gelesener Bytes
     */
    size_t read(uint64_t& cursor, uint8_t* out, size_t maxLength, size_t& skipped);

    uint64_t newest() const;
    uint64_t oldest() const;
    size_t   capacity() const { return m_capacity; }

private:
    uint8_t*          m_buffer {nullptr};
    size_t            m_capacity {0};
    uint64_t          m_total {0};  ///< insgesamt je geschriebene Bytes
    SemaphoreHandle_t m_mutex {nullptr};
};
