#include "honeycord_pipewire_loopback.h"

#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>

namespace honeycord {
namespace {

// Ein Wiedergabe-Strom einer Anwendung. Genau diese Knoten wollen wir
// einsammeln — Geräte (Audio/Sink) und Aufnehmer sind hier uninteressant.
constexpr char kAppPlaybackClass[] = "Stream/Output/Audio";

FILE* OpenLog() {
  static FILE* f = nullptr;
  static bool tried = false;
  if (tried) return f;
  tried = true;
  std::string dir;
  if (const char* state = getenv("XDG_STATE_HOME")) {
    dir = std::string(state) + "/honeycord";
  } else if (const char* home = getenv("HOME")) {
    dir = std::string(home) + "/.local/state/honeycord";
  } else {
    dir = "/tmp";
  }
  std::string cmd = "mkdir -p '" + dir + "' 2>/dev/null";
  if (system(cmd.c_str()) != 0) dir = "/tmp";
  f = fopen((dir + "/screen-audio.log").c_str(), "a");
  return f;
}

}  // namespace

void PwLogFromPlugin(const char* fmt, ...) {
  FILE* f = OpenLog();
  if (!f) return;
  time_t t = time(nullptr);
  char stamp[32] = {0};
  struct tm tmv;
  if (localtime_r(&t, &tmv)) strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);
  fprintf(f, "[%s] ", stamp);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fflush(f);
}

// spa_hook-Strukturen: liegen hier, damit der Header PipeWire-frei bleibt.
struct PipewireLoopback::Hooks {
  spa_hook registry;
  spa_hook stream;
  spa_hook core;
};

namespace {
// Ohne diesen Beobachter scheitert das Anlegen einer Verknuepfung STILL: das
// Werk antwortet asynchron, und der Fehler landet sonst nirgends. Genau daran
// war der erste Versuch nicht zu erkennen (2026-07-26).
void OnCoreError(void* /*data*/, uint32_t id, int seq, int res,
                 const char* message) {
  PwLogFromPlugin("PipeWire meldet Fehler: objekt=%u seq=%d res=%d (%s) — %s", id,
                  seq, res, spa_strerror(res), message ? message : "");
}
const pw_core_events kCoreEvents = {
    PW_VERSION_CORE_EVENTS,
    /*info=*/nullptr,
    /*done=*/nullptr,
    /*ping=*/nullptr,
    OnCoreError,
    /*remove_id=*/nullptr,
    /*bound_id=*/nullptr,
    /*add_mem=*/nullptr,
    /*remove_mem=*/nullptr,
    /*bound_props=*/nullptr,
};
}  // namespace

PipewireLoopback::PipewireLoopback(
    libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source)
    : source_(source) {
  chunk_.resize(static_cast<size_t>(kChunkFrames) * kChannels, 0);
  own_pid_ = static_cast<int>(getpid());

  // GEMESSEN 2026-07-26 im Flatpak: getpid() liefert dort 2 (eigener
  // Prozess-Namensraum), waehrend PipeWire die Nummer von AUSSEN meldet. Der
  // Selbst-Ausschluss ueber die Prozess-ID allein greift im Sandkasten also
  // NICHT — unsere eigene Wiedergabe wurde als "fremd" eingestuft und
  // mitgeschnitten. Deshalb zusaetzlich ueber Namen erkennen.
  if (const char* fid = getenv("FLATPAK_ID")) own_app_id_ = fid;
  if (FILE* f = fopen("/proc/self/comm", "r")) {
    char name[128] = {0};
    if (fgets(name, sizeof(name), f)) {
      std::string s(name);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      own_binary_ = s;
    }
    fclose(f);
  }
  PwLogFromPlugin("Selbst-Kennzeichen: pid=%d app-id='%s' programm='%s'",
                  own_pid_, own_app_id_.c_str(), own_binary_.c_str());
}

PipewireLoopback::~PipewireLoopback() {
  Stop();
}

// ── Registratur ──────────────────────────────────────────────────────────────

void PipewireLoopback::OnRegistryGlobal(void* data, uint32_t id,
                                        uint32_t /*permissions*/,
                                        const char* type, uint32_t /*version*/,
                                        const struct spa_dict* props) {
  auto* self = static_cast<PipewireLoopback*>(data);
  if (!type || !props) return;
  if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
    self->HandleNode(id, props);
  } else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
    self->HandlePort(id, props);
  }
}

void PipewireLoopback::OnRegistryGlobalRemove(void* data, uint32_t id) {
  auto* self = static_cast<PipewireLoopback*>(data);

  // Verknuepfungen dieses Knotens (oder dieses Anschlusses) abbauen.
  for (auto it = self->links_.begin(); it != self->links_.end();) {
    if (it->node_id == id || it->out_port == id || it->in_port == id) {
      if (it->proxy) pw_proxy_destroy(it->proxy);
      it = self->links_.erase(it);
      PwLogFromPlugin("Objekt %u verschwunden -> Verknuepfung entfernt", id);
    } else {
      ++it;
    }
  }
  for (auto a = self->apps_.begin(); a != self->apps_.end(); ++a) {
    if (*a == id) {
      self->apps_.erase(a);
      break;
    }
  }
  auto raus = [id](std::vector<Anschluss>& v) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it->id == id || it->node_id == id) {
        v.erase(it);
        return;
      }
    }
  };
  raus(self->fremde_ausgaenge_);
  raus(self->eigene_eingaenge_);
}

bool PipewireLoopback::IstEigenerKnoten(const struct spa_dict* props) const {
  // Drei Kriterien, weil KEINES allein reicht:
  //  - Prozess-ID: greift ausserhalb des Sandkastens (Archiv-Fassung).
  //  - Anwendungs-Kennung: im Flatpak ist getpid() == 2, aber der Knoten heisst
  //    "de.honeycord.honeycord" (= FLATPAK_ID). GEMESSEN 2026-07-26.
  //  - Programmname: falls die Anwendung sich anders meldet.
  const char* pid = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
  if (pid && atoi(pid) == own_pid_) return true;

  auto passt = [](const char* wert, const std::string& eigen) {
    return wert && !eigen.empty() && eigen == wert;
  };
  const char* app = spa_dict_lookup(props, PW_KEY_APP_NAME);
  const char* bin = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY);
  const char* node = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (passt(app, own_app_id_) || passt(app, own_binary_)) return true;
  if (passt(bin, own_binary_)) return true;
  if (passt(node, own_app_id_) || passt(node, own_binary_)) return true;
  return false;
}

void PipewireLoopback::HandleNode(uint32_t id, const struct spa_dict* props) {
  const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!media_class || strcmp(media_class, kAppPlaybackClass) != 0) return;

  // SELBST-AUSSCHLUSS (der eigentliche Zweck der ganzen Konstruktion):
  // Unsere eigene Wiedergabe — also die Stimmen der Zuhoerer — darf NICHT
  // eingesammelt werden, sonst kommt sie bei ihnen zurueck (Fehler id13).
  // Windows: EXCLUDE_TARGET_PROCESS_TREE, macOS: excludesCurrentProcessAudio.
  const char* name = spa_dict_lookup(props, PW_KEY_APP_NAME);
  if (IstEigenerKnoten(props)) {
    PwLogFromPlugin("Knoten %u (%s) ist unsere eigene Wiedergabe -> uebersprungen",
                    id, name ? name : "ohne Namen");
    return;
  }

  for (uint32_t known : apps_) {
    if (known == id) return;
  }
  PwLogFromPlugin("Fremder Wiedergabe-Knoten %u (%s) gefunden", id,
                  name ? name : "ohne Namen");
  apps_.push_back(id);
  VerknuepfeOffene();
}

void PipewireLoopback::HandlePort(uint32_t id, const struct spa_dict* props) {
  const char* richtung = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
  const char* knoten = spa_dict_lookup(props, PW_KEY_NODE_ID);
  if (!richtung || !knoten) return;
  const uint32_t node_id = static_cast<uint32_t>(strtoul(knoten, nullptr, 10));
  const char* kanal = spa_dict_lookup(props, "audio.channel");

  Anschluss a;
  a.id = id;
  a.node_id = node_id;
  a.kanal = kanal ? kanal : "";

  // ALLE Anschluesse merken und erst beim Verknuepfen filtern. Die Registratur
  // garantiert keine Reihenfolge: ein Anschluss kann gemeldet werden, bevor
  // sein Knoten eingeordnet ist — und unsere eigenen Eingaenge erscheinen unter
  // Umstaenden, bevor wir die eigene Knoten-ID kennen. Wer hier frueh filtert,
  // verliert sie still.
  if (strcmp(richtung, "out") == 0) {
    fremde_ausgaenge_.push_back(a);
  } else if (strcmp(richtung, "in") == 0) {
    eigene_eingaenge_.push_back(a);
  } else {
    return;
  }
  VerknuepfeOffene();
}

void PipewireLoopback::VerknuepfeOffene() {
  if (own_node_id_ == 0 || !core_) return;

  // ANSCHLUSS-genau verknuepfen. Der erste Versuch gab dem Verknuepfungs-Werk
  // nur die KNOTEN-Nummern mit — dabei floss nichts (gemessen 2026-07-26: der
  // Aufnehmer blieb auf "paused"). `pw-link`, das im Vorversuch nachweislich
  // funktionierte, verbindet intern Anschluss fuer Anschluss; genau das machen
  // wir jetzt auch. Mehrere Quellen auf denselben Eingang mischt PipeWire.
  for (const Anschluss& aus : fremde_ausgaenge_) {
    // Gehoert dieser Ausgang zu einer fremden Wiedergabe? (Filter hier statt
    // beim Melden — siehe HandlePort.)
    bool ist_app = false;
    for (uint32_t app : apps_) {
      if (app == aus.node_id) {
        ist_app = true;
        break;
      }
    }
    if (!ist_app) continue;

    for (const Anschluss& ein : eigene_eingaenge_) {
      if (ein.node_id != own_node_id_) continue;  // nur unsere eigenen Eingaenge
      // Kanaltreue, wo bekannt; Mono (oder unbenannt) geht auf beide Seiten.
      const bool mono = aus.kanal.empty() || aus.kanal == "MONO";
      if (!mono && !ein.kanal.empty() && aus.kanal != ein.kanal) continue;

      bool schon_da = false;
      for (const Verknuepfung& v : links_) {
        if (v.out_port == aus.id && v.in_port == ein.id) {
          schon_da = true;
          break;
        }
      }
      if (schon_da) continue;

      pw_properties* p = pw_properties_new(nullptr, nullptr);
      pw_properties_setf(p, PW_KEY_LINK_OUTPUT_NODE, "%u", aus.node_id);
      pw_properties_setf(p, PW_KEY_LINK_OUTPUT_PORT, "%u", aus.id);
      pw_properties_setf(p, PW_KEY_LINK_INPUT_NODE, "%u", own_node_id_);
      pw_properties_setf(p, PW_KEY_LINK_INPUT_PORT, "%u", ein.id);
      auto* link = static_cast<pw_proxy*>(
          pw_core_create_object(core_, "link-factory", PW_TYPE_INTERFACE_Link,
                                PW_VERSION_LINK, &p->dict, 0));
      pw_properties_free(p);
      if (link) {
        links_.push_back({aus.node_id, aus.id, ein.id, link});
        PwLogFromPlugin("verknuepft: Knoten %u Anschluss %u (%s) -> unser Anschluss %u (%s)",
                        aus.node_id, aus.id, aus.kanal.c_str(), ein.id,
                        ein.kanal.c_str());
      } else {
        PwLogFromPlugin("Verknuepfung %u -> %u fehlgeschlagen", aus.id, ein.id);
      }
    }
  }
}

// ── Aufnehmer ────────────────────────────────────────────────────────────────

void PipewireLoopback::OnStreamStateChanged(void* data, int /*old_state*/,
                                            int state, const char* error) {
  auto* self = static_cast<PipewireLoopback*>(data);
  PwLogFromPlugin("Aufnehmer-Zustand: %s%s%s",
                  pw_stream_state_as_string(static_cast<pw_stream_state>(state)),
                  error ? " — " : "", error ? error : "");
  if (state == PW_STREAM_STATE_ERROR) return;
  if (self->own_node_id_ == 0 && self->stream_) {
    uint32_t id = pw_stream_get_node_id(self->stream_);
    if (id != PW_ID_ANY && id != 0) {
      self->own_node_id_ = id;
      PwLogFromPlugin("Eigener Aufnehmer hat Knoten-ID %u — hole Rueckstand nach",
                      id);
      // Alles, was vor unserer eigenen Knoten-ID gemeldet wurde, jetzt binden.
      self->VerknuepfeOffene();
    }
  }
}

void PipewireLoopback::OnStreamProcess(void* data) {
  auto* self = static_cast<PipewireLoopback*>(data);
  if (!self->stream_) return;
  pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
  if (!b) return;
  spa_buffer* buf = b->buffer;
  if (buf && buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk) {
    const auto* samples = static_cast<const int16_t*>(buf->datas[0].data);
    const uint32_t bytes = buf->datas[0].chunk->size;
    const uint32_t frames =
        bytes / static_cast<uint32_t>(sizeof(int16_t) * kChannels);
    if (frames > 0) self->PushPcm(samples, frames);
  }
  pw_stream_queue_buffer(self->stream_, b);
}

void PipewireLoopback::PushPcm(const int16_t* frames, uint32_t frame_count) {
  // libwebrtc' Senke verlangt exakt 10-ms-Bloecke (gleicher RTC_CHECK wie auf
  // Mac und Windows -> Abbruch bei abweichender Groesse). Deshalb sammeln und
  // in 480er-Bloecken abgeben.
  uint32_t offset = 0;
  while (offset < frame_count) {
    const uint32_t platz = kChunkFrames - chunk_filled_frames_;
    const uint32_t nehmen = (frame_count - offset < platz) ? (frame_count - offset) : platz;
    memcpy(chunk_.data() + static_cast<size_t>(chunk_filled_frames_) * kChannels,
           frames + static_cast<size_t>(offset) * kChannels,
           static_cast<size_t>(nehmen) * kChannels * sizeof(int16_t));
    chunk_filled_frames_ += nehmen;
    offset += nehmen;
    if (chunk_filled_frames_ >= kChunkFrames) {
      if (source_.get()) {
        source_->CaptureFrame(chunk_.data(), 16, kSampleRate,
                              static_cast<size_t>(kChannels), kChunkFrames);
      }
      chunk_filled_frames_ = 0;
    }
  }
}

// ── Auf- und Abbau ───────────────────────────────────────────────────────────

bool PipewireLoopback::Start() {
  if (started_) return true;

  static const pw_registry_events registry_events = {
      PW_VERSION_REGISTRY_EVENTS,
      PipewireLoopback::OnRegistryGlobal,
      PipewireLoopback::OnRegistryGlobalRemove,
  };
  static const pw_stream_events stream_events = {
      PW_VERSION_STREAM_EVENTS,
      /*destroy=*/nullptr,
      // Zwischenstueck: PipeWire verlangt hier seinen eigenen Aufzaehlungstyp.
      // Der Header soll aber PipeWire-frei bleiben (er wird auch vom
      // gemeinsamen Plugin-Code eingebunden), deshalb die Umsetzung hier.
      [](void* data, enum pw_stream_state old_state, enum pw_stream_state state,
         const char* error) {
        PipewireLoopback::OnStreamStateChanged(data, static_cast<int>(old_state),
                                               static_cast<int>(state), error);
      },
      /*control_info=*/nullptr,
      /*io_changed=*/nullptr,
      /*param_changed=*/nullptr,
      /*add_buffer=*/nullptr,
      /*remove_buffer=*/nullptr,
      PipewireLoopback::OnStreamProcess,
  };

  pw_init(nullptr, nullptr);
  hooks_ = new Hooks();
  memset(hooks_, 0, sizeof(Hooks));

  loop_ = pw_thread_loop_new("honeycord-bildschirmton", nullptr);
  if (!loop_) {
    PwLogFromPlugin("Start: eigene Schleife liess sich nicht anlegen");
    Stop();
    return false;
  }
  pw_thread_loop_lock(loop_);

  context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (!context_) {
    PwLogFromPlugin("Start: kein PipeWire-Kontext");
    pw_thread_loop_unlock(loop_);
    Stop();
    return false;
  }
  core_ = pw_context_connect(context_, nullptr, 0);
  if (!core_) {
    // Haeufigster Fall: Flatpak ohne die Berechtigung xdg-run/pipewire-0.
    // Kein Grund zum Absturz — die Freigabe laeuft dann eben ohne Ton.
    PwLogFromPlugin(
        "Start: keine Verbindung zu PipeWire (Berechtigung xdg-run/pipewire-0?)");
    pw_thread_loop_unlock(loop_);
    Stop();
    return false;
  }

  pw_core_add_listener(core_, &hooks_->core, &kCoreEvents, this);

  registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
  if (registry_) {
    pw_registry_add_listener(registry_, &hooks_->registry, &registry_events, this);
  }

  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_NODE_NAME, "honeycord-bildschirmton",
      PW_KEY_NODE_DESCRIPTION, "HoneyCord Bildschirmton",
      // NICHT automatisch verbinden: sonst haengt PipeWire uns an die
      // Standard-Quelle (= Monitor inkl. unserer eigenen Wiedergabe) und der
      // Selbst-Ausschluss waere hinfaellig.
      PW_KEY_NODE_AUTOCONNECT, "false", nullptr);
  stream_ = pw_stream_new(core_, "HoneyCord Bildschirmton", props);
  if (!stream_) {
    PwLogFromPlugin("Start: Aufnehmer liess sich nicht anlegen");
    pw_thread_loop_unlock(loop_);
    Stop();
    return false;
  }
  pw_stream_add_listener(stream_, &hooks_->stream, &stream_events, this);

  uint8_t buffer[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  spa_audio_info_raw info = {};
  info.format = SPA_AUDIO_FORMAT_S16;
  info.rate = kSampleRate;
  info.channels = kChannels;
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;
  const spa_pod* params[1];
  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

  // Kein RT_PROCESS: der Rueckruf darf in der normalen Schleife laufen. In
  // einem Echtzeit-Faden waere der Sprung nach libwebrtc (Sperren/Allokation)
  // riskant, und 10-ms-Bloecke brauchen die Haerte nicht.
  int res = pw_stream_connect(
      stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
  if (res < 0) {
    PwLogFromPlugin("Start: Verbinden fehlgeschlagen (%s)", spa_strerror(res));
    pw_thread_loop_unlock(loop_);
    Stop();
    return false;
  }

  pw_thread_loop_unlock(loop_);
  if (pw_thread_loop_start(loop_) < 0) {
    PwLogFromPlugin("Start: Schleife liess sich nicht starten");
    Stop();
    return false;
  }

  started_ = true;
  PwLogFromPlugin("Start: laeuft (eigene pid %d, 48 kHz/16 Bit/stereo)", own_pid_);
  return true;
}

void PipewireLoopback::Stop() {
  if (loop_ && started_) pw_thread_loop_stop(loop_);
  started_ = false;

  if (loop_) pw_thread_loop_lock(loop_);
  for (auto& v : links_) {
    if (v.proxy) pw_proxy_destroy(v.proxy);
  }
  links_.clear();
  apps_.clear();
  fremde_ausgaenge_.clear();
  eigene_eingaenge_.clear();
  if (stream_) {
    pw_stream_destroy(stream_);
    stream_ = nullptr;
  }
  if (registry_) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
    registry_ = nullptr;
  }
  if (core_) {
    pw_core_disconnect(core_);
    core_ = nullptr;
  }
  if (loop_) pw_thread_loop_unlock(loop_);

  if (context_) {
    pw_context_destroy(context_);
    context_ = nullptr;
  }
  if (loop_) {
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
  delete hooks_;
  hooks_ = nullptr;
  own_node_id_ = 0;
  chunk_filled_frames_ = 0;
}

}  // namespace honeycord
