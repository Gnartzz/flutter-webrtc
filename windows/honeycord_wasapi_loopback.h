// HoneyCord — WASAPI-Loopback-Capture für Screen-Share-System-Audio (Windows).
//
// Spiegelt analog zum Mac-SCK-Audio (siehe FlutterRTCDesktopCapturer.m) das,
// was gerade aus dem System-Lautsprecher kommt, in eine
// libwebrtc::RTCAudioSource (SourceType::kCustom). Das Plugin baut daraus
// einen RTCAudioTrack, der via LiveKit als TrackSource.screenShareAudio
// publiziert wird — separat vom Mikrofon-Track.
//
// Implementierung:
//  - WASAPI shared-mode Loopback (IAudioClient::Initialize mit
//    AUDCLNT_STREAMFLAGS_LOOPBACK), liefert das gemixte Render-Stream als
//    Capture.
//  - WASAPI gibt typischerweise Float32 (IEEE754) im Geräte-Mix-Format
//    (oft 48 kHz, 2 Kanäle). Wir konvertieren zu Int16-interleaved.
//  - libwebrtc' AudioSink-API erwartet exakt 10-ms-Frames pro Push (gleicher
//    RTC_CHECK_EQ wie auf Mac → SIGABRT bei abweichender Größe), daher
//    Ring-Buffer mit Drain in 480-Frame-Blöcken @ 48 kHz.
#ifndef HONEYCORD_WASAPI_LOOPBACK_H_
#define HONEYCORD_WASAPI_LOOPBACK_H_

#include <atomic>
#include <cstdint>
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
  explicit WasapiLoopback(
      libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source);
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

  libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source_;
  std::atomic<bool> running_{false};
  std::thread thread_;

  IMMDevice* device_ = nullptr;
  IAudioClient* client_ = nullptr;
  IAudioCaptureClient* capture_ = nullptr;
  WAVEFORMATEX* mix_format_ = nullptr;

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
