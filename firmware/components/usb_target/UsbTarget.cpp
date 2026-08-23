#include "UsbTarget.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "usb/usb_helpers.h"
#include "usb/vcp.hpp"
#include "usb/vcp_ch34x.hpp"
#include "usb/vcp_cp210x.hpp"
#include "usb/vcp_ftdi.hpp"

namespace {

constexpr const char* TAG = "usb";

constexpr uint8_t USB_CLASS_CDC_      = 0x02;
constexpr uint8_t USB_CLASS_HID_      = 0x03;
constexpr uint8_t USB_CLASS_MSC_      = 0x08;
constexpr uint8_t USB_CLASS_HUB_      = 0x09;
constexpr uint8_t USB_CLASS_CDC_DATA_ = 0x0A;
constexpr uint8_t USB_CLASS_MISC_     = 0xEF;
constexpr uint8_t USB_CLASS_VENDOR_   = 0xFF;

constexpr uint16_t ANY_PID = 0xFFFF;

/// VID von Raspberry Pi. Nur diese Geraete kennen den 1200-Baud-Touch.
constexpr uint16_t VID_RASPBERRY = 0x2E8A;

/// Wie lange DTR beim Touch aktiv bleibt, bevor es wieder abfaellt.
constexpr uint32_t TOUCH_HOLD_MS = 50;

/// Wartezeit auf den Massenspeicher nach einem Touch (docs/architecture.md).
constexpr uint32_t MSC_TIMEOUT_MS = 5000;

/// Ein Versuch und ein Retry — mehr bringt nichts, wenn die Firmware den
/// Touch gar nicht auswertet.
constexpr uint32_t TOUCH_ATTEMPTS = 2;

struct KnownDevice
{
    uint16_t             vid;
    uint16_t             pid;  ///< ANY_PID = beliebig, muss dann zuletzt stehen
    const char*          name;
    UsbTarget::Kind      kind;
    UsbTarget::Driver    driver;
};

/**
 * Reihenfolge zählt: der erste Treffer gewinnt, deshalb stehen die konkreten
 * PIDs vor dem ANY_PID-Eintrag desselben Herstellers.
 */
constexpr KnownDevice KNOWN_DEVICES[] = {
    // --- Raspberry Pi / RP2040 ---------------------------------------------
    {0x2E8A, 0x0003, "RP2040 im BOOTSEL-Modus",       UsbTarget::Kind::Bootsel,      UsbTarget::Driver::Msc},
    {0x2E8A, 0x000A, "Raspberry Pi Pico (USB-CDC)",   UsbTarget::Kind::Cdc,          UsbTarget::Driver::CdcAcm},
    {0x2E8A, 0x0009, "RP2040 (Pico SDK, CDC)",        UsbTarget::Kind::Cdc,          UsbTarget::Driver::CdcAcm},
    {0x2E8A, 0x00C0, "Raspberry Pi Pico (Arduino)",   UsbTarget::Kind::Cdc,          UsbTarget::Driver::CdcAcm},
    {0x2E8A, ANY_PID, "Raspberry Pi RP2040",          UsbTarget::Kind::Cdc,          UsbTarget::Driver::CdcAcm},

    // --- Espressif (für die künftigen netzwerkfähigen OpenKNX-Geräte) ------
    {0x303A, 0x1001, "ESP32 USB-Serial/JTAG",         UsbTarget::Kind::Cdc,          UsbTarget::Driver::CdcAcm},
    {0x303A, 0x0002, "ESP32-S2 USB-CDC",              UsbTarget::Kind::Cdc,          UsbTarget::Driver::CdcAcm},
    {0x303A, 0x1000, "ESP32-S2 (TinyUSB)",            UsbTarget::Kind::Cdc,          UsbTarget::Driver::CdcAcm},
    {0x303A, ANY_PID, "Espressif ESP32 (USB)",        UsbTarget::Kind::Cdc,          UsbTarget::Driver::CdcAcm},

    // --- USB-UART-Bridges: vendor-specific, NICHT CDC ----------------------
    {0x1A86, 0x7523, "USB-UART-Bridge CH340",         UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Ch34x},
    {0x1A86, 0x5523, "USB-UART-Bridge CH341",         UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Ch34x},
    {0x1A86, 0x55D3, "USB-UART-Bridge CH343",         UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Ch34x},
    {0x1A86, 0x55D4, "USB-UART-Bridge CH9102",        UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Ch34x},
    {0x1A86, ANY_PID, "USB-UART-Bridge (WCH)",        UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Ch34x},

    {0x10C4, 0xEA60, "USB-UART-Bridge CP2102",        UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Cp210x},
    {0x10C4, 0xEA70, "USB-UART-Bridge CP2105",        UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Cp210x},
    {0x10C4, 0xEA71, "USB-UART-Bridge CP2108",        UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Cp210x},
    {0x10C4, ANY_PID, "USB-UART-Bridge (Silicon Labs)", UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Cp210x},

    {0x0403, 0x6001, "USB-UART-Bridge FT232R",        UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Ftdi},
    {0x0403, 0x6010, "USB-UART-Bridge FT2232",        UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Ftdi},
    {0x0403, 0x6014, "USB-UART-Bridge FT232H",        UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Ftdi},
    {0x0403, 0x6015, "USB-UART-Bridge FT230X",        UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Ftdi},
    {0x0403, ANY_PID, "USB-UART-Bridge (FTDI)",       UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Ftdi},

    {0x067B, 0x2303, "USB-UART-Bridge PL2303",        UsbTarget::Kind::SerialBridge, UsbTarget::Driver::Pl2303},

    // --- sonstige, die im OpenKNX-Umfeld auftauchen können -----------------
    {0x0483, 0x5740, "STM32 Virtual COM Port",        UsbTarget::Kind::Cdc,          UsbTarget::Driver::CdcAcm},
};

/// USB-Stringdescriptoren sind UTF-16LE; für die Anzeige reicht ASCII.
std::string toAscii(const usb_str_desc_t* desc)
{
    if (desc == nullptr || desc->bLength < 2) return {};

    const size_t chars = static_cast<size_t>(desc->bLength - 2) / 2;
    std::string  out;
    out.reserve(chars);

    for (size_t i = 0; i < chars; ++i)
    {
        const uint16_t c = desc->wData[i];
        out.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
    }
    return out;
}

const char* speedName(usb_speed_t speed)
{
    switch (speed)
    {
        case USB_SPEED_LOW:  return "low";
        case USB_SPEED_FULL: return "full";
        default:             return "unknown";
    }
}

}  // namespace

UsbTarget::~UsbTarget()
{
    if (m_mutex != nullptr) vSemaphoreDelete(m_mutex);
}

// ---------------------------------------------------------------------------
// Namen
// ---------------------------------------------------------------------------

const char* UsbTarget::kindName(Kind kind)
{
    switch (kind)
    {
        case Kind::Cdc:          return "cdc";
        case Kind::SerialBridge: return "serial_bridge";
        case Kind::Bootsel:      return "bootsel";
        case Kind::Msc:          return "msc";
        case Kind::Hid:          return "hid";
        case Kind::Hub:          return "hub";
        case Kind::Unknown:      break;
    }
    return "unknown";
}

const char* UsbTarget::driverName(Driver driver)
{
    switch (driver)
    {
        case Driver::CdcAcm:      return "cdc_acm";
        case Driver::Msc:         return "msc";
        case Driver::Ch34x:       return "ch34x";
        case Driver::Cp210x:      return "cp210x";
        case Driver::Ftdi:        return "ftdi";
        case Driver::Pl2303:      return "pl2303";
        case Driver::Unsupported: return "unsupported";
        case Driver::None:        break;
    }
    return "none";
}

const char* UsbTarget::stateName() const
{
    switch (state())
    {
        case State::NotStarted: return "not_started";
        case State::NoTarget:   return "no_target";
        case State::Connected:  return "connected";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Zustand
// ---------------------------------------------------------------------------

UsbTarget::State UsbTarget::state() const
{
    if (m_mutex == nullptr) return m_state;
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    const State s = m_state;
    xSemaphoreGive(m_mutex);
    return s;
}

UsbTarget::DeviceInfo UsbTarget::device() const
{
    if (m_mutex == nullptr) return m_info;
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    const DeviceInfo info = m_info;
    xSemaphoreGive(m_mutex);
    return info;
}

uint32_t UsbTarget::secondsInState() const
{
    if (m_mutex == nullptr) return 0;
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    const int64_t since = m_stateSinceUs;
    xSemaphoreGive(m_mutex);
    if (since == 0) return 0;
    return static_cast<uint32_t>((esp_timer_get_time() - since) / 1000000);
}

void UsbTarget::setState(State state)
{
    xSemaphoreTake(m_mutex, portMAX_DELAY);
    m_state        = state;
    m_stateSinceUs = esp_timer_get_time();
    xSemaphoreGive(m_mutex);
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

esp_err_t UsbTarget::begin()
{
    m_mutex = xSemaphoreCreateMutex();
    if (m_mutex == nullptr) return ESP_ERR_NO_MEM;

    usb_host_config_t hostConfig = {};
    hostConfig.skip_phy_setup    = false;
    hostConfig.intr_flags        = ESP_INTR_FLAG_LEVEL1;

    esp_err_t err = usb_host_install(&hostConfig);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    // Beide Tasks auf Core 1, damit sie sich nicht mit WiFi/lwIP auf Core 0
    // um die CPU streiten — das war der Hauptgrund fuer den S3 statt eines S2.
    if (xTaskCreatePinnedToCore(&UsbTarget::daemonTask, "usb_daemon", 4096, this, 10,
                                &m_daemonTask, 1) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(&UsbTarget::clientTask, "usb_client", 5120, this, 9,
                                &m_clientTask, 1) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    // CDC-ACM-Treiber und die VCP-Treiber für die UART-Bridges. Letztere sind
    // Pflicht: die OpenKNX-Geräte mit klassischem ESP32 haben keine native
    // USB-Peripherie und damit einen Bridge-Chip auf dem Board.
    cdc_acm_host_driver_config_t cdcConfig = {};
    cdcConfig.driver_task_stack_size       = 4096;
    cdcConfig.driver_task_priority         = 11;
    cdcConfig.xCoreID                      = 1;
    cdcConfig.new_dev_cb                   = nullptr;

    err = cdc_acm_host_install(&cdcConfig);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "cdc_acm_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_usb::VCP::register_driver<esp_usb::FT23x>();
    esp_usb::VCP::register_driver<esp_usb::CP210x>();
    esp_usb::VCP::register_driver<esp_usb::CH34x>();

    // Massenspeicher für den RP2040 im BOOTSEL-Modus.
    msc_host_driver_config_t mscConfig = {};
    mscConfig.create_backround_task    = true;
    mscConfig.task_priority            = 11;
    mscConfig.stack_size               = 4096;
    mscConfig.core_id                  = 1;
    mscConfig.callback                 = &UsbTarget::mscEventCb;
    mscConfig.callback_arg             = this;

    err = msc_host_install(&mscConfig);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "msc_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreatePinnedToCore(&UsbTarget::serialTask, "usb_serial", 4096, this, 8,
                                &m_serialTask, 1) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    setState(State::NoTarget);
    ESP_LOGI(TAG, "USB host up, waiting for target");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Serielle Sitzung
// ---------------------------------------------------------------------------

void UsbTarget::setRxCallback(RxCallback cb, void* ctx)
{
    m_rxContext  = ctx;
    m_rxCallback = cb;
}

bool UsbTarget::isSerialOpen() const
{
    return m_serial != nullptr;
}

bool UsbTarget::serialDataCb(const uint8_t* data, size_t length, void* arg)
{
    auto* self = static_cast<UsbTarget*>(arg);
    if (self->m_rxCallback != nullptr)
    {
        self->m_rxCallback(data, length, self->m_rxContext);
    }
    return true;  // verarbeitet, RX-Puffer darf geleert werden
}

void UsbTarget::serialEventCb(const cdc_acm_host_dev_event_data_t* event, void* arg)
{
    auto* self = static_cast<UsbTarget*>(arg);

    switch (event->type)
    {
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGW(TAG, "serial session closed (device gone)");
            self->closeSerial();
            break;
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "serial error %d", event->data.error);
            break;
        default:
            break;
    }
}

void UsbTarget::serialTask(void* arg)
{
    auto* self = static_cast<UsbTarget*>(arg);

    while (true)
    {
        // Warten, bis ein Gerät gemeldet wurde. Das Öffnen darf nicht im
        // Callback passieren: cdc_acm_host_open() und msc_host_install_device()
        // blockieren beide.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (self->m_mscPendingAddress != 0)
        {
            self->openMsc();
        }
        else
        {
            self->openSerial();
        }
    }
}

// ---------------------------------------------------------------------------
// Massenspeicher
// ---------------------------------------------------------------------------

void UsbTarget::mscEventCb(const msc_host_event_t* event, void* arg)
{
    auto* self = static_cast<UsbTarget*>(arg);

    // Der Event-Enum ist anonym in der Struktur deklariert; in C++ muessen die
    // Werte deshalb ueber den Strukturnamen qualifiziert werden.
    if (event->event == msc_host_event_t::MSC_DEVICE_CONNECTED)
    {
        self->m_mscPendingAddress = event->device.address;
        if (self->m_serialTask != nullptr) xTaskNotifyGive(self->m_serialTask);
    }
    else if (event->event == msc_host_event_t::MSC_DEVICE_DISCONNECTED)
    {
        self->closeMsc();
    }
}

void UsbTarget::openMsc()
{
    const uint8_t address   = m_mscPendingAddress;
    m_mscPendingAddress     = 0;
    if (address == 0 || m_msc != nullptr) return;

    msc_host_device_handle_t handle = nullptr;
    const esp_err_t err = msc_host_install_device(address, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
        return;
    }

    msc_host_device_info_t info = {};
    if (msc_host_get_device_info(handle, &info) == ESP_OK)
    {
        m_mscSectorSize  = info.sector_size;
        m_mscSectorCount = info.sector_count;
    }

    m_msc = handle;
    ESP_LOGI(TAG, "mass storage ready: %lu sectors x %lu bytes (%lu KB)",
             static_cast<unsigned long>(m_mscSectorCount),
             static_cast<unsigned long>(m_mscSectorSize),
             static_cast<unsigned long>((static_cast<uint64_t>(m_mscSectorCount) *
                                         m_mscSectorSize) / 1024));
}

void UsbTarget::closeMsc()
{
    msc_host_device_handle_t handle = m_msc;
    if (handle == nullptr) return;

    m_msc            = nullptr;
    m_mscSectorCount = 0;
    msc_host_uninstall_device(handle);
    ESP_LOGW(TAG, "mass storage gone");
}

// ---------------------------------------------------------------------------
// Weg nach BOOTSEL
// ---------------------------------------------------------------------------

esp_err_t UsbTarget::touch()
{
    if (m_serial == nullptr) return ESP_ERR_INVALID_STATE;

    /*
     * Die Baudrate wird bewusst NICHT in m_baudRate uebernommen: klappt der
     * Touch, kommt das Ziel nach dem Flashen als CDC zurueck, und dann soll
     * openSerial() die Konsole wieder mit CONSOLE_BAUD aufsetzen und nicht
     * mit 1200.
     */
    const uint32_t keep = m_baudRate;
    const esp_err_t err = setLineCoding(TOUCH_BAUD);
    m_baudRate          = keep;
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "line coding 1200 failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * DTR aktiv, kurz halten, dann fallen lassen — genau die Folge, die ein
     * Oeffnen und Schliessen des Ports am PC auf dem Bus erzeugt (das meint
     * PlatformIO mit "Forcing reset using 1200bps open/close"). Ein Teil der
     * Firmwares triggert auf die Baudrate allein, ein Teil erst auf das
     * Abfallen von DTR; diese Reihenfolge deckt beide ab.
     */
    setControlLines(true, true);
    vTaskDelay(pdMS_TO_TICKS(TOUCH_HOLD_MS));
    return setControlLines(false, false);
}

bool UsbTarget::waitForMsc(uint32_t timeoutMs)
{
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(timeoutMs) * 1000;

    while (!isMscReady() && esp_timer_get_time() < deadline)
    {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    return isMscReady();
}

esp_err_t UsbTarget::restoreConsole()
{
    if (m_serial == nullptr) return ESP_ERR_INVALID_STATE;

    const esp_err_t err = setLineCoding(CONSOLE_BAUD);
    resetControlLines();
    return err;
}

esp_err_t UsbTarget::enterBootsel(std::string& error)
{
    error.clear();

    // Schon dort — nichts zu tun. Das ist der Normalfall, wenn jemand die
    // Taster selbst gedrueckt hat.
    if (isMscReady()) return ESP_OK;

    if (state() != State::Connected)
    {
        error = "kein Zielgeraet angesteckt";
        return ESP_ERR_INVALID_STATE;
    }

    const DeviceInfo info = device();

    if (info.vid != VID_RASPBERRY)
    {
        char ids[16];
        snprintf(ids, sizeof(ids), "%04X:%04X", info.vid, info.pid);
        error = "Ziel " + std::string(ids) +
                " ist kein RP2040 - der 1200-Baud-Touch gilt nur fuer Raspberry-Pi-Geraete";
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (info.kind != Kind::Cdc)
    {
        error = "Ziel meldet sich als " + std::string(kindName(info.kind)) +
                ", nicht als CDC - ein 1200-Baud-Touch ist da nicht moeglich";
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (m_serial == nullptr)
    {
        error = "serielle Sitzung zum Ziel ist nicht offen, Touch nicht moeglich";
        return ESP_ERR_INVALID_STATE;
    }

    for (uint32_t attempt = 1; attempt <= TOUCH_ATTEMPTS; ++attempt)
    {
        ESP_LOGI(TAG, "1200 baud touch, attempt %lu/%lu",
                 static_cast<unsigned long>(attempt),
                 static_cast<unsigned long>(TOUCH_ATTEMPTS));

        const esp_err_t err = touch();

        // Ein Fehler beim Absetzen ist kein Beinbruch: reagiert das Ziel auf
        // die 1200 Baud, verschwindet es noch waehrend des Steuertransfers,
        // und dann quittiert der abgebrochene Transfer mit einem Fehler.
        if (err != ESP_OK && m_serial != nullptr && !isMscReady())
        {
            error = std::string("Touch konnte nicht abgesetzt werden: ") +
                    esp_err_to_name(err);
            return err;
        }

        if (waitForMsc(MSC_TIMEOUT_MS))
        {
            ESP_LOGI(TAG, "target is in BOOTSEL after attempt %lu",
                     static_cast<unsigned long>(attempt));
            return ESP_OK;
        }

        ESP_LOGW(TAG, "no mass storage within %lu ms",
                 static_cast<unsigned long>(MSC_TIMEOUT_MS));
    }

    // Gescheitert: das Ziel darf nicht mit 1200 Baud und abgefallenem DTR
    // zurueckbleiben, sonst ist hinterher auch die Konsole unbrauchbar.
    restoreConsole();

    error = "1200-Baud-Touch " + std::to_string(TOUCH_ATTEMPTS) + "x abgesetzt, aber das " +
            "Ziel hat sich nicht als Massenspeicher gemeldet (je " +
            std::to_string(MSC_TIMEOUT_MS / 1000) + " s gewartet). Wertet die Firmware " +
            "des Ziels den Touch nicht aus? Dann am Geraet BOOTSEL halten und RUN kurz " +
            "druecken.";
    return ESP_ERR_TIMEOUT;
}

esp_err_t UsbTarget::writeSector(uint32_t sector, const void* data, size_t size)
{
    msc_host_device_handle_t handle = m_msc;
    if (handle == nullptr) return ESP_ERR_INVALID_STATE;
    return msc_host_write_sector(handle, sector, data, size);
}

void UsbTarget::openSerial()
{
    if (m_serial != nullptr) return;

    const DeviceInfo info = device();
    if (info.kind != Kind::Cdc && info.kind != Kind::SerialBridge) return;

    cdc_acm_host_device_config_t config = {};
    config.connection_timeout_ms        = 5000;
    config.out_buffer_size              = 512;

    /*
     * 0 bedeutet: Groesse des IN-Endpunkts (MPS) verwenden. Das ist hier keine
     * Optimierung, sondern notwendig.
     *
     * Ein USB-IN-Transfer endet, wenn die angeforderte Laenge erreicht ist ODER
     * ein Paket kuerzer als MPS eintrifft. Bei einem grossen Wert (vorher 512)
     * sammelt der Treiber also weiter, solange nur volle Pakete kommen. Eine
     * Ausgabe, deren Laenge genau ein Vielfaches von MPS ist, bleibt dann im
     * offenen Transfer liegen, bis irgendwann mehr Daten nachkommen.
     *
     * Am Geraet gemessen: die CH340 liefert 32-Byte-Pakete. Eine 63-Byte-Zeile
     * (32+31) kam sofort durch, eine 64-Byte-Zeile (32+32) blieb haengen, bis
     * der naechste Tastendruck sie mit herausschob — die Konsole hing dadurch um
     * genau ein Ereignis nach. Direkt am PC angesteckt trat das nicht auf, das
     * Zielgeraet war also unschuldig.
     */
    config.in_buffer_size               = 0;
    config.event_cb                     = &UsbTarget::serialEventCb;
    config.data_cb                      = &UsbTarget::serialDataCb;
    config.user_arg                     = this;

    CdcAcmDevice* opened = nullptr;

    if (info.kind == Kind::Cdc)
    {
        auto* cdc = new CdcAcmDevice();
        const esp_err_t err = cdc->open(info.vid, info.pid, 0, &config);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "cdc_acm open failed: %s", esp_err_to_name(err));
            delete cdc;
            return;
        }
        opened = cdc;
    }
    else
    {
        // Die VCP-Klammer wählt CH34x / CP210x / FT23x anhand VID/PID aus.
        // Sie meldet Fehler über Exceptions, deshalb hier abgefangen.
        try
        {
            opened = esp_usb::VCP::open(info.vid, info.pid, &config);
        }
        catch (...)
        {
            opened = nullptr;
        }
        if (opened == nullptr)
        {
            ESP_LOGE(TAG, "VCP open failed for %04X:%04X", info.vid, info.pid);
            return;
        }
    }

    m_serial = opened;

    // Definierter Ausgangszustand. Bei echtem CDC ist die Baudrate egal (USB),
    // bei einer Bridge bestimmt sie die Leitung zum Zielchip.
    setLineCoding(m_baudRate);
    resetControlLines();

    ESP_LOGI(TAG, "serial session open (%s, %lu baud)",
             driverName(info.driver), static_cast<unsigned long>(m_baudRate));
}

void UsbTarget::closeSerial()
{
    // Atomar herausnehmen: closeSerial() kommt aus zwei Tasks (USB-Event-
    // Callback bei DEVICE_DISCONNECTED und onDeviceGone). Zwei gleichzeitige
    // Aufrufe wuerden denselben Zeiger zweimal freigeben.
    CdcAcmDevice* dev = __atomic_exchange_n(&m_serial, nullptr, __ATOMIC_SEQ_CST);
    if (dev == nullptr) return;

    dev->close();
    delete dev;
}

esp_err_t UsbTarget::write(const uint8_t* data, size_t length, uint32_t timeoutMs)
{
    CdcAcmDevice* dev = m_serial;
    if (dev == nullptr) return ESP_ERR_INVALID_STATE;
    return dev->tx_blocking(const_cast<uint8_t*>(data), length, timeoutMs);
}

esp_err_t UsbTarget::setLineCoding(uint32_t baud, uint8_t dataBits, uint8_t parity,
                                   uint8_t stopBits)
{
    CdcAcmDevice* dev = m_serial;
    if (dev == nullptr) return ESP_ERR_INVALID_STATE;

    cdc_acm_line_coding_t coding = {};
    coding.dwDTERate             = baud;
    coding.bDataBits             = dataBits;
    coding.bParityType           = parity;
    coding.bCharFormat           = stopBits;

    const esp_err_t err = dev->line_coding_set(&coding);
    if (err == ESP_OK) m_baudRate = baud;
    return err;
}

esp_err_t UsbTarget::resetControlLines()
{
    // Siehe Kommentar im Header: DTR nur bei echtem CDC.
    const bool cdc = (device().kind == Kind::Cdc);
    return setControlLines(cdc, false);
}

esp_err_t UsbTarget::setControlLines(bool dtr, bool rts)
{
    CdcAcmDevice* dev = m_serial;
    if (dev == nullptr) return ESP_ERR_INVALID_STATE;
    return dev->set_control_line_state(dtr, rts);
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

void UsbTarget::daemonTask(void* arg)
{
    (void)arg;

    while (true)
    {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);

        if ((flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0)
        {
            usb_host_device_free_all();
        }
        if ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0)
        {
            ESP_LOGD(TAG, "all devices freed");
        }
    }
}

void UsbTarget::clientTask(void* arg)
{
    auto* self = static_cast<UsbTarget*>(arg);

    usb_host_client_config_t clientConfig    = {};
    clientConfig.is_synchronous              = false;
    clientConfig.max_num_event_msg           = 5;
    clientConfig.async.client_event_callback = &UsbTarget::clientEventCb;
    clientConfig.async.callback_arg          = self;

    const esp_err_t err = usb_host_client_register(&clientConfig, &self->m_client);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "usb_host_client_register failed: %s", esp_err_to_name(err));
        vTaskDelete(nullptr);
        return;
    }

    while (true)
    {
        usb_host_client_handle_events(self->m_client, portMAX_DELAY);
    }
}

void UsbTarget::clientEventCb(const usb_host_client_event_msg_t* msg, void* arg)
{
    auto* self = static_cast<UsbTarget*>(arg);

    switch (msg->event)
    {
        case USB_HOST_CLIENT_EVENT_NEW_DEV:
            self->onNewDevice(msg->new_dev.address);
            break;
        case USB_HOST_CLIENT_EVENT_DEV_GONE:
            self->onDeviceGone(msg->dev_gone.dev_hdl);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Identifikation
// ---------------------------------------------------------------------------

void UsbTarget::identify(DeviceInfo& info)
{
    for (const KnownDevice& known : KNOWN_DEVICES)
    {
        if (known.vid != info.vid) continue;
        if (known.pid != ANY_PID && known.pid != info.pid) continue;

        info.description = known.name;
        info.kind        = known.kind;
        info.driver      = known.driver;
        return;
    }

    // Unbekannte VID: nach Klassen-Code einordnen. Die Geräteklasse ist bei
    // Composite-Geräten 0x00 oder 0xEF und sagt dann nichts — in dem Fall
    // zählt die Klasse des ersten Interface.
    const uint8_t cls = (info.deviceClass != 0 && info.deviceClass != USB_CLASS_MISC_)
                            ? info.deviceClass
                            : info.interfaceClass;

    switch (cls)
    {
        case USB_CLASS_CDC_:
        case USB_CLASS_CDC_DATA_:
            info.kind        = Kind::Cdc;
            info.driver      = Driver::CdcAcm;
            info.description = "USB-CDC-Geraet (COM-Port)";
            break;
        case USB_CLASS_MSC_:
            info.kind        = Kind::Msc;
            info.driver      = Driver::Msc;
            info.description = "USB-Massenspeicher";
            break;
        case USB_CLASS_HID_:
            info.kind        = Kind::Hid;
            info.driver      = Driver::Unsupported;
            info.description = "USB-HID-Geraet";
            break;
        case USB_CLASS_HUB_:
            info.kind        = Kind::Hub;
            info.driver      = Driver::Unsupported;
            info.description = "USB-Hub";
            break;
        case USB_CLASS_VENDOR_:
            // Fast immer eine UART-Bridge mit herstellereigenem Protokoll.
            info.kind        = Kind::SerialBridge;
            info.driver      = Driver::Unsupported;
            info.description = "COM-Port (unbekannter Chip)";
            break;
        default:
            info.kind        = Kind::Unknown;
            info.driver      = Driver::Unsupported;
            info.description = "Unbekanntes USB-Geraet";
            break;
    }

    // Wenn das Gerät sich selbst benennt, ist das aussagekräftiger als unsere
    // Klassen-Heuristik.
    if (!info.product.empty())
    {
        info.description += " / " + info.product;
    }
}

// ---------------------------------------------------------------------------
// Geräte-Ereignisse
// ---------------------------------------------------------------------------

void UsbTarget::onNewDevice(uint8_t address)
{
    usb_device_handle_t handle = nullptr;
    esp_err_t           err    = usb_host_device_open(m_client, address, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "device_open(%u) failed: %s", address, esp_err_to_name(err));
        return;
    }

    DeviceInfo info;
    info.address = address;

    usb_device_info_t devInfo = {};
    if (usb_host_device_info(handle, &devInfo) == ESP_OK)
    {
        info.speed        = speedName(devInfo.speed);
        info.manufacturer = toAscii(devInfo.str_desc_manufacturer);
        info.product      = toAscii(devInfo.str_desc_product);
        info.serial       = toAscii(devInfo.str_desc_serial_num);
    }

    const usb_device_desc_t* deviceDesc = nullptr;
    if (usb_host_get_device_descriptor(handle, &deviceDesc) == ESP_OK && deviceDesc != nullptr)
    {
        info.vid         = deviceDesc->idVendor;
        info.pid         = deviceDesc->idProduct;
        info.deviceClass = deviceDesc->bDeviceClass;
    }

    const usb_config_desc_t* configDesc = nullptr;
    if (usb_host_get_active_config_descriptor(handle, &configDesc) == ESP_OK && configDesc != nullptr)
    {
        int                    offset = 0;
        const usb_intf_desc_t* intf   = usb_parse_interface_descriptor(configDesc, 0, 0, &offset);
        if (intf != nullptr) info.interfaceClass = intf->bInterfaceClass;
    }

    identify(info);

    xSemaphoreTake(m_mutex, portMAX_DELAY);
    m_device       = handle;
    m_info         = info;
    m_state        = State::Connected;
    m_stateSinceUs = esp_timer_get_time();
    xSemaphoreGive(m_mutex);

    ESP_LOGI(TAG, "target attached: %s", info.description.c_str());
    ESP_LOGI(TAG, "  %04X:%04X  kind=%s  driver=%s  class=0x%02X/0x%02X  speed=%s",
             info.vid, info.pid, kindName(info.kind), driverName(info.driver),
             info.deviceClass, info.interfaceClass, info.speed.c_str());
    ESP_LOGI(TAG, "  manufacturer='%s' product='%s' serial='%s'",
             info.manufacturer.c_str(), info.product.c_str(), info.serial.c_str());

    if (info.kind == Kind::Bootsel)
    {
        ESP_LOGW(TAG, "  RP2040 im BOOTSEL-Modus, bereit fuer UF2");
    }
    else if (info.driver == Driver::Unsupported)
    {
        ESP_LOGW(TAG, "  fuer dieses Geraet gibt es noch keinen Host-Treiber");
    }
    else if (m_serialTask != nullptr)
    {
        // Öffnen im eigenen Task, nicht hier: wir stecken im Client-Callback.
        xTaskNotifyGive(m_serialTask);
    }
}

void UsbTarget::onDeviceGone(usb_device_handle_t handle)
{
    ESP_LOGW(TAG, "target detached");

    xSemaphoreTake(m_mutex, portMAX_DELAY);
    const bool wasOurs = (handle == m_device);
    if (wasOurs)
    {
        m_device       = nullptr;
        m_info         = DeviceInfo{};
        m_state        = State::NoTarget;
        m_stateSinceUs = esp_timer_get_time();
    }
    xSemaphoreGive(m_mutex);

    usb_host_device_close(m_client, handle);
}
