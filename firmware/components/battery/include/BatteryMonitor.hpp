#pragma once

#include <atomic>

#include "Settings.hpp"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Die Zellenspannung ueber den Teiler auf dem Dongle-PCB.
 *
 * Hardware (V0.1): `R5`/`R6` 1 M / 1 M vom **geschalteten** B+ nach Masse,
 * Abgriff `BatMon` mit `C9` 10 uF auf **GPIO5** = ADC1_CH4. Der Teiler haengt
 * hinter dem Akkuschalter und hinter dem Schutz-IC — steht der Schalter auf
 * Aus, liegt der Abgriff auf Masse und die Messung meldet `Absent`. Genau so
 * sieht auch der reine USB-Betrieb ohne Zelle aus; beides ist dasselbe und
 * wird nicht unterschieden.
 *
 * Zwei Eigenheiten, die aus der Beschaltung folgen und den Code praegen:
 *
 * 1. **Einschwingen.** Die Thevenin-Impedanz des Teilers ist 500 kOhm, mit
 *    `C9` ergibt das tau = 5 s. Nach dem Einschalten laeuft der Abgriff von
 *    Null hoch und liest rund 20 s lang zu niedrig. Ohne Aufwaermphase meldete
 *    die Probe bei jedem Start "Zelle fast leer". Siehe WARMUP_MS.
 * 2. **ADC1 ist Pflicht.** ADC2 belegt der WLAN-Treiber, Lesezugriffe liefern
 *    dort `ESP_ERR_TIMEOUT`. begin() rechnet den Pin selbst in Einheit und
 *    Kanal um und bricht ab, wenn er nicht auf ADC1 liegt — damit faellt ein
 *    kuenftiger Pinwechsel sofort auf, statt still nichts zu liefern.
 *
 * Der Ladezustand in Prozent ist eine Schaetzung aus der Leerlaufkennlinie
 * einer Li-Ion-Zelle. Unter Last (30-120 mA) sackt die Klemmenspannung ab, die
 * Anzeige springt also beim Verbinden eines Clients nach unten. Als Tankuhr
 * taugt das, als Messgeraet nicht.
 */
class BatteryMonitor
{
public:
    enum class Level
    {
        Unknown,   ///< noch keine gueltige Messung (ADC aus, oder Aufwaermphase)
        Absent,    ///< keine Zelle bzw. Schalter aus — Probe haengt am USB
        Normal,    ///< Zelle da, Spannung in Ordnung
        Low,       ///< unter LOW_MV — Zeit zum Laden
        Critical,  ///< unter CRITICAL_MV — Abschaltung steht bevor
    };

    BatteryMonitor() = default;
    ~BatteryMonitor();

    BatteryMonitor(const BatteryMonitor&)            = delete;
    BatteryMonitor& operator=(const BatteryMonitor&) = delete;

    /**
     * Baut ADC und Kalibrierung auf und startet den Messtask.
     *
     * @param settings Quelle des Feinabgleichs (`bat_trim`, siehe trim()).
     *                 Wird auch zum Speichern in calibrateTo() gebraucht,
     *                 muss also den Monitor ueberleben.
     */
    esp_err_t begin(Settings& settings);

    /// true, wenn der ADC steht. Sagt nichts darueber, ob eine Zelle dranhaengt.
    bool available() const { return m_task != nullptr; }

    Level    level() const { return m_level.load(); }
    bool     present() const { return level() >= Level::Normal; }

    /// Zellenspannung in mV, 0 solange nichts Gueltiges vorliegt.
    uint16_t millivolts() const { return m_cellMv.load(); }

    /// Spannung am Pin in mV — fuer die Fehlersuche am Teiler selbst.
    uint16_t pinMillivolts() const { return m_pinMv.load(); }

    /// Grobe Tankuhr 0..100, 0 wenn keine Zelle da ist.
    uint8_t percent() const;

    /// false, solange `C9` noch laedt und die Messung zu niedrig liest.
    bool warmedUp() const { return m_warm.load(); }

    /**
     * Feinabgleich gegen ein Multimeter: @p actualMv ist die an der Zelle
     * gemessene Spannung. Der Faktor landet in NVS und gilt ab sofort.
     *
     * Nur sinnvoll mit eingeschwungener Messung und gesteckter Zelle; sonst
     * `ESP_ERR_INVALID_STATE`. Abwegige Werte (Faktor ausserhalb 0,8..1,25)
     * werden abgelehnt — das faengt den Zahlendreher ab, der sonst dauerhaft
     * in NVS steht.
     */
    esp_err_t calibrateTo(uint16_t actualMv);

    /// Abgleichfaktor in Promille, 1000 = unveraendert.
    uint16_t trim() const { return m_trim.load(); }

    static const char* levelName(Level level);

    /// NVS-Schluessel des Feinabgleichs.
    static constexpr const char* KEY_TRIM = "bat_trim";

    /// Messpin. Aendert er sich, muss er auf ADC1 bleiben (GPIO1..GPIO10).
    static constexpr int MEASURE_GPIO = 5;

    /// `R5`/`R6` sind gleich gross, der Abgriff traegt also die halbe Zelle.
    static constexpr uint32_t DIVIDER_FACTOR = 2;

private:
    static void task(void* arg);
    void        run();
    esp_err_t   openAdc();
    void        closeAdc();

    /// Eine Messung: oversampled, entprellt, ohne Filter.
    bool readPinMv(uint16_t& outMv);

    /// Ordnet eine Zellenspannung einem Level zu — mit Hysterese, damit die
    /// LED an der Schwelle nicht flattert.
    Level classify(uint16_t cellMv) const;

    Settings*    m_settings {nullptr};
    TaskHandle_t m_task {nullptr};

    adc_oneshot_unit_handle_t m_adc {nullptr};
    adc_cali_handle_t         m_cali {nullptr};
    adc_channel_t             m_channel {};

    std::atomic<uint16_t> m_pinMv {0};
    std::atomic<uint16_t> m_cellMv {0};
    std::atomic<uint16_t> m_trim {1000};
    std::atomic<Level>    m_level {Level::Unknown};
    std::atomic<bool>     m_warm {false};
    std::atomic<bool>     m_stop {false};
};
