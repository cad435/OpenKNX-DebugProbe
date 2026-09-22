#include "BatteryMonitor.hpp"

#include <algorithm>
#include <array>

#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace {
constexpr const char* TAG = "battery";

/// Messabstand. Eine Zelle aendert sich langsam, und jede Messung weckt den
/// ADC — haeufiger waere nur Stromverbrauch ohne Erkenntnis.
constexpr uint32_t SAMPLE_PERIOD_MS = 2000;

/**
 * So lange nach dem Start gilt keine Messung.
 *
 * `C9` (10 uF) laedt sich ueber die 500 kOhm des Teilers auf, tau = 5 s. Nach
 * 4 tau fehlen noch knapp 2 %, das reicht. Der Keramik-C hat unter DC-Bias
 * deutlich weniger Kapazitaet als aufgedruckt, die echte Einschwingzeit ist
 * also eher kuerzer — 25 s sind die sichere Seite.
 */
constexpr uint32_t WARMUP_MS = 25000;

/// Oversampling je Messung. Der Teiler ist hochohmig, aber `C9` glaettet
/// bereits alles Schnelle; das hier faengt nur das ADC-Rauschen ein.
constexpr int OVERSAMPLE = 16;

/// Darunter haengt keine Zelle am Messpunkt: Schalter aus, oder Probe am USB.
/// Eine Li-Ion-Zelle unter 2,0 V hat der Schutz-IC laengst abgeworfen.
constexpr uint16_t ABSENT_MV = 2000;

constexpr uint16_t LOW_MV      = 3550;
constexpr uint16_t CRITICAL_MV = 3300;

/// Rueckweg nach oben. Ohne das flackert die rote LED an der Schwelle, weil
/// ein verbundener Client die Klemmenspannung um einige zehn mV drueckt.
constexpr uint16_t HYSTERESIS_MV = 60;

/**
 * Leerlaufkennlinie einer Li-Ion-/LiPo-Zelle, Stuetzstellen mV -> Prozent.
 *
 * Absteigend sortiert, linear interpoliert. Mehr braucht es nicht: die Probe
 * zieht 30-120 mA, bei einer Zelle dieser Groesse weit unter 0,1 C. Der
 * Spannungsabfall am Innenwiderstand ist damit klein gegen die Streuung der
 * Kennlinie selbst — eine Lastkompensation waere Scheingenauigkeit.
 *
 * Die Null liegt bei 3,30 V und nicht bei den ueblichen 3,00 V: der LDO des
 * SuperMini gibt zwischen 3,0 und 3,5 V auf (am Geraet gemessen, siehe
 * CLAUDE.md). Was darunter in der Zelle steckt, kann diese Probe nicht mehr
 * nutzen, also zeigen wir es auch nicht als Vorrat an.
 *
 * Der Wert ist absichtlich derselbe wie CRITICAL_MV: "0 %" und "critical"
 * fallen damit exakt zusammen. Sonst gaebe es einen Bereich, in dem die
 * Anzeige 0 % sagt, der Zustand aber noch "low" ist — zwei Aussagen, die
 * sich widersprechen, ohne dass eine davon falsch waere.
 */
struct CurvePoint
{
    uint16_t mv;
    uint8_t  percent;
};

constexpr std::array<CurvePoint, 13> CURVE = {{
    {4200, 100}, {4100, 90}, {4000, 80}, {3930, 70}, {3870, 60},
    {3820, 50},  {3790, 40}, {3770, 30}, {3740, 20}, {3680, 15},
    {3600, 10},  {3500, 5},  {3300, 0},
}};
}  // namespace

BatteryMonitor::~BatteryMonitor()
{
    if (m_task != nullptr)
    {
        m_stop.store(true);
        // Der Task raeumt selbst ab und loescht sich; ein vTaskDelete von
        // aussen koennte ihn mitten im ADC-Zugriff erwischen.
        while (m_task != nullptr) vTaskDelay(pdMS_TO_TICKS(10));
    }
    closeAdc();
}

// ---------------------------------------------------------------------------
// Aufbau
// ---------------------------------------------------------------------------

esp_err_t BatteryMonitor::begin(Settings& settings)
{
    if (m_task != nullptr) return ESP_ERR_INVALID_STATE;

    m_settings = &settings;

    const uint32_t stored = settings.getU32(KEY_TRIM, 1000);
    if (stored >= 800 && stored <= 1250)
    {
        m_trim.store(static_cast<uint16_t>(stored));
    }
    else
    {
        ESP_LOGW(TAG, "Abgleich %lu Promille in NVS ist unplausibel, ignoriert",
                 static_cast<unsigned long>(stored));
    }

    const esp_err_t err = openAdc();
    if (err != ESP_OK) return err;

    if (xTaskCreatePinnedToCore(&BatteryMonitor::task, "battery", 3072, this, 2, &m_task, 0) !=
        pdPASS)
    {
        closeAdc();
        m_task = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Zellenmessung auf GPIO%d, Teiler 1:%lu, Abgleich %u Promille",
             MEASURE_GPIO, static_cast<unsigned long>(DIVIDER_FACTOR), m_trim.load());
    return ESP_OK;
}

esp_err_t BatteryMonitor::openAdc()
{
    // Pin -> Einheit und Kanal ausrechnen lassen, statt die Zuordnung
    // hinzuschreiben. Landet der Teiler beim naechsten Layout auf einem
    // anderen Pin, faellt ein Griff nach ADC2 hier auf und nicht erst bei den
    // ESP_ERR_TIMEOUT der ersten Messung mit eingeschaltetem WLAN.
    adc_unit_t unit = ADC_UNIT_1;
    esp_err_t  err  = adc_oneshot_io_to_channel(MEASURE_GPIO, &unit, &m_channel);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "GPIO%d ist kein ADC-Pin: %s", MEASURE_GPIO, esp_err_to_name(err));
        return err;
    }
    if (unit != ADC_UNIT_1)
    {
        ESP_LOGE(TAG, "GPIO%d liegt auf ADC2 - mit laufendem WLAN unbenutzbar", MEASURE_GPIO);
        return ESP_ERR_NOT_SUPPORTED;
    }

    adc_oneshot_unit_init_cfg_t unitCfg = {};
    unitCfg.unit_id                     = ADC_UNIT_1;
    unitCfg.ulp_mode                    = ADC_ULP_MODE_DISABLE;

    err = adc_oneshot_new_unit(&unitCfg, &m_adc);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return err;
    }

    // 12 dB deckt 0..rund 3,1 V ab. Bei voller Zelle stehen 2,1 V am Pin,
    // bei der Abschaltschwelle 1,7 V — beides bequem im Fenster.
    adc_oneshot_chan_cfg_t chanCfg = {};
    chanCfg.atten                  = ADC_ATTEN_DB_12;
    chanCfg.bitwidth               = ADC_BITWIDTH_DEFAULT;

    err = adc_oneshot_config_channel(m_adc, m_channel, &chanCfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_config_channel: %s", esp_err_to_name(err));
        closeAdc();
        return err;
    }

    // Ohne Kalibrierung waere der Rohwert um zweistellige Prozent daneben —
    // die Werkskennlinie im eFuse ist der ganze Unterschied zwischen einer
    // Tankuhr und einer Zufallszahl. Fehlt sie, messen wir trotzdem weiter
    // und sagen es im Log.
    adc_cali_curve_fitting_config_t caliCfg = {};
    caliCfg.unit_id                         = ADC_UNIT_1;
    caliCfg.chan                            = m_channel;
    caliCfg.atten                           = ADC_ATTEN_DB_12;
    caliCfg.bitwidth                        = ADC_BITWIDTH_DEFAULT;

    const esp_err_t caliErr = adc_cali_create_scheme_curve_fitting(&caliCfg, &m_cali);
    if (caliErr != ESP_OK)
    {
        m_cali = nullptr;
        ESP_LOGW(TAG, "keine ADC-Kalibrierung (%s), Werte sind grob",
                 esp_err_to_name(caliErr));
    }

    return ESP_OK;
}

void BatteryMonitor::closeAdc()
{
    if (m_cali != nullptr)
    {
        adc_cali_delete_scheme_curve_fitting(m_cali);
        m_cali = nullptr;
    }
    if (m_adc != nullptr)
    {
        adc_oneshot_del_unit(m_adc);
        m_adc = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Messung
// ---------------------------------------------------------------------------

bool BatteryMonitor::readPinMv(uint16_t& outMv)
{
    if (m_adc == nullptr) return false;

    uint32_t sum   = 0;
    int      taken = 0;

    for (int i = 0; i < OVERSAMPLE; ++i)
    {
        int raw = 0;
        if (adc_oneshot_read(m_adc, m_channel, &raw) != ESP_OK) continue;

        int mv = 0;
        if (m_cali != nullptr)
        {
            if (adc_cali_raw_to_voltage(m_cali, raw, &mv) != ESP_OK) continue;
        }
        else
        {
            // Notnagel ohne eFuse-Kennlinie: 12 Bit auf die nominellen 3,1 V
            // der 12-dB-Daempfung. Gut genug, um "Zelle da" von "keine Zelle"
            // zu unterscheiden, mehr nicht.
            mv = static_cast<int>((static_cast<uint32_t>(raw) * 3100) / 4095);
        }

        sum += static_cast<uint32_t>(mv);
        ++taken;
    }

    if (taken == 0) return false;

    outMv = static_cast<uint16_t>(sum / static_cast<uint32_t>(taken));
    return true;
}

BatteryMonitor::Level BatteryMonitor::classify(uint16_t cellMv) const
{
    if (cellMv < ABSENT_MV) return Level::Absent;

    const Level current = m_level.load();

    // Nach unten sofort, nach oben erst mit Abstand. Die Warnung soll
    // stehenbleiben, solange sie berechtigt ist.
    if (cellMv < CRITICAL_MV) return Level::Critical;
    if (current == Level::Critical && cellMv < CRITICAL_MV + HYSTERESIS_MV) return Level::Critical;

    if (cellMv < LOW_MV) return Level::Low;
    if ((current == Level::Low || current == Level::Critical) &&
        cellMv < LOW_MV + HYSTERESIS_MV)
    {
        return Level::Low;
    }

    return Level::Normal;
}

void BatteryMonitor::task(void* arg)
{
    static_cast<BatteryMonitor*>(arg)->run();
}

void BatteryMonitor::run()
{
    const int64_t startedUs  = esp_timer_get_time();
    Level         lastLogged = Level::Unknown;

    while (!m_stop.load())
    {
        uint16_t pinMv = 0;
        if (readPinMv(pinMv))
        {
            m_pinMv.store(pinMv);

            const uint32_t cell =
                (static_cast<uint32_t>(pinMv) * DIVIDER_FACTOR * m_trim.load()) / 1000;
            const uint16_t cellMv = static_cast<uint16_t>(std::min<uint32_t>(cell, 65535));
            m_cellMv.store(cellMv);

            const bool warm =
                (esp_timer_get_time() - startedUs) >= static_cast<int64_t>(WARMUP_MS) * 1000;
            m_warm.store(warm);

            if (warm)
            {
                const Level next = classify(cellMv);
                m_level.store(next);

                if (next != lastLogged)
                {
                    ESP_LOGI(TAG, "%s - Zelle %u mV (Pin %u mV)", levelName(next), cellMv, pinMv);
                    lastLogged = next;
                }
            }
            else
            {
                // Waehrend `C9` laedt, ist jede Aussage falsch — und eine
                // falsche Leerwarnung waere die schaedlichste davon.
                m_level.store(Level::Unknown);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }

    m_task = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Abfragen
// ---------------------------------------------------------------------------

uint8_t BatteryMonitor::percent() const
{
    if (!present()) return 0;

    const uint16_t mv = m_cellMv.load();
    if (mv >= CURVE.front().mv) return CURVE.front().percent;
    if (mv <= CURVE.back().mv) return CURVE.back().percent;

    for (size_t i = 1; i < CURVE.size(); ++i)
    {
        const CurvePoint& hi = CURVE[i - 1];
        const CurvePoint& lo = CURVE[i];
        if (mv > lo.mv)
        {
            const uint32_t span = static_cast<uint32_t>(hi.mv - lo.mv);
            const uint32_t into = static_cast<uint32_t>(mv - lo.mv);
            const uint32_t rise = static_cast<uint32_t>(hi.percent - lo.percent);
            return static_cast<uint8_t>(lo.percent + (into * rise + span / 2) / span);
        }
    }
    return 0;
}

esp_err_t BatteryMonitor::calibrateTo(uint16_t actualMv)
{
    if (m_settings == nullptr) return ESP_ERR_INVALID_STATE;
    if (!warmedUp() || !present()) return ESP_ERR_INVALID_STATE;
    if (actualMv < ABSENT_MV || actualMv > 4400) return ESP_ERR_INVALID_ARG;

    // Der bisherige Abgleich steckt in m_cellMv schon drin — also aus der
    // ungetrimmten Rohspannung rechnen, sonst multipliziert sich jeder
    // weitere Abgleich auf den vorigen auf.
    const uint32_t rawCell = static_cast<uint32_t>(m_pinMv.load()) * DIVIDER_FACTOR;
    if (rawCell == 0) return ESP_ERR_INVALID_STATE;

    const uint32_t trim = (static_cast<uint32_t>(actualMv) * 1000 + rawCell / 2) / rawCell;
    if (trim < 800 || trim > 1250) return ESP_ERR_INVALID_ARG;

    m_trim.store(static_cast<uint16_t>(trim));
    const esp_err_t err = m_settings->setU32(KEY_TRIM, trim);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Abgleich auf %u mV gesetzt, Faktor %lu Promille", actualMv,
             static_cast<unsigned long>(trim));
    return ESP_OK;
}

const char* BatteryMonitor::levelName(Level level)
{
    switch (level)
    {
        case Level::Unknown:  return "unknown";
        case Level::Absent:   return "absent";
        case Level::Normal:   return "normal";
        case Level::Low:      return "low";
        case Level::Critical: return "critical";
    }
    return "unknown";
}
