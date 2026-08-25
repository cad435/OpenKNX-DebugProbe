#include "StatusLed.hpp"

#include "esp_log.h"
#include "led_strip.h"

namespace {

constexpr const char* TAG = "led";

/// Rendertakt. 50 Hz ist fuer ein Pulsieren mehr als genug und laesst dem
/// USB-Host und lwIP den Kern.
constexpr uint32_t TICK_MS = 20;

/// Dauer eines vollen Pulsierens im Leerlauf.
constexpr uint32_t BREATHE_MS = 2400;

/// Blinktakt beim Flashen: schnell genug, um als Warnung zu lesen.
constexpr uint32_t BLINK_MS = 320;

/// Blinktakt fuer "Akku fast leer": 1 s Periode, also ruhiger als die
/// Flash-Warnung. Ein leerer Akku ist dringend, aber nicht hektisch.
constexpr uint32_t BLINK_SLOW_MS = 500;

/**
 * Gelb und Orange auf einem WS2812.
 *
 * Gleiche Anteile Rot und Gruen ergeben kein Gelb, sondern ein giftiges
 * Gruengelb — die Gruenchips dieser LEDs sind deutlich effizienter. Der
 * Gruenanteil wird deshalb heruntergezogen.
 */
constexpr uint8_t YELLOW_GREEN_PERCENT = 55;
constexpr uint8_t ORANGE_GREEN_PERCENT = 22;

}  // namespace

StatusLed::~StatusLed()
{
    m_stop.store(true);
    // Der Task raeumt den Strip selbst ab, sobald er das Stop-Flag sieht.
    if (m_task != nullptr) vTaskDelay(pdMS_TO_TICKS(TICK_MS * 3));
}

const char* StatusLed::modeName(Mode mode)
{
    switch (mode)
    {
        case Mode::Flashing:    return "flashing";
        case Mode::BatteryLow:  return "battery_low";
        case Mode::NoWifi:      return "no_wifi";
        case Mode::Busy:        return "busy";
        case Mode::TargetReady: return "target_ready";
        case Mode::Idle:        return "idle";
    }
    return "unknown";
}

esp_err_t StatusLed::openStrip(gpio_num_t pin)
{
    closeStrip();

    led_strip_config_t stripCfg = {};
    stripCfg.strip_gpio_num     = pin;
    stripCfg.max_leds           = 1;
    stripCfg.led_pixel_format   = LED_PIXEL_FORMAT_GRB;
    stripCfg.led_model          = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmtCfg = {};
    rmtCfg.clk_src        = RMT_CLK_SRC_DEFAULT;
    rmtCfg.resolution_hz  = 10 * 1000 * 1000;  // 10 MHz, 0,1 us Aufloesung

    led_strip_handle_t handle = nullptr;
    const esp_err_t    err    = led_strip_new_rmt_device(&stripCfg, &rmtCfg, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "led_strip on GPIO%d failed: %s", static_cast<int>(pin),
                 esp_err_to_name(err));
        return err;
    }

    m_strip = handle;
    m_pin   = pin;
    ESP_LOGI(TAG, "status LED on GPIO%d", static_cast<int>(pin));
    return ESP_OK;
}

void StatusLed::closeStrip()
{
    if (m_strip == nullptr) return;
    auto handle = static_cast<led_strip_handle_t>(m_strip);
    m_strip     = nullptr;
    led_strip_del(handle);
}

void StatusLed::write(uint8_t r, uint8_t g, uint8_t b)
{
    if (m_strip == nullptr) return;
    auto handle = static_cast<led_strip_handle_t>(m_strip);
    led_strip_set_pixel(handle, 0, r, g, b);
    led_strip_refresh(handle);
}

uint8_t StatusLed::scale(uint8_t value) const
{
    // Quadratisch: die Wahrnehmung ist logarithmisch, ein linearer Verlauf
    // sieht in der oberen Haelfte wie Stillstand aus.
    const uint32_t squared = (static_cast<uint32_t>(value) * value) / 255;
    return static_cast<uint8_t>((squared * m_brightness) / 255);
}

esp_err_t StatusLed::begin(gpio_num_t pin, uint8_t brightness)
{
    if (m_task != nullptr) return ESP_ERR_INVALID_STATE;

    m_brightness = brightness;

    const esp_err_t err = openStrip(pin);
    if (err != ESP_OK) return err;

    if (xTaskCreatePinnedToCore(&StatusLed::task, "status_led", 3072, this, 3, &m_task, 0) !=
        pdPASS)
    {
        closeStrip();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t StatusLed::setPin(gpio_num_t pin)
{
    if (pin == m_pin && m_strip != nullptr) return ESP_OK;

    // Nicht hier umschalten: der Rendertask haelt den Handle. Er baut neu auf,
    // sobald er das Flag sieht — sonst gaebe es einen Handle, den zwei Tasks
    // gleichzeitig anfassen.
    m_pin = pin;
    m_reopen.store(true);

    for (int i = 0; i < 25 && m_reopen.load(); ++i) vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    return m_strip != nullptr ? ESP_OK : ESP_FAIL;
}

void StatusLed::task(void* arg)
{
    static_cast<StatusLed*>(arg)->run();
}

void StatusLed::run()
{
    uint32_t phase = 0;

    while (!m_stop.load())
    {
        if (m_reopen.exchange(false))
        {
            // Den alten Strip erst dunkel schalten. Ein WS2812 ist ein Latch:
            // ohne neue Daten haelt er seine letzte Farbe unbegrenzt. Wer den
            // Pin umlegt, liesse sonst eine LED auf ihrer letzten Farbe
            // einfrieren, bis das Board stromlos wird.
            write(0, 0, 0);
            openStrip(m_pin);
            phase = 0;
        }

        const Mode mode = m_mode.load();

        switch (mode)
        {
            case Mode::Idle:
            case Mode::NoWifi:
            {
                // Dreieck statt Sinus — spart die Mathematik, und nach der
                // quadratischen Kennlinie sieht man den Unterschied nicht.
                const uint32_t half  = BREATHE_MS / 2;
                const uint32_t pos   = phase % BREATHE_MS;
                const uint32_t up    = (pos < half) ? pos : (BREATHE_MS - pos);
                const uint8_t  level = static_cast<uint8_t>((up * 255) / half);

                if (mode == Mode::NoWifi)
                    write(scale(level), 0, 0);
                else
                    write(0, scale(level), 0);
                break;
            }

            case Mode::BatteryLow:
            {
                const bool on = ((phase / BLINK_SLOW_MS) % 2) == 0;
                write(on ? scale(255) : 0, 0, 0);
                break;
            }

            case Mode::TargetReady:
                write(0, scale(255), 0);
                break;

            case Mode::Busy:
                write(scale(255), (scale(255) * YELLOW_GREEN_PERCENT) / 100, 0);
                break;

            case Mode::Flashing:
            {
                const bool on = ((phase / BLINK_MS) % 2) == 0;
                if (on)
                    write(scale(255), (scale(255) * ORANGE_GREEN_PERCENT) / 100, 0);
                else
                    write(0, 0, 0);
                break;
            }
        }

        phase += TICK_MS;
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }

    write(0, 0, 0);
    closeStrip();
    m_task = nullptr;
    vTaskDelete(nullptr);
}
