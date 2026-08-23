#include "RingBuffer.hpp"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace {
constexpr const char* TAG = "ring";
}

RingBuffer::~RingBuffer()
{
    if (m_buffer != nullptr) heap_caps_free(m_buffer);
    if (m_mutex != nullptr) vSemaphoreDelete(m_mutex);
}

esp_err_t RingBuffer::init(size_t capacity)
{
    if (capacity == 0) return ESP_ERR_INVALID_ARG;

    // Bevorzugt ins PSRAM — der interne RAM wird für USB- und WiFi-Puffer
    // gebraucht.
    m_buffer = static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM));
    if (m_buffer == nullptr)
    {
        m_buffer = static_cast<uint8_t*>(heap_caps_malloc(capacity, MALLOC_CAP_DEFAULT));
    }
    if (m_buffer == nullptr) return ESP_ERR_NO_MEM;

    m_mutex = xSemaphoreCreateMutex();
    if (m_mutex == nullptr)
    {
        heap_caps_free(m_buffer);
        m_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }

    m_capacity = capacity;
    m_total    = 0;
    ESP_LOGI(TAG, "%u bytes console buffer", static_cast<unsigned>(capacity));
    return ESP_OK;
}

void RingBuffer::write(const uint8_t* data, size_t length)
{
    if (m_buffer == nullptr || data == nullptr || length == 0) return;

    xSemaphoreTake(m_mutex, portMAX_DELAY);

    if (length >= m_capacity)
    {
        // Nur der jüngste Teil passt noch hinein.
        const size_t offset = length - m_capacity;
        std::memcpy(m_buffer, data + offset, m_capacity);
        m_total += length;
    }
    else
    {
        size_t position = static_cast<size_t>(m_total % m_capacity);
        const size_t firstChunk = std::min(length, m_capacity - position);
        std::memcpy(m_buffer + position, data, firstChunk);
        if (firstChunk < length)
        {
            std::memcpy(m_buffer, data + firstChunk, length - firstChunk);
        }
        m_total += length;
    }

    xSemaphoreGive(m_mutex);
}

size_t RingBuffer::read(uint64_t& cursor, uint8_t* out, size_t maxLength, size_t& skipped)
{
    skipped = 0;
    if (m_buffer == nullptr || out == nullptr || maxLength == 0) return 0;

    xSemaphoreTake(m_mutex, portMAX_DELAY);

    const uint64_t oldestIndex = (m_total > m_capacity) ? (m_total - m_capacity) : 0;
    if (cursor < oldestIndex)
    {
        skipped = static_cast<size_t>(oldestIndex - cursor);
        cursor  = oldestIndex;
    }
    if (cursor > m_total) cursor = m_total;

    const size_t available = static_cast<size_t>(m_total - cursor);
    const size_t count     = std::min(available, maxLength);

    if (count > 0)
    {
        const size_t position   = static_cast<size_t>(cursor % m_capacity);
        const size_t firstChunk = std::min(count, m_capacity - position);
        std::memcpy(out, m_buffer + position, firstChunk);
        if (firstChunk < count)
        {
            std::memcpy(out + firstChunk, m_buffer, count - firstChunk);
        }
        cursor += count;
    }

    xSemaphoreGive(m_mutex);
    return count;
}

uint64_t RingBuffer::newest() const
{
    if (m_mutex == nullptr) return 0;
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    const uint64_t value = m_total;
    xSemaphoreGive(m_mutex);
    return value;
}

uint64_t RingBuffer::oldest() const
{
    if (m_mutex == nullptr) return 0;
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    const uint64_t value = (m_total > m_capacity) ? (m_total - m_capacity) : 0;
    xSemaphoreGive(m_mutex);
    return value;
}
