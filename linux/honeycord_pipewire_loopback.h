// HoneyCord — PipeWire-Mitschnitt des Systemtons für die Bildschirmfreigabe (Linux).
//
// Pendant zu honeycord_wasapi_loopback (Windows) und zum SCK-Audio-Pfad des
// Macs (FlutterRTCDesktopCapturer.m): der Ton, den andere Programme gerade
// abspielen, landet in einer libwebrtc::RTCAudioSource (SourceType::kCustom).
// Das Plugin baut daraus einen RTCAudioTrack, den LiveKit als
// TrackSource.screenShareAudio publiziert — getrennt vom Mikrofon.
//
// WARUM NICHT DER EINFACHE WEG (Monitor-Mitschnitt):
// Die Monitor-Quelle einer Ausgabe enthält ALLES, was aus dem Lautsprecher
// kommt — auch HoneyCords eigene Wiedergabe. Wer teilt, schickte damit die
// Stimmen der Zuhörer digital sauber an jedem Echo-Unterdrücker vorbei zu
// ihnen zurück. Genau das war Fehler id13; auf Windows löst ihn
// EXCLUDE_TARGET_PROCESS_TREE, auf dem Mac excludesCurrentProcessAudio.
//
// WIE ES HIER GELÖST IST:
// Der Aufnehmer verbindet sich NICHT automatisch (node.autoconnect = false).
// Stattdessen beobachten wir die Registratur und verknüpfen gezielt jeden
// fremden Wiedergabe-Knoten (media.class = "Stream/Output/Audio") mit ihm —
// die eigenen Knoten (gleiche Prozess-ID) bleiben außen vor. PipeWire mischt
// mehrere Verknüpfungen am Eingang selbst zusammen; wir müssen nichts mischen.
//
// GEMESSEN 2026-07-26 (Fedora 42, PipeWire 1.4.11), zwei Testtöne parallel:
//   fremde App  440 Hz -> Pegel 5742   (kommt an)
//   eigener Ton 1000 Hz -> Pegel    0,5 (bleibt draußen)
// Mit zwei fremden Apps gleichzeitig: 440 Hz -> 5744, 660 Hz -> 5480, eigener
// Ton 3,3. Verknüpfen auf KNOTEN-Ebene genügt (ohne Anschluss-Buchführung),
// ebenfalls gemessen.
//
// Format geben wir vor (48 kHz / 16 Bit / stereo), PipeWire wandelt selbst —
// deshalb braucht es hier keinen eigenen Resampler wie auf Windows.
// libwebrtc' Audio-Senke erwartet exakt 10-ms-Blöcke pro Push (RTC_CHECK im
// Kern -> Abbruch bei abweichender Größe), daher Sammelpuffer mit Abgabe in
// 480er-Blöcken.
#ifndef HONEYCORD_PIPEWIRE_LOOPBACK_H_
#define HONEYCORD_PIPEWIRE_LOOPBACK_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "rtc_audio_source.h"

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_stream;
struct pw_proxy;
struct spa_dict;

namespace honeycord {

// Schreibt eine Zeile nach $XDG_STATE_HOME/honeycord/screen-audio.log
// (ersatzweise ~/.local/state/honeycord/…). printf-Stil, auch aus dem
// Plugin-Code nutzbar.
void PwLogFromPlugin(const char* fmt, ...);

class PipewireLoopback {
 public:
  explicit PipewireLoopback(
      libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source);
  ~PipewireLoopback();

  PipewireLoopback(const PipewireLoopback&) = delete;
  PipewireLoopback& operator=(const PipewireLoopback&) = delete;

  // Baut Verbindung, Aufnehmer und Registratur-Beobachter auf und startet die
  // eigene Schleife. false = PipeWire nicht erreichbar (dann bleibt der
  // Bildschirm einfach ohne Ton, statt dass etwas abstürzt).
  bool Start();
  void Stop();

 private:
  // --- Rückrufe aus der PipeWire-Schleife -----------------------------------
  static void OnRegistryGlobal(void* data, uint32_t id, uint32_t permissions,
                               const char* type, uint32_t version,
                               const struct spa_dict* props);
  static void OnRegistryGlobalRemove(void* data, uint32_t id);
  static void OnStreamProcess(void* data);
  static void OnStreamStateChanged(void* data, int old_state, int state,
                                   const char* error);

  void HandleNode(uint32_t id, const struct spa_dict* props);
  void HandlePort(uint32_t id, const struct spa_dict* props);
  // True, wenn dieser Knoten zu UNS gehoert und deshalb nicht mitgeschnitten
  // werden darf. Siehe Kommentar in der .cc — die Prozess-ID allein reicht im
  // Flatpak-Sandkasten nicht.
  bool IstEigenerKnoten(const struct spa_dict* props) const;
  void VerknuepfeOffene();
  void PushPcm(const int16_t* frames, uint32_t frame_count);

  libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source_;

  // Die spa_hook-Strukturen leben in der .cc, damit dieser Header keine
  // PipeWire-Kopfdateien braucht (er wird auch vom gemeinsamen Plugin-Code
  // eingebunden).
  struct Hooks;
  Hooks* hooks_ = nullptr;

  pw_thread_loop* loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  pw_registry* registry_ = nullptr;
  pw_stream* stream_ = nullptr;

  // Ein Anschluss (Port) in PipeWires Registratur.
  struct Anschluss {
    uint32_t id = 0;
    uint32_t node_id = 0;
    std::string kanal;  // "FL", "FR", "MONO", …
  };

  // Eine von uns angelegte Verknüpfung — mit der Knoten-ID der fremden
  // Anwendung, damit wir beim Verschwinden gezielt aufräumen können.
  struct Verknuepfung {
    uint32_t node_id = 0;
    uint32_t out_port = 0;
    uint32_t in_port = 0;
    pw_proxy* proxy = nullptr;
  };

  std::vector<Anschluss> fremde_ausgaenge_;  // Ausgänge fremder Anwendungen
  std::vector<Anschluss> eigene_eingaenge_;  // Eingänge unseres Aufnehmers
  std::vector<uint32_t> apps_;               // fremde Wiedergabe-Knoten
  std::vector<Verknuepfung> links_;

  uint32_t own_node_id_ = 0;  // unser Aufnehmer (nie mit sich selbst verknüpfen)
  int own_pid_ = 0;
  std::string own_app_id_;    // FLATPAK_ID, falls im Sandkasten
  std::string own_binary_;    // /proc/self/comm

  // Sammelpuffer für die 10-ms-Abgabe.
  std::vector<int16_t> chunk_;
  uint32_t chunk_filled_frames_ = 0;

  bool started_ = false;

  static constexpr int kSampleRate = 48000;
  static constexpr int kChannels = 2;
  static constexpr uint32_t kChunkFrames = kSampleRate / 100;  // 10 ms = 480
};

}  // namespace honeycord

#endif  // HONEYCORD_PIPEWIRE_LOOPBACK_H_
