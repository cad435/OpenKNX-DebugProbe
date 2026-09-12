#pragma once

#include <atomic>
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
        HidBootloader, ///< rv003usb-Bootloader auf einem CH32V003 (bitgebangtes USB)
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
        HidRaw,        ///< kein Treiber noetig: Feature-Reports ueber Endpunkt 0
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

    // -----------------------------------------------------------------------
    // Neustart des Ziels
    // -----------------------------------------------------------------------

    /// Wohin das Ziel neu gestartet werden soll.
    enum class ResetMode
    {
        Run,         ///< in die eigene Anwendung
        Bootloader,  ///< im Bootlader bzw. BOOTSEL anhalten
    };

    /**
     * Ob dieser Neustart beim aktuell angesteckten Ziel ueberhaupt geht - und
     * womit.
     *
     * Bewusst ein Paar aus Flag und Klartext: das Web-UI kann den Knopf damit
     * sperren **und** danebenschreiben, warum. Ein blosses `false` liesse den
     * Anwender raten, ob die Probe, das Kabel oder das Ziel schuld ist.
     */
    struct ResetSupport
    {
        bool        possible {false};
        std::string how;  ///< das Verfahren, bei `!possible` der Grund
    };

    ResetSupport resetSupport(ResetMode mode) const;

    /**
     * Startet das Ziel neu.
     *
     * Einen gemeinsamen Mechanismus gibt es dafuer nicht - welcher greift,
     * haengt am Geraet:
     *
     * | Ziel | Run | Bootloader |
     * |---|---|---|
     * | UART-Bridge (CH34x/CP210x/FTDI) | RTS-Puls auf EN | DTR/RTS-Folge auf IO0 und EN |
     * | Espressif mit nativem USB | RTS-Puls ueber USB-Serial-JTAG | DTR/RTS-Folge ueber USB-Serial-JTAG |
     * | RP2040 (CDC) | **nicht moeglich** | 1200-Baud-Touch |
     * | RP2040 im BOOTSEL | **nicht moeglich** | steht schon dort |
     *
     * @param error  bei Misserfolg gefuellt, sonst geleert.
     */
    esp_err_t resetTarget(ResetMode mode, std::string& error);

    /**
     * Standard-Report, mit dem ein rv003usb-Ziel in seinen Bootloader
     * zurueckgeschickt wird.
     *
     * Das ist **keine** USB-Norm, sondern die Konvention dieses Bootloaders:
     * die Anwendung beantwortet ein GET_REPORT auf diese ID, indem sie
     * `FLASH->STATR` Bit 14 setzt, den D--Pullup kurz abschaltet und sich
     * selbst resettet. Danach meldet sie sich als `1209:B003`.
     */
    static constexpr uint8_t HID_BOOT_REPORT = 0xAB;

    /**
     * Holt einen HID-Feature-Report vom Ziel (GET_REPORT ueber Endpunkt 0).
     *
     * Bewusst ohne HID-Host-Treiber: Feature-Reports sind Klassen-
     * Steuertransfers auf dem Default-Pipe, dieselbe Maschinerie wie bei
     * PICOBOOT. Ein Ziel, das die Report-ID nicht kennt, quittiert mit einem
     * Stall — das meldet der Transfer als Fehler und richtet keinen Schaden an.
     */
    esp_err_t hidGetFeature(uint8_t reportId, size_t length, std::string& error);

    /**
     * HID-Feature-Report in beide Richtungen, mit Nutzdaten.
     *
     * @param toDevice true = SET_REPORT (0x09), false = GET_REPORT (0x01)
     * @param reportId Report-ID; steht bei hidapi-Konvention auch in data[0]
     * @param data     Puffer, @p length Bytes, inklusive der Report-ID
     *
     * Das ist die Transportschicht des rv003usb-Bootloaders: minichlink
     * schickt seinen Scratchpad als Feature-Report und pollt das Ergebnis
     * ueber einen zweiten. Beides sind Steuertransfers auf Endpunkt 0.
     */
    esp_err_t hidFeature(bool toDevice, uint8_t reportId, uint8_t* data, size_t length,
                         std::string& error, size_t* actual = nullptr);

    /// true, wenn ein HID-Interface gefunden wurde.
    bool hasHid() const { return m_hidIntf != PICOBOOT_NO_INTF; }

    /**
     * true, wenn das Ziel das PICOBOOT-Interface des RP2040-Boot-ROMs
     * anbietet. Nur dann gibt es aus dem BOOTSEL einen Weg zurueck in die
     * Anwendung, ohne ein UF2 zu schreiben.
     */
    bool hasPicoboot() const { return m_picobootIntf != PICOBOOT_NO_INTF; }

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

    /// DTR/RTS-Folge fuer die Transistorschaltung auf ESP32-Boards mit
    /// Bridge-Chip (DTR an IO0, RTS an EN).
    esp_err_t bridgeReset(ResetMode mode);

    /// DTR/RTS-Folge fuer den USB-Serial-JTAG der ESP32-S- und -C-Reihe, wo
    /// der Chip die beiden Leitungen selbst auswertet.
    esp_err_t jtagReset(ResetMode mode);

    /**
     * Startet einen RP2040 im BOOTSEL ueber PICOBOOT in seine Anwendung.
     *
     * Das Boot-ROM legt neben dem Massenspeicher ein zweites, herstellereigenes
     * Interface aus (Klasse 0xFF/0/0) mit einem Bulk-Paar. Darueber laeuft
     * dasselbe Protokoll, mit dem `picotool reboot` arbeitet: ein 32 Byte
     * grosses Kommandopaket, `PC_REBOOT` mit `dPC = 0` heisst „zurueck in den
     * regulaeren Boot-Pfad".
     *
     * Nur aus dem BOOTSEL heraus. Eine laufende RP2040-Anwendung bietet kein
     * PICOBOOT an — dort bleibt ein Neustart ueber USB unmoeglich.
     */
    esp_err_t picobootReboot(std::string& error);

    /// Sucht das PICOBOOT-Interface und dessen Bulk-Endpunkte im
    /// Konfigurationsdeskriptor.
    void findPicoboot(usb_device_handle_t handle);

    /// Merkt sich die Nummer des HID-Interface (Klasse 0x03).
    void findHid(usb_device_handle_t handle);

    static void picobootXferCb(usb_transfer_t* xfer);

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

    /// Sentinel: kein PICOBOOT-Interface gefunden.
    static constexpr uint8_t PICOBOOT_NO_INTF = 0xFF;

    uint8_t m_picobootIntf {PICOBOOT_NO_INTF};
    uint8_t m_hidIntf {PICOBOOT_NO_INTF};  ///< derselbe Sentinel

    /**
     * Das aktuell belegte Interface, oder PICOBOOT_NO_INTF.
     *
     * Muss ueber den Aufruf hinaus bekannt sein: verschwindet das Ziel
     * mitten in einem Transfer, raeumt `onDeviceGone()` auf — und
     * `usb_host_device_close()` scheitert, solange noch ein Interface belegt
     * ist. Das Geraeteobjekt bliebe dann liegen und an der Adresse wuerde
     * nichts mehr enumerieren: der USB-Stack der Probe haengt.
     *
     * `atomic`, weil Freigabe aus zwei Tasks kommen kann (HTTP-Handler und
     * USB-Client-Task). Wer zuerst tauscht, gibt frei.
     */
    std::atomic<uint8_t> m_claimedIntf {PICOBOOT_NO_INTF};

    /// Belegt @p intf und merkt es sich.
    esp_err_t claimInterface(usb_device_handle_t device, uint8_t intf);

    /// Gibt ein gemerktes Interface frei. Mehrfachaufruf ist harmlos.
    void releaseClaimed(usb_device_handle_t device);
    uint8_t m_picobootEpOut {0};
    uint8_t m_picobootEpIn {0};

    msc_host_device_handle_t m_msc {nullptr};
    volatile uint8_t         m_mscPendingAddress {0};
    uint32_t                 m_mscSectorSize {512};
    uint32_t                 m_mscSectorCount {0};
};
