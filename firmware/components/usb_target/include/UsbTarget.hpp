#pragma once

#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/msc_host.h"
#include "usb/usb_host.h"

/**
 * USB-Host-Seite der Probe: erkennt das angesteckte Zielgerät und hält dessen
 * Identität fest.
 *
 * Stufe 2 — nur Enumeration und Identifikation. Datenverkehr (serielle Konsole,
 * UF2) kommt in den folgenden Stufen darauf auf.
 *
 * Achtung: sobald der OTG-Port als Host läuft, ist der USB-Serial-JTAG des S3 tot
 * (gleiche Pins GPIO19/20). Die Konsole muss auf UART0 liegen.
 */
class UsbTarget
{
public:
    enum class State
    {
        NotStarted,
        NoTarget,
        Connected,
    };

    /// Was das Gerät funktional ist — entscheidet später über den Betriebsmodus.
    enum class Kind
    {
        Unknown,
        Cdc,           ///< echtes USB-CDC-ACM (Klasse 0x02) — RP2040, ESP32-S2/S3, STM32
        SerialBridge,  ///< vendor-specific UART-Bridge (CH34x, CP210x, FTDI, PL2303)
        Bootsel,       ///< RP2040 im Boot-ROM (RPI-RP2, Massenspeicher)
        Msc,           ///< sonstiger Massenspeicher
        Hid,
        Hub,
    };

    /**
     * Welcher Host-Treiber gebraucht wird, um mit dem Gerät zu reden.
     *
     * Wichtig für die Planung: `usb_host_cdc_acm` deckt nur `Cdc` ab. Für die
     * Bridge-Chips braucht es je einen eigenen Treiber (in der ESP-Component-
     * Registry als `usb_host_ch34x_vcp`, `usb_host_cp210x_vcp`,
     * `usb_host_ftdi_vcp` vorhanden).
     */
    enum class Driver
    {
        None,
        CdcAcm,
        Msc,
        Ch34x,
        Cp210x,
        Ftdi,
        Pl2303,
        Unsupported,
    };

    struct DeviceInfo
    {
        uint8_t     address {0};
        uint16_t    vid {0};
        uint16_t    pid {0};
        uint8_t     deviceClass {0};
        uint8_t     interfaceClass {0};
        Kind        kind {Kind::Unknown};
        Driver      driver {Driver::None};
        std::string description;   ///< sprechender Name, z. B. "USB-UART-Bridge CH340"
        std::string manufacturer;  ///< aus dem Stringdescriptor
        std::string product;       ///< aus dem Stringdescriptor
        std::string serial;        ///< aus dem Stringdescriptor
        std::string speed;
    };

    /// Magische Baudrate, auf die RP2040-Firmware mit einem Reboot nach
    /// BOOTSEL reagiert (arduino-pico und picotool machen es genauso).
    static constexpr uint32_t TOUCH_BAUD = 1200;

    /// Ruhezustand der Konsole, auf den nach einem Touch zurueckgestellt wird.
    static constexpr uint32_t CONSOLE_BAUD = 115200;

    UsbTarget() = default;
    ~UsbTarget();

    UsbTarget(const UsbTarget&)            = delete;
    UsbTarget& operator=(const UsbTarget&) = delete;

    /// Installiert den USB-Host-Stack und startet Daemon- und Client-Task.
    esp_err_t begin();

    State       state() const;
    DeviceInfo  device() const;
    const char* stateName() const;

    /// Sekunden seit dem letzten Wechsel (angesteckt / abgezogen).
    uint32_t secondsInState() const;

    static const char* kindName(Kind kind);
    static const char* driverName(Driver driver);

    // -----------------------------------------------------------------------
    // Serielle Sitzung zum Ziel
    // -----------------------------------------------------------------------

    /// Wird aus dem Treiber-Task aufgerufen, sobald Daten vom Ziel kommen.
    using RxCallback = void (*)(const uint8_t* data, size_t length, void* ctx);

    void setRxCallback(RxCallback cb, void* ctx);

    /// true, sobald CDC-ACM bzw. der VCP-Treiber das Ziel geöffnet hat.
    bool isSerialOpen() const;

    esp_err_t write(const uint8_t* data, size_t length, uint32_t timeoutMs = 1000);

    esp_err_t setLineCoding(uint32_t baudRate,
                            uint8_t  dataBits = 8,
                            uint8_t  parity   = 0,
                            uint8_t  stopBits = 0);
    esp_err_t setControlLines(bool dtr, bool rts);

    /**
     * Setzt DTR/RTS auf den fuer die Geraeteart passenden Ruhezustand.
     *
     * Bei echtem CDC muss DTR aktiv sein, sonst haelt das Zielgeraet seine
     * Ausgabe zurueck (arduino-pico wertet DTR als "Port ist offen" aus).
     *
     * Bei einer UART-Bridge waere genau das schaedlich: dort haengen DTR und
     * RTS am Auto-Reset-Transistorpaar des Boards. DTR aktiv zieht IO0 auf
     * low, und das Ziel startet beim naechsten Reset im Bootloader statt in
     * seiner Anwendung. Deshalb bleiben hier beide Leitungen inaktiv.
     */
    esp_err_t resetControlLines();

    uint32_t baudRate() const { return m_baudRate; }

    // -----------------------------------------------------------------------
    // Massenspeicher (RP2040 im BOOTSEL-Modus)
    // -----------------------------------------------------------------------

    /**
     * Schickt ein laufendes RP2040-Ziel per 1200-Baud-Touch nach BOOTSEL und
     * wartet, bis es sich als Massenspeicher gemeldet hat.
     *
     * Das ist Schritt 3 des Flash-Ablaufs aus `docs/architecture.md` und der
     * Grund, warum am Geraet niemand Taster druecken muss. Bleibt der
     * Massenspeicher aus, wird **ein** Versuch wiederholt; danach steht die
     * Konsole wieder im Ruhezustand, damit ein gescheiterter Touch das Ziel
     * nicht mit 1200 Baud und abgefallenem DTR zuruecklaesst.
     *
     * Steht das Ziel schon im BOOTSEL, kehrt die Funktion sofort zurueck.
     *
     * @param error  bei Misserfolg gefuellt, sonst geleert.
     */
    esp_err_t enterBootsel(std::string& error);

    bool     isMscReady() const { return m_msc != nullptr; }
    uint32_t mscSectorSize() const { return m_mscSectorSize; }
    uint32_t mscSectorCount() const { return m_mscSectorCount; }

    /// Roher Sektorschreibzugriff. Das RP2040-Boot-ROM durchsucht jeden
    /// geschriebenen Sektor nach dem UF2-Magic; ein Dateisystem braucht es nicht.
    esp_err_t writeSector(uint32_t sector, const void* data, size_t size);

private:
    static void daemonTask(void* arg);
    static void clientTask(void* arg);
    static void clientEventCb(const usb_host_client_event_msg_t* msg, void* arg);

    void onNewDevice(uint8_t address);
    void onDeviceGone(usb_device_handle_t handle);

    /// Füllt kind/driver/description aus VID/PID und den Klassen-Codes.
    static void identify(DeviceInfo& info);

    void setState(State state);

    /// Öffnet die serielle Sitzung. Läuft im eigenen Task, weil
    /// cdc_acm_host_open() blockiert und den USB-Client-Task nicht blockieren darf.
    static void serialTask(void* arg);
    void        openSerial();
    void        closeSerial();

    static bool serialDataCb(const uint8_t* data, size_t length, void* arg);
    static void serialEventCb(const cdc_acm_host_dev_event_data_t* event, void* arg);

    /// Der eigentliche Touch: Line-Coding auf 1200, DTR an und wieder aus.
    esp_err_t touch();

    /// Pollt bis zum Timeout, ob der Massenspeicher aufgetaucht ist.
    bool waitForMsc(uint32_t timeoutMs);

    /// Baudrate und Steuerleitungen zurueck in den Konsolen-Ruhezustand.
    esp_err_t restoreConsole();

    static void mscEventCb(const msc_host_event_t* event, void* arg);
    void        openMsc();
    void        closeMsc();

    usb_host_client_handle_t m_client {nullptr};
    usb_device_handle_t      m_device {nullptr};
    TaskHandle_t             m_daemonTask {nullptr};
    TaskHandle_t             m_clientTask {nullptr};
    TaskHandle_t             m_serialTask {nullptr};
    SemaphoreHandle_t        m_mutex {nullptr};
    State                    m_state {State::NotStarted};
    DeviceInfo               m_info {};
    int64_t                  m_stateSinceUs {0};

    CdcAcmDevice* m_serial {nullptr};
    RxCallback    m_rxCallback {nullptr};
    void*         m_rxContext {nullptr};
    uint32_t      m_baudRate {115200};

    msc_host_device_handle_t m_msc {nullptr};
    volatile uint8_t         m_mscPendingAddress {0};
    uint32_t                 m_mscSectorSize {512};
    uint32_t                 m_mscSectorCount {0};
};
