// HoneyCord — WASAPI-Loopback-Capture für Screen-Share-System-Audio (Windows).
//
// Spiegelt analog zum Mac-SCK-Audio (siehe FlutterRTCDesktopCapturer.m) das,
// was gerade aus dem System-Lautsprecher kommt, in eine
// libwebrtc::RTCAudioSource (SourceType::kCustom). Das Plugin baut daraus
// einen RTCAudioTrack, der via LiveKit als TrackSource.screenShareAudio
// publiziert wird — separat vom Mikrofon-Track.
//
// Implementierung:
//  - BEVORZUGT (Win10 2004+, Fix id13-Echo): Process-Loopback mit
//    Selbst-Ausschluss (ActivateAudioInterfaceAsync +
//    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK im Modus
//    EXCLUDE_TARGET_PROCESS_TREE auf die eigene PID) — captured ALLES
//    System-Audio AUSSER HoneyCords eigener Wiedergabe. Windows-Pendant zu
//    SCKs excludesCurrentProcessAudio (Mac). Vorher ging die eigene
//    Voice-WIEDERGABE (= die Stimmen der Zuschauer) mit in den Stream und kam
//    zu ihnen zurueck — digital sauber, an jedem AEC vorbei. Format geben wir
//    vor (48 kHz/16 Bit/stereo), Capture Event-getrieben.
//  - FALLBACK (Aktivierung schlaegt fehl): klassisches Endpoint-Loopback
//    (Default-Render-Device + AUDCLNT_STREAMFLAGS_LOOPBACK) = bisheriges
//    Verhalten inkl. eigener Wiedergabe — Status quo statt Crash.
//  - WASAPI liefert im Fallback typischerweise Float32 im Geraete-Mix-Format
//    (oft 48 kHz, 2 Kanäle). Wir konvertieren zu Int16-interleaved; fremde
//    Raten via Linear-Resampler auf 48 kHz (bei 48-kHz-Input 1:1).
//  - libwebrtc' AudioSink-API erwartet exakt 10-ms-Frames pro Push (gleicher
//    RTC_CHECK_EQ wie auf Mac → SIGABRT bei abweichender Größe), daher
//    Ring-Buffer mit Drain in 480-Frame-Blöcken @ 48 kHz.
#ifndef HONEYCORD_WASAPI_LOOPBACK_H_
#define HONEYCORD_WASAPI_LOOPBACK_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "rtc_audio_source.h"

struct IAudioCaptureClient;
struct IAudioClient;
struct IMMDevice;
typedef struct tWAVEFORMATEX WAVEFORMATEX;

namespace honeycord {

// Schreibt eine Zeile nach %TEMP%\honeycord-wasapi.log (auch nutzbar aus
// dem Plugin-Code, nicht nur intern). printf-style.
void WasapiLogFromPlugin(const char* fmt, ...);

class WasapiLoopback {
 public:
  // include_pid == 0 (Default, Screen-Share): System-Mix OHNE den eigenen
  // Prozessbaum (EXCLUDE, id13-Echo-Fix). include_pid != 0 (Fenster-/Game-
  // Share, id16 Per-App-Audio): NUR das Audio des Prozessbaums dieser PID
  // (INCLUDE) — z. B. nur das geteilte Spiel, keine Musik/Notifys daneben.
  // Schlaegt der Include-Modus fehl, faellt Start() auf Exclude (System-Mix)
  // und zuletzt aufs Endpoint-Loopback zurueck.
  explicit WasapiLoopback(
      libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source,
      unsigned long include_pid = 0);
  // ★ HoneyCord (05.09.2026, „Vier Stroeme" Block 1): NICHT der System-Mitschnitt,
  // sondern ein gewaehltes AUFNAHMEGERAET (WASAPI-Endpoint-Id, z. B. die USB-
  // Tonseite einer Capture-Karte). Kein Process-Loopback, kein Loopback-Flag;
  // Format, Resampler und 10-ms-Bloecke sind dieselben wie beim Mitschnitt.
  WasapiLoopback(libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source,
                 const std::wstring& capture_device_id);
  ~WasapiLoopback();

  // Startet den COM-/WASAPI-Stack und den Capture-Thread. Gibt false zurück,
  // wenn die Initialisierung fehlschlägt (kein Render-Device, kein Mix-Format,
  // o. ä.). Idempotent — zweiter Aufruf nach erfolgreichem Start ist No-Op.
  bool Start();

  // Beendet den Capture-Thread und gibt alle COM-Objekte frei. Wird auch im
  // Destruktor aufgerufen.
  void Stop();

 private:
  void CaptureLoop();
  // Versucht Process-Loopback (bevorzugt); setzt client_/mix_format_/
  // capture_event_. include_mode=true -> NUR include_pid_-Prozessbaum
  // (Per-App-Audio); false -> alles AUSSER dem eigenen Prozessbaum.
  // false-Rueckgabe = naechste Fallback-Stufe versuchen.
  bool TryStartProcessLoopback(bool include_mode);
  // Klassisches Endpoint-Loopback auf dem Default-Render-Device (Fallback).
  bool StartEndpointLoopback();
  // Aufnahme eines gewaehlten Capture-Endpoints (capture_device_id_), ohne
  // Loopback-Flag — der Kartenton-Weg.
  bool StartCaptureEndpoint();
  // Gibt COM-Objekte/Format/Event frei (fuer Stop + Aufraeumen zwischen den
  // beiden Start-Versuchen). Stoppt NICHT den Thread.
  void ReleaseCom();

  libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source_;
  // Per-App-Audio (id16): PID des geteilten Fensters (0 = Screen-Share ->
  // System-Mix im Exclude-Modus).
  unsigned long include_pid_ = 0;
  std::wstring capture_device_id_;   // leer = Mitschnitt-Modi wie bisher
  std::atomic<bool> running_{false};
  std::thread thread_;

  IMMDevice* device_ = nullptr;
  IAudioClient* client_ = nullptr;
  IAudioCaptureClient* capture_ = nullptr;
  WAVEFORMATEX* mix_format_ = nullptr;
  // Capture-Event (Process-Loopback- und Capture-Endpoint-Modus; nur der
  // Endpoint-Fallback pollt). void* statt HANDLE, damit der Header ohne
  // windows.h auskommt.
  void* capture_event_ = nullptr;

  // 10-ms-Akkumulator (interleaved Int16) NACH Resampling auf
  // libwebrtc-Opus-fähige Sample-Rate. Größe = chunk_frames_ * channels_.
  std::vector<int16_t> chunk_buf_;
  size_t chunk_frames_ = 0;        // 10 ms @ out_sample_rate_ in Frames
  size_t chunk_filled_frames_ = 0; // wie weit im Akkumulator wir sind
  int in_sample_rate_ = 0;         // WASAPI-Mix-Format-Rate (z. B. 44100)
  int out_sample_rate_ = 0;        // Ziel-Rate (48000), libwebrtc/Opus
  int channels_ = 0;
  bool input_is_float_ = false;    // Float32-Input statt PCM16

  // Linear-Interpolation-Resampler-State (Phase + letzte Input-Samples pro
  // Kanal). Bei 44100 → 48000 ist das Verhältnis ~0.91875; pro Output-Frame
  // konsumieren wir im Mittel 0.918... Input-Frames. Phase trackt den
  // Bruchteil über CaptureLoop-Iterationen hinweg.
  double resample_phase_ = 0.0;
  int16_t prev_left_ = 0;
  int16_t prev_right_ = 0;
};

}  // namespace honeycord

#endif  // HONEYCORD_WASAPI_LOOPBACK_H_
