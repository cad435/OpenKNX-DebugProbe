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

/// VID von Espressif. Deren Chips mit nativem USB werten DTR und RTS im
/// USB-Serial-JTAG selbst aus, brauchen aber eine andere Folge als die
/// Transistorschaltung auf den Bridge-Boards.
constexpr uint16_t VID_ESPRESSIF = 0x303A;

/// Wie lange EN beim Reset unten bleibt (esptool: 100 ms).
constexpr uint32_t RESET_HOLD_MS = 100;

/// Wie lange IO0 nach dem Loslassen von EN noch unten bleibt, damit der Chip
/// den Pegel beim Hochlaufen sicher abtastet (esptool: 50 ms).
constexpr uint32_t BOOT_HOLD_MS = 50;

/// Schrittweite der USB-Serial-JTAG-Folge (esptool: 100 ms je Schritt).
constexpr uint32_t JTAG_STEP_MS = 100;

/// Der USB-Serial-JTAG haengt am selben Kabel wie die Daten: nach einem Reset
/// muss sich der Port erst neu melden. esptool wartet hier 200 ms.
constexpr uint32_t JTAG_SETTLE_MS = 200;

/*
 * PICOBOOT — das Vendor-Interface des RP2040-Boot-ROMs. Werte aus
 * `boot/picoboot.h` des pico-sdk, nicht aus dem Gedaechtnis:
 * Magic, Control-Request zum Ruecksetzen des Interface, Kommando-ID, und die
 * Feldoffsets des 32 Byte grossen Kommandopakets.
 */
constexpr uint32_t PICOBOOT_MAGIC    = 0x431FD10Bu;
constexpr uint8_t  PICOBOOT_IF_RESET = 0x41;  ///< Control OUT, wLength 0
constexpr uint8_t  PC_REBOOT         = 0x02;
constexpr size_t   PICOBOOT_CMD_LEN  = 32;

/// Vorlauf, den das Boot-ROM vor dem Neustart wartet. Lang genug, dass die
/// Quittung noch durchkommt, kurz genug, dass es sich sofort anfuehlt.
constexpr uint32_t PICOBOOT_DELAY_MS = 100;

/// Geduld fuer einen einzelnen PICOBOOT-Transfer.
constexpr uint32_t PICOBOOT_TIMEOUT_MS = 1000;

/*
 * HID-Klassenrequests (USB HID 1.11, 7.2). Feature-Reports laufen als
 * Steuertransfer ueber Endpunkt 0 — dafuer braucht es keinen HID-Treiber.
 */
constexpr uint8_t  HID_REQ_GET_REPORT = 0x01;
constexpr uint8_t  HID_REQ_SET_REPORT = 0x09;
constexpr uint16_t HID_REPORT_FEATURE = 0x03;

/// VID von pid.codes. Der rv003usb-Bootloader meldet sich darunter.
constexpr uint16_t VID_PIDCODES     = 0x1209;
constexpr uint16_t PID_RV003USB_BL  = 0xB003;

void put32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

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

    // CH32V003 mit cnlohrs rv003usb-Bootloader. Der Chip hat keine
    // USB-Peripherie — das ist bitgebangtes Low-Speed-USB in Software.
    {0x1209, 0xB003, "CH32V003 im rv003usb-HID-Bootloader",
                                                      UsbTarget::Kind::HidBootloader, UsbTarget::Driver::HidRaw},
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
        case Kind::HidBootloader: return "hid_bootloader";
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
        case Driver::HidRaw:      return "hid_raw";
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

// ---------------------------------------------------------------------------
// Neustart des Ziels
// ---------------------------------------------------------------------------

UsbTarget::ResetSupport UsbTarget::resetSupport(ResetMode mode) const
{
    ResetSupport out;

    if (state() != State::Connected)
    {
        out.how = "kein Zielgeraet angesteckt";
        return out;
    }

    const DeviceInfo info = device();

    /*
     * Ziel steht im BOOTSEL: es meldet sich als Massenspeicher, eine serielle
     * Sitzung gibt es dann nicht. In den Bootmodus muss es niemand mehr
     * schicken; zurueck in die Anwendung fuehrt ueber USB aber auch kein Weg,
     * denn das Boot-ROM startet erst nach einem vollstaendigen UF2 neu.
     */
    if (isMscReady() || info.kind == Kind::Bootsel || info.kind == Kind::Msc)
    {
        if (mode == ResetMode::Bootloader)
        {
            out.possible = true;
            out.how      = "Ziel steht bereits im BOOTSEL";
            return out;
        }
        if (m_picobootIntf != PICOBOOT_NO_INTF)
        {
            out.possible = true;
            out.how      = "PICOBOOT-Reboot ueber das Vendor-Interface des Boot-ROMs";
            return out;
        }

        out.how = "Ziel steht im BOOTSEL und bietet kein PICOBOOT-Interface an - entweder "
                  "ein UF2 schreiben oder am Geraet RUN druecken.";
        return out;
    }

    /*
     * Die Pruefung auf eine offene serielle Sitzung gehoert NUR zu den
     * CDC-Wegen. Sie stand hier einmal davor und hat damit die HID-Zweige
     * blockiert: ein HID-Geraet hat nie eine serielle Sitzung, und der
     * rv003usb-Bootmodus laeuft ueber Endpunkt 0.
     */
    switch (info.kind)
    {
        case Kind::SerialBridge:
            if (!isSerialOpen())
            {
                out.how = "keine serielle Sitzung zum Ziel offen";
                return out;
            }
            out.possible = true;
            out.how      = (mode == ResetMode::Run)
                               ? "RTS-Puls auf EN (esptool-Hard-Reset)"
                               : "DTR/RTS-Folge auf IO0 und EN (esptool-Bootloader-Reset)";
            return out;

        case Kind::Cdc:
            if (!isSerialOpen())
            {
                out.how = "keine serielle Sitzung zum Ziel offen";
                return out;
            }
            if (info.vid == VID_ESPRESSIF)
            {
                out.possible = true;
                out.how      = (mode == ResetMode::Run)
                                   ? "RTS-Puls ueber den USB-Serial-JTAG"
                                   : "DTR/RTS-Folge ueber den USB-Serial-JTAG";
                return out;
            }
            if (info.vid == VID_RASPBERRY)
            {
                if (mode == ResetMode::Bootloader)
                {
                    out.possible = true;
                    out.how      = "1200-Baud-Touch";
                    return out;
                }
                /*
                 * Kein Versehen, sondern die Hardware: bei nativem USB gibt es
                 * keine Leitung am Kabel, die den RP2040 zuruecksetzt. DTR und
                 * RTS landen in der Firmware des Ziels, nicht an RUN.
                 */
                out.how = "Ein RP2040 mit nativem USB hat keine Reset-Leitung am Kabel, "
                          "und PICOBOOT gibt es nur im Boot-ROM. Aus der Ferne geht es "
                          "trotzdem: erst in den Bootmodus, dann neu starten - der Weg "
                          "fuehrt dann durchs Boot-ROM. Sonst: Konsole des Ziels, ein "
                          "neues UF2, oder der RUN-Taster.";
                return out;
            }
            out.how = "Unbekanntes CDC-Geraet: wie dessen Reset beschaltet ist, weiss die "
                      "Probe nicht - ein DTR/RTS-Puls waere geraten.";
            return out;

        case Kind::HidBootloader:
            /*
             * Steht schon im Bootloader. Zurueck in die Anwendung fuehrt nur
             * der Weg ueber minichlink: dieser Bootloader kennt kein
             * "boot"-Kommando, sondern nur "fuehre diesen Scratchpad aus" —
             * der Code dafuer kommt vom Werkzeug, nicht von der Probe.
             */
            if (mode == ResetMode::Bootloader)
            {
                out.possible = true;
                out.how      = "Ziel steht bereits im rv003usb-Bootloader";
                return out;
            }
            out.how = "Ziel steht im rv003usb-Bootloader. Zurueck in die Anwendung kommt "
                      "es ueber minichlink - der Bootloader fuehrt nur hochgeladenen Code "
                      "aus und kennt kein eigenes Boot-Kommando.";
            return out;

        case Kind::Hid:
            if (mode == ResetMode::Bootloader && m_hidIntf != PICOBOOT_NO_INTF)
            {
                /*
                 * Konvention des rv003usb-Bootloaders, keine USB-Norm. Ein
                 * Ziel, das die Report-ID nicht kennt, quittiert mit einem
                 * Stall — das ist harmlos und wird als Absage gemeldet.
                 * Deshalb wird der Versuch angeboten, statt ihn an eine
                 * PID-Liste zu binden, die nie vollstaendig waere.
                 */
                out.possible = true;
                out.how      = "HID-Feature-Report 0xAB (rv003usb-Konvention)";
                return out;
            }
            out.how = (mode == ResetMode::Run)
                          ? "Ein HID-Geraet hat keine Reset-Leitung am Kabel."
                          : "kein HID-Interface am Ziel gefunden";
            return out;

        default:
            out.how = std::string("Geraeteart '") + kindName(info.kind) +
                      "' kennt keinen Neustart ueber USB.";
            return out;
    }
}

esp_err_t UsbTarget::bridgeReset(ResetMode mode)
{
    /*
     * Beschaltung wie auf jedem ESP32-Board mit Bridge-Chip: RTS an EN, DTR an
     * IO0, beide ueber ein Transistorpaar, das bei zwei gleichzeitig aktiven
     * Leitungen absichtlich gar nichts tut.
     *
     * Wir setzen beide Bits in einem einzigen Steuertransfer. Der
     * Zwischenzustand, ueber den pyserial hier stolpert - eine Leitung nach der
     * anderen, rund 50 ms auseinander, siehe CLAUDE.md zu RFC2217 - entsteht
     * dabei gar nicht erst.
     */
    esp_err_t err = setControlLines(false, true);  // EN low: Ziel im Reset
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(RESET_HOLD_MS));

    if (mode == ResetMode::Bootloader)
    {
        err = setControlLines(true, false);  // EN frei, IO0 noch low
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(BOOT_HOLD_MS));
    }

    return setControlLines(false, false);
}

esp_err_t UsbTarget::jtagReset(ResetMode mode)
{
    /*
     * USB-Serial-JTAG der ESP32-C- und -S-Reihe. Hier werten nicht Transistoren
     * die Steuerleitungen aus, sondern der Chip selbst — und er baut dabei den
     * Interlock der Devkit-Schaltung nach. Am Geraet gemessen (ESP32-C3,
     * 2026-09-12):
     *
     *     DTR inaktiv + RTS-Puls  -> Chip resettet (rst:0x15 USB_UART_CHIP_RESET)
     *     DTR aktiv   + RTS-Puls  -> gar nichts, keine Neuanmeldung
     *
     * Beide Leitungen gleichzeitig aktiv sind also wirkungslos, damit ein
     * Terminal nicht versehentlich resettet.
     *
     * Der Download-Modus kommt deshalb nicht aus dem Pegel im Moment des
     * Loslassens, sondern aus einem **Latch**: eine Phase mit aktivem DTR
     * merkt sich der Chip, und beim naechsten Reset bootet er in den
     * Download-Modus. Genau darum hat esptool fuer diese Chips eine eigene
     * Folge (USBJTAGSerialReset) und geht dabei bewusst NICHT ueber (0,0) —
     * das wuerde den Latch wieder loeschen.
     *
     * Und genau da lag der Fehler: die Konsole laesst DTR bei CDC absichtlich
     * aktiv (resetControlLines(), damit arduino-pico seine Ausgabe nicht
     * zurueckhaelt). Der Ausgangszustand ist also (DTR aktiv, RTS inaktiv) —
     * ein Reset von dort aus ist fuer den Chip eine Download-Anforderung. Ein
     * Neustart in die Anwendung muss den Latch erst loeschen.
     */
    esp_err_t err = setControlLines(false, false);  // Latch loeschen
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(JTAG_STEP_MS));

    if (mode == ResetMode::Bootloader)
    {
        // DTR allein aktiv: GPIO9 wird gelatcht.
        err = setControlLines(true, false);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(JTAG_STEP_MS));

        /*
         * Beide Bits in einem Transfer: der Uebergang beruehrt weder (1,1) noch
         * (0,0). Das erste waere wirkungslos, das zweite wuerde den Latch
         * loeschen — pyserial stolpert genau hier, weil es Leitung fuer Leitung
         * quittiert (siehe CLAUDE.md zu RFC2217).
         */
        err = setControlLines(false, true);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(JTAG_STEP_MS));
    }
    else
    {
        // Kein DTR dazwischen — der Latch bleibt leer, der Chip bootet normal.
        err = setControlLines(false, true);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(JTAG_STEP_MS));
    }

    err = setControlLines(false, false);  // Reset loslassen
    if (err != ESP_OK) return err;

    // Der Port meldet sich nach dem Reset neu; esptool wartet hier genauso.
    vTaskDelay(pdMS_TO_TICKS(JTAG_SETTLE_MS));
    return ESP_OK;
}

void UsbTarget::picobootXferCb(usb_transfer_t* xfer)
{
    // Laeuft im clientTask (dort wird usb_host_client_handle_events gepumpt).
    auto sem = static_cast<SemaphoreHandle_t>(xfer->context);
    if (sem != nullptr) xSemaphoreGive(sem);
}

void UsbTarget::findPicoboot(usb_device_handle_t handle)
{
    m_picobootIntf  = PICOBOOT_NO_INTF;
    m_picobootEpOut = 0;
    m_picobootEpIn  = 0;

    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(handle, &cfg) != ESP_OK || cfg == nullptr) return;

    for (uint8_t n = 0; n < cfg->bNumInterfaces; ++n)
    {
        int                    offset = 0;
        const usb_intf_desc_t* intf   = usb_parse_interface_descriptor(cfg, n, 0, &offset);
        if (intf == nullptr) continue;

        // PICOBOOT meldet sich als Vendor-spezifisch ohne Subklasse/Protokoll.
        // Der Massenspeicher daneben ist 0x08 und faellt hier heraus.
        if (intf->bInterfaceClass != USB_CLASS_VENDOR_) continue;
        if (intf->bInterfaceSubClass != 0 || intf->bInterfaceProtocol != 0) continue;

        uint8_t epOut = 0;
        uint8_t epIn  = 0;

        for (int e = 0; e < intf->bNumEndpoints; ++e)
        {
            // Der Offset muss je Endpunkt wieder am Interface starten.
            int                  epOffset = offset;
            const usb_ep_desc_t* ep =
                usb_parse_endpoint_descriptor_by_index(intf, e, cfg->wTotalLength, &epOffset);
            if (ep == nullptr) continue;
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) !=
                USB_BM_ATTRIBUTES_XFER_BULK)
                continue;

            if ((ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) != 0)
                epIn = ep->bEndpointAddress;
            else
                epOut = ep->bEndpointAddress;
        }

        if (epOut == 0) continue;  // ohne OUT nichts zu senden

        m_picobootIntf  = intf->bInterfaceNumber;
        m_picobootEpOut = epOut;
        m_picobootEpIn  = epIn;

        ESP_LOGI(TAG, "  PICOBOOT auf Interface %u (out 0x%02X, in 0x%02X)",
                 m_picobootIntf, m_picobootEpOut, m_picobootEpIn);
        return;
    }
}

void UsbTarget::findHid(usb_device_handle_t handle)
{
    m_hidIntf = PICOBOOT_NO_INTF;

    const usb_config_desc_t* cfg = nullptr;
    if (usb_host_get_active_config_descriptor(handle, &cfg) != ESP_OK || cfg == nullptr) return;

    for (uint8_t n = 0; n < cfg->bNumInterfaces; ++n)
    {
        int                    offset = 0;
        const usb_intf_desc_t* intf   = usb_parse_interface_descriptor(cfg, n, 0, &offset);
        if (intf == nullptr) continue;
        if (intf->bInterfaceClass != USB_CLASS_HID_) continue;

        m_hidIntf = intf->bInterfaceNumber;
        ESP_LOGI(TAG, "  HID auf Interface %u", m_hidIntf);
        return;
    }
}

esp_err_t UsbTarget::hidGetFeature(uint8_t reportId, size_t length, std::string& error)
{
    return hidFeature(false, reportId, nullptr, length, error);
}

esp_err_t UsbTarget::hidFeature(bool toDevice, uint8_t reportId, uint8_t* data, size_t length,
                                std::string& error)
{
    error.clear();

    if (m_hidIntf == PICOBOOT_NO_INTF)
    {
        error = "kein HID-Interface am Ziel gefunden";
        return ESP_ERR_NOT_SUPPORTED;
    }

    usb_device_handle_t device = m_device;
    if (device == nullptr)
    {
        error = "Zielgeraet ist nicht geoeffnet";
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t intf = m_hidIntf;

    esp_err_t err = usb_host_interface_claim(m_client, device, intf, 0);
    if (err != ESP_OK)
    {
        error = std::string("HID-Interface nicht belegbar: ") + esp_err_to_name(err);
        return err;
    }

    SemaphoreHandle_t sem  = xSemaphoreCreateBinary();
    usb_transfer_t*   xfer = nullptr;

    auto cleanup = [&]() {
        if (xfer != nullptr) usb_host_transfer_free(xfer);
        if (sem != nullptr) vSemaphoreDelete(sem);
        // Nach einem Reboot ins Bootloader-Image ist das Geraet weg; Fehler normal.
        usb_host_interface_release(m_client, device, intf);
    };

    if (sem == nullptr)
    {
        cleanup();
        error = "kein Speicher fuer die Quittung";
        return ESP_ERR_NO_MEM;
    }

    err = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + length, 0, &xfer);
    if (err != ESP_OK)
    {
        cleanup();
        error = std::string("kein Transferpuffer: ") + esp_err_to_name(err);
        return err;
    }

    auto* setup          = reinterpret_cast<usb_setup_packet_t*>(xfer->data_buffer);
    setup->bmRequestType = (toDevice ? USB_BM_REQUEST_TYPE_DIR_OUT : USB_BM_REQUEST_TYPE_DIR_IN) |
                           USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest      = toDevice ? HID_REQ_SET_REPORT : HID_REQ_GET_REPORT;
    setup->wValue        = static_cast<uint16_t>((HID_REPORT_FEATURE << 8) | reportId);
    setup->wIndex        = intf;
    setup->wLength       = static_cast<uint16_t>(length);

    // hidapi-Konvention: bei einer Report-ID != 0 geht der ganze Puffer raus,
    // die ID eingeschlossen. Genau so erwartet es der Bootloader.
    if (toDevice && data != nullptr)
    {
        std::memcpy(xfer->data_buffer + sizeof(usb_setup_packet_t), data, length);
    }

    xfer->device_handle    = device;
    xfer->bEndpointAddress = 0;
    xfer->num_bytes        = sizeof(usb_setup_packet_t) + length;
    xfer->callback         = &UsbTarget::picobootXferCb;
    xfer->context          = sem;
    xfer->timeout_ms       = PICOBOOT_TIMEOUT_MS;

    err = usb_host_transfer_submit_control(m_client, xfer);
    if (err != ESP_OK)
    {
        cleanup();
        error = std::string("Feature-Report nicht absetzbar: ") + esp_err_to_name(err);
        return err;
    }

    /*
     * Wie bei PICOBOOT und beim 1200-Baud-Touch: bleibt die Quittung aus, ist
     * das kein Fehler. Ein Ziel, das auf diesen Report hin neu startet, reisst
     * den Transfer genau dabei ab — das ist der Erfolgsfall, nicht der
     * Fehlerfall.
     */
    if (xSemaphoreTake(sem, pdMS_TO_TICKS(PICOBOOT_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "HID-Feature 0x%02X: keine Quittung - Ziel startet vermutlich neu",
                 reportId);
    }
    else if (xfer->status != USB_TRANSFER_STATUS_COMPLETED)
    {
        /*
         * Ein Stall heisst: die Report-ID kennt das Ziel nicht. Das ist eine
         * ehrliche Absage und wird auch als solche gemeldet — sonst behauptete
         * die Probe einen Bootmodus, den es nie gab.
         */
        if (xfer->status == USB_TRANSFER_STATUS_STALL)
        {
            cleanup();
            char ids[8];
            snprintf(ids, sizeof(ids), "0x%02X", reportId);
            error = std::string("Ziel kennt den Feature-Report ") + ids +
                    " nicht (Stall) - andere Firmware, oder es steht schon im Bootloader";
            return ESP_ERR_NOT_SUPPORTED;
        }
        ESP_LOGW(TAG, "HID-Feature 0x%02X: Status %d - Ziel startet vermutlich neu",
                 reportId, static_cast<int>(xfer->status));
    }

    if (!toDevice && data != nullptr && xfer->actual_num_bytes > 0)
    {
        const size_t got =
            std::min(length, static_cast<size_t>(xfer->actual_num_bytes));
        std::memcpy(data, xfer->data_buffer + sizeof(usb_setup_packet_t), got);
    }

    cleanup();
    return ESP_OK;
}

esp_err_t UsbTarget::picobootReboot(std::string& error)
{
    error.clear();

    if (m_picobootIntf == PICOBOOT_NO_INTF)
    {
        error = "Ziel bietet kein PICOBOOT-Interface an";
        return ESP_ERR_NOT_SUPPORTED;
    }

    usb_device_handle_t device = m_device;
    if (device == nullptr)
    {
        error = "Zielgeraet ist nicht geoeffnet";
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t intf = m_picobootIntf;

    esp_err_t err = usb_host_interface_claim(m_client, device, intf, 0);
    if (err != ESP_OK)
    {
        error = std::string("PICOBOOT-Interface nicht belegbar: ") + esp_err_to_name(err);
        return err;
    }

    SemaphoreHandle_t sem  = xSemaphoreCreateBinary();
    usb_transfer_t*   xfer = nullptr;

    auto cleanup = [&]() {
        if (xfer != nullptr) usb_host_transfer_free(xfer);
        if (sem != nullptr) vSemaphoreDelete(sem);
        // Nach einem Neustart ist das Geraet weg; ein Fehler hier ist normal.
        usb_host_interface_release(m_client, device, intf);
    };

    if (sem == nullptr)
    {
        cleanup();
        error = "kein Speicher fuer die Quittung";
        return ESP_ERR_NO_MEM;
    }

    err = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + PICOBOOT_CMD_LEN, 0, &xfer);
    if (err != ESP_OK)
    {
        cleanup();
        error = std::string("kein Transferpuffer: ") + esp_err_to_name(err);
        return err;
    }

    auto submitAndWait = [&](bool control) -> esp_err_t {
        xfer->device_handle = device;
        xfer->callback      = &UsbTarget::picobootXferCb;
        xfer->context       = sem;
        xfer->timeout_ms    = PICOBOOT_TIMEOUT_MS;

        const esp_err_t sErr = control ? usb_host_transfer_submit_control(m_client, xfer)
                                       : usb_host_transfer_submit(xfer);
        if (sErr != ESP_OK) return sErr;

        if (xSemaphoreTake(sem, pdMS_TO_TICKS(PICOBOOT_TIMEOUT_MS)) != pdTRUE)
            return ESP_ERR_TIMEOUT;

        return ESP_OK;
    };

    /*
     * Schritt 1: das Interface zuruecksetzen. Das hebt haengende Stalls auf und
     * ist der erste Griff, den picotool auch macht — ohne ihn kann eine
     * abgebrochene Vorsitzung das Bulk-Paar blockiert lassen.
     */
    auto* setup          = reinterpret_cast<usb_setup_packet_t*>(xfer->data_buffer);
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR |
                           USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest      = PICOBOOT_IF_RESET;
    setup->wValue        = 0;
    setup->wIndex        = intf;
    setup->wLength       = 0;

    xfer->bEndpointAddress = 0;
    xfer->num_bytes        = sizeof(usb_setup_packet_t);

    err = submitAndWait(true);
    if (err != ESP_OK)
    {
        cleanup();
        error = std::string("PICOBOOT-Interface liess sich nicht zuruecksetzen: ") +
                esp_err_to_name(err);
        return err;
    }

    /*
     * Schritt 2: das Kommandopaket. Die Felder werden einzeln und
     * little-endian geschrieben, statt eine Struktur aus dem pico-sdk zu
     * spiegeln — so haengt das Layout nicht am Packing des Compilers.
     *
     *   dMagic | dToken | bCmdId | bCmdSize | _unused | dTransferLength | args
     *      4        4        1         1          2            4            16
     *
     * args ist hier picoboot_reboot_cmd: dPC, dSP, dDelayMS. **dPC = 0 heisst
     * „zurueck in den regulaeren Boot-Pfad"** — genau das, was gebraucht wird.
     * dSP wird dabei nicht ausgewertet.
     */
    uint8_t* cmd = xfer->data_buffer;
    std::memset(cmd, 0, PICOBOOT_CMD_LEN);
    put32(cmd + 0, PICOBOOT_MAGIC);
    put32(cmd + 4, 1);  // dToken, nur zum Korrelieren einer Statusabfrage
    cmd[8]  = PC_REBOOT;
    cmd[9]  = 12;  // bCmdSize: drei uint32 in args
    put32(cmd + 12, 0);                      // dTransferLength
    put32(cmd + 16, 0);                      // dPC = 0 -> regulaerer Boot-Pfad
    put32(cmd + 20, 0);                      // dSP, bei dPC = 0 ohne Bedeutung
    put32(cmd + 24, PICOBOOT_DELAY_MS);      // dDelayMS

    xfer->bEndpointAddress = m_picobootEpOut;
    xfer->num_bytes        = PICOBOOT_CMD_LEN;

    err = usb_host_transfer_submit(xfer);
    if (err != ESP_OK)
    {
        cleanup();
        error = std::string("PICOBOOT-Kommando nicht absetzbar: ") + esp_err_to_name(err);
        return err;
    }

    /*
     * Auf die Quittung wird gewartet, ihr Ausbleiben ist aber kein Fehler: das
     * Ziel startet nach dDelayMS neu und reisst den Transfer dabei ab. Genau
     * dieselbe Nachsicht wie beim 1200-Baud-Touch, wo das Geraet noch waehrend
     * des Steuertransfers verschwindet.
     */
    if (xSemaphoreTake(sem, pdMS_TO_TICKS(PICOBOOT_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "PICOBOOT: keine Quittung - Ziel startet vermutlich schon neu");
    }
    else if (xfer->status != USB_TRANSFER_STATUS_COMPLETED)
    {
        ESP_LOGW(TAG, "PICOBOOT: Transfer endete mit Status %d - Ziel startet vermutlich neu",
                 static_cast<int>(xfer->status));
    }

    cleanup();
    return ESP_OK;
}

esp_err_t UsbTarget::resetTarget(ResetMode mode, std::string& error)
{
    error.clear();

    const ResetSupport support = resetSupport(mode);
    if (!support.possible)
    {
        error = support.how;
        // "nichts angesteckt" ist ein anderer Fehler als "kann das Geraet nicht".
        return (state() != State::Connected) ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_SUPPORTED;
    }

    const DeviceInfo info = device();

    /*
     * Schon im BOOTSEL - hier kommt ohnehin nur der Bootmodus an. Die Abfrage
     * muss dieselbe sein wie in resetSupport(): ein Geraet, das sich gerade
     * erst als Massenspeicher meldet und dessen MSC-Sitzung noch nicht offen
     * ist, liefe sonst in die DTR/RTS-Zweige und scheiterte dort mit einer
     * irrefuehrenden Meldung.
     */
    if (isMscReady() || info.kind == Kind::Bootsel || info.kind == Kind::Msc)
    {
        // Bootmodus ist schon erreicht; zurueck in die Anwendung geht nur ueber
        // PICOBOOT, und resetSupport() hat vorher geprueft, dass es das gibt.
        if (mode == ResetMode::Bootloader) return ESP_OK;
        return picobootReboot(error);
    }

    if (info.kind == Kind::Cdc && info.vid == VID_RASPBERRY)
    {
        // Ebenfalls nur Bootmodus: Run hat resetSupport() schon abgelehnt.
        return enterBootsel(error);
    }

    if (info.kind == Kind::HidBootloader) return ESP_OK;  // steht schon dort

    if (info.kind == Kind::Hid)
    {
        // Nur Bootmodus kommt hier an, Run hat resetSupport() abgelehnt.
        return hidGetFeature(HID_BOOT_REPORT, 64, error);
    }

    const esp_err_t err =
        (info.kind == Kind::SerialBridge) ? bridgeReset(mode) : jtagReset(mode);

    if (err != ESP_OK)
    {
        error = std::string("Steuerleitungen liessen sich nicht setzen: ") +
                esp_err_to_name(err);
        return err;
    }

    /*
     * Nach einem Neustart in die Anwendung soll die Konsole sofort wieder
     * mitlesen koennen, also zurueck in den Ruhezustand. Im Bootmodus bleibt
     * stehen, was die Folge hinterlassen hat: dort wartet als Naechstes ein
     * Flash-Werkzeug, das seine Leitungen ohnehin selbst setzt.
     */
    if (mode == ResetMode::Run) resetControlLines();

    return ESP_OK;
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
            /*
             * HidRaw statt Unsupported: fuer HID braucht es keinen
             * Host-Treiber. Feature-Reports laufen als Klassen-Steuertransfer
             * ueber Endpunkt 0, und genau darueber geht der Bootmodus eines
             * rv003usb-Ziels (Report 0xAB) und das ganze B003-Protokoll.
             * "kein Host-Treiber" waere hier schlicht falsch.
             */
            info.kind        = Kind::Hid;
            info.driver      = Driver::HidRaw;
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
        // Legt fest, ob /api/reset aus dem BOOTSEL heraus etwas kann.
        findPicoboot(handle);
    }
    else if (info.kind == Kind::Hid || info.kind == Kind::HidBootloader)
    {
        // Fuer Feature-Reports ueber Endpunkt 0 (rv003usb-Bootmodus).
        findHid(handle);
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

    m_picobootIntf  = PICOBOOT_NO_INTF;
    m_picobootEpOut = 0;
    m_picobootEpIn  = 0;
    m_hidIntf       = PICOBOOT_NO_INTF;

    usb_host_device_close(m_client, handle);
}
