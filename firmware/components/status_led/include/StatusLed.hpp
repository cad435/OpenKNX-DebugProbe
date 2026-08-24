#pragma once

#include <atomic>

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Die RGB-LED des Moduls als Betriebsanzeige.
 *
 * Ein einzelner WS2812 an einer Datenleitung. Welcher Pin das ist,
 * unterscheidet sich zwischen den SuperMini-Varianten und steht in keinem
 * Datenblatt, das wir haben — deshalb ist er zur Laufzeit setzbar und wird in
 * NVS gemerkt, statt als Konstante einkompiliert zu sein.
 *
 * Die Zustaende sind nach Dringlichkeit geordnet: `Flashing` gewinnt immer,
 * weil dort ein Abziehen des Kabels echten Schaden anrichtet.
 */
class StatusLed
{
public:
    /**
     * Nach Dringlichkeit geordnet — die Aufsichtsschleife nimmt den ersten
     * zutreffenden von oben.
     */
    enum class Mode
    {
        Flashing,     ///< orange blinkend   — jetzt nicht abstecken
        BatteryLow,   ///< rot, 1 s Takt     — Zelle fast leer
        NoWifi,       ///< rot pulsierend    — kein WLAN, Probe unerreichbar
        Busy,         ///< gelb              — Client verbunden, Funk wach
        TargetReady,  ///< gruen dauerhaft   — Ziel erkannt
        Idle,         ///< gruen pulsierend  — bereit, kein Ziel angesteckt
    };

    StatusLed() = default;
    ~StatusLed();

    StatusLed(const StatusLed&)            = delete;
    StatusLed& operator=(const StatusLed&) = delete;

    /**
     * Startet die Anzeige auf @p pin.
     *
     * @param brightness Obergrenze je Farbkanal (0..255). Ein WS2812 auf
     *                   Vollausschlag ist als Statusanzeige unbrauchbar hell
     *                   und zieht bei Weiss rund 60 mA — im Akkubetrieb
     *                   relevant. 48 entspricht etwa 5 mA fuer eine Farbe.
     */
    esp_err_t begin(gpio_num_t pin, uint8_t brightness = 48);

    /// Wechselt den Pin im Betrieb (Suche nach dem richtigen) und baut neu auf.
    esp_err_t setPin(gpio_num_t pin);

    void       setMode(Mode mode) { m_mode.store(mode); }
    Mode       mode() const { return m_mode.load(); }
    gpio_num_t pin() const { return m_pin; }
    bool       isRunning() const { return m_task != nullptr; }

    static const char* modeName(Mode mode);

private:
    static void task(void* arg);
    void        run();
    esp_err_t   openStrip(gpio_num_t pin);
    void        closeStrip();
    void        write(uint8_t r, uint8_t g, uint8_t b);

    /// Skaliert 0..255 auf die Helligkeitsobergrenze und krummt die Kennlinie:
    /// die Wahrnehmung ist logarithmisch, ein linearer Verlauf sieht am oberen
    /// Ende wie Stillstand aus.
    uint8_t scale(uint8_t value) const;

    void*             m_strip {nullptr};  ///< led_strip_handle_t, hier opak
    TaskHandle_t      m_task {nullptr};
    gpio_num_t        m_pin {GPIO_NUM_NC};
    uint8_t           m_brightness {48};
    std::atomic<Mode> m_mode {Mode::Idle};
    std::atomic<bool> m_reopen {false};
    std::atomic<bool> m_stop {false};
};
