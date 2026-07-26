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
};

PipewireLoopback::PipewireLoopback(
    libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source)
    : source_(source) {
  chunk_.resize(static_cast<size_t>(kChunkFrames) * kChannels, 0);
  own_pid_ = static_cast<int>(getpid());
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
  if (!type || strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;
  self->HandleNode(id, props);
}

void PipewireLoopback::OnRegistryGlobalRemove(void* data, uint32_t id) {
  auto* self = static_cast<PipewireLoopback*>(data);
  auto it = self->links_.find(id);
  if (it != self->links_.end()) {
    if (it->second) pw_proxy_destroy(it->second);
    self->links_.erase(it);
    PwLogFromPlugin("Knoten %u verschwunden -> Verknuepfung entfernt", id);
  }
  for (auto a = self->apps_.begin(); a != self->apps_.end(); ++a) {
    if (*a == id) {
      self->apps_.erase(a);
      break;
    }
  }
}

void PipewireLoopback::HandleNode(uint32_t id, const struct spa_dict* props) {
  const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!media_class || strcmp(media_class, kAppPlaybackClass) != 0) return;

  // SELBST-AUSSCHLUSS (der eigentliche Zweck der ganzen Konstruktion):
  // Unsere eigene Wiedergabe — also die Stimmen der Zuhoerer — darf NICHT
  // eingesammelt werden, sonst kommt sie bei ihnen zurueck. Auf Windows macht
  // das EXCLUDE_TARGET_PROCESS_TREE, auf dem Mac excludesCurrentProcessAudio.
  const char* pid = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
  if (pid && atoi(pid) == own_pid_) {
    PwLogFromPlugin("Knoten %u ist unsere eigene Wiedergabe (pid %s) -> uebersprungen",
                    id, pid);
    return;
  }

  const char* name = spa_dict_lookup(props, PW_KEY_APP_NAME);
  PwLogFromPlugin("Fremder Wiedergabe-Knoten %u (%s) gefunden", id,
                  name ? name : "ohne Namen");
  for (uint32_t known : apps_) {
    if (known == id) return;
  }
  apps_.push_back(id);
  LinkNode(id);
}

void PipewireLoopback::LinkNode(uint32_t node_id) {
  // Der eigene Aufnehmer hat erst nach dem Verbinden eine Knoten-ID. Kommt ein
  // fremder Knoten frueher, wird er in apps_ gemerkt und spaeter verknuepft.
  if (own_node_id_ == 0 || !core_) return;
  if (links_.find(node_id) != links_.end()) return;

  // GEMESSEN: Verknuepfen auf KNOTEN-Ebene genuegt — die Anschluesse ordnet
  // PipeWire selbst zu, und mehrere Quellen mischt es am Eingang zusammen.
  // Deshalb keine Anschluss-Buchfuehrung (spart ~150 Zeilen und Fehlerquellen).
  pw_properties* p = pw_properties_new(nullptr, nullptr);
  pw_properties_setf(p, PW_KEY_LINK_OUTPUT_NODE, "%u", node_id);
  pw_properties_setf(p, PW_KEY_LINK_INPUT_NODE, "%u", own_node_id_);
  auto* link = static_cast<pw_proxy*>(
      pw_core_create_object(core_, "link-factory", PW_TYPE_INTERFACE_Link,
                            PW_VERSION_LINK, &p->dict, 0));
  pw_properties_free(p);
  if (link) {
    links_[node_id] = link;
    PwLogFromPlugin("Knoten %u -> Aufnehmer %u verknuepft", node_id, own_node_id_);
  } else {
    PwLogFromPlugin("Knoten %u konnte NICHT verknuepft werden", node_id);
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
      for (uint32_t app : self->apps_) self->LinkNode(app);
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
  for (auto& kv : links_) {
    if (kv.second) pw_proxy_destroy(kv.second);
  }
  links_.clear();
  apps_.clear();
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
