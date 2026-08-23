#pragma once

/**
 * Meldung nach draussen, dass ein Client an der Bruecke haengt.
 *
 * Gebraucht fuer das dynamische WLAN-Stromsparen: solange jemand die Konsole
 * mitliest oder gerade flasht, muss der Funk wach bleiben; liegt die Probe
 * ungenutzt herum, darf sie dosen.
 *
 * Bewusst ein Funktionszeiger und kein Verweis auf den WiFiManager — die
 * Bruecke soll nichts vom WLAN wissen. Verdrahtet wird das in `main.cpp`,
 * genauso wie `UsbTarget::RxCallback`.
 */
using ClientActivityHook = void (*)(bool busy, void* ctx);

/**
 * RAII-Klammer um eine Client-Sitzung: meldet beim Anlegen „belegt", beim
 * Verlassen des Blocks „frei".
 *
 * `serveClient()` hat in beiden Servern mehrere Ausstiegspunkte. Von Hand
 * gesetzte Gegenstuecke wuerden dort irgendwann vergessen, und dann bliebe der
 * Funk fuer immer wach — ein Fehler, der niemandem auffaellt, weil alles
 * funktioniert und nur der Strom hoeher ist.
 */
class ClientActivityScope
{
public:
    ClientActivityScope(ClientActivityHook hook, void* ctx) : m_hook(hook), m_ctx(ctx)
    {
        if (m_hook != nullptr) m_hook(true, m_ctx);
    }

    ~ClientActivityScope()
    {
        if (m_hook != nullptr) m_hook(false, m_ctx);
    }

    ClientActivityScope(const ClientActivityScope&)            = delete;
    ClientActivityScope& operator=(const ClientActivityScope&) = delete;

private:
    ClientActivityHook m_hook;
    void*              m_ctx;
};
