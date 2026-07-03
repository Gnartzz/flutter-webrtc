// HoneyCord — Memory-Telemetrie (Windows). Diagnose fuer den OOM-Befund vom
// Abendtest 2026-07-03 (Client + Spiel starben nach ~2h mit Out-of-memory):
// loggt einmal pro Minute RAM (WorkingSet/PrivateBytes), Handle-/GDI-/USER-
// Objekt-Zahlen (klassische Leak-Indikatoren) und VRAM (DXGI-Budget/Usage)
// nach %LOCALAPPDATA%\HoneyCord\mem.log (rotiert bei >1 MB). Die naechste
// lange Session liefert damit die Wachstumskurve: Leak (steigt linear) vs.
// externe Ursache (flach).
#ifndef HONEYCORD_MEM_TELEMETRY_H_
#define HONEYCORD_MEM_TELEMETRY_H_

namespace honeycord {

// Startet den Telemetrie-Thread einmal pro Prozess (weitere Aufrufe = No-Op).
void StartMemTelemetryOnce();

}  // namespace honeycord

#endif  // HONEYCORD_MEM_TELEMETRY_H_
