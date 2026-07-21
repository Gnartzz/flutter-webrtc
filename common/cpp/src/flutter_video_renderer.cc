#include "flutter_video_renderer.h"

#ifdef _WINDOWS
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#endif

namespace flutter_webrtc_plugin {

#ifdef _WINDOWS
namespace {
inline int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
// Log-Rotation (wie honeycord_wasapi_loopback.cc): > 1 MB -> <path>.old
// (ersetzt Vorgaenger), Archiv aelter 14 Tage -> geloescht. Einmal pro Prozess.
void RotateLogIfNeededA(const char* path) {
  char old_path[MAX_PATH + 8];
  std::snprintf(old_path, sizeof(old_path), "%s.old", path);
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExA(old_path, GetFileExInfoStandard, &fad)) {
    FILETIME now_ft;
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now_u{}, old_u{};
    now_u.LowPart = now_ft.dwLowDateTime;
    now_u.HighPart = now_ft.dwHighDateTime;
    old_u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    old_u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    const unsigned long long k14Days100ns = 14ULL * 24 * 3600 * 10000000ULL;
    if (now_u.QuadPart > old_u.QuadPart &&
        now_u.QuadPart - old_u.QuadPart > k14Days100ns) {
      DeleteFileA(old_path);
    }
  }
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
    const unsigned long long size =
        (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
        fad.nFileSizeLow;
    if (size > 1024ULL * 1024ULL) {
      MoveFileExA(path, old_path, MOVEFILE_REPLACE_EXISTING);
    }
  }
}

// Diagnose: Render-fps pro Kachel nach %LOCALAPPDATA%\HoneyCord\render.log.
// getenv/fopen loesen unter MSVC C4996 aus (bei /WX = Fehler) -> nur hier unterdruecken.
#pragma warning(push)
#pragma warning(disable : 4996)
void RenderLog(int64_t tex, int throttled, const char* what, double fps, int w, int h,
               int native, double fb_ms, double mark_ms) {
  if (const char* base = std::getenv("LOCALAPPDATA")) {
    std::string path = std::string(base) + "\\HoneyCord\\render.log";
    static const bool rotated = [&path] {
      RotateLogIfNeededA(path.c_str());
      return true;
    }();
    (void)rotated;
    if (FILE* f = std::fopen(path.c_str(), "a")) {
      std::fprintf(f,
                   "[render] tex=%lld throttled=%d %s=%.1f fps %dx%d native=%d "
                   "fbconv=%.2f ms mark=%.1f ms\n",
                   static_cast<long long>(tex), throttled, what, fps, w, h, native,
                   fb_ms, mark_ms);
      std::fclose(f);
    }
  }
}
#pragma warning(pop)
}  // namespace
#endif

FlutterVideoRenderer::~FlutterVideoRenderer() {
  // Sicherheitsnetz fuer jeden Zerstoerungspfad ohne vorheriges Dispose
  // (z. B. Plugin-Teardown beim App-Ende): aus dem Sink des Tracks austragen.
  // RemoveRenderer blockiert auf dem Adapter-Mutex, bis ein laufendes
  // OnFrame fertig ist -> danach kann kein Frame mehr hierher gelangen.
  // Ohne das ruft der Capture-Thread in ein halb-zerstoertes Objekt
  // (purecall -> abort).
  SetVideoTrack(nullptr);
}

void FlutterVideoRenderer::initialize(
    TextureRegistrar* registrar,
    BinaryMessenger* messenger,
    TaskRunner* task_runner,
    std::unique_ptr<flutter::TextureVariant> texture,
    int64_t trxture_id) {
  registrar_ = registrar;
  texture_ = std::move(texture);
  texture_id_ = trxture_id;
  std::string channel_name =
      "FlutterWebRTC/Texture" + std::to_string(texture_id_);
  event_channel_ = EventChannelProxy::Create(messenger, task_runner, channel_name);
}

const FlutterDesktopPixelBuffer* FlutterVideoRenderer::CopyPixelBuffer(
    size_t width,
    size_t height) const {
  mutex_.lock();
  if (pixel_buffer_.get() && frame_.get()) {
    if (pixel_buffer_->width != frame_->width() ||
        pixel_buffer_->height != frame_->height()) {
      size_t buffer_size =
          (size_t(frame_->width()) * size_t(frame_->height())) * (32 >> 3);
      rgb_buffer_.reset(new uint8_t[buffer_size]);
      pixel_buffer_->width = frame_->width();
      pixel_buffer_->height = frame_->height();
    }

    frame_->ConvertToARGB(RTCVideoFrame::Type::kABGR, rgb_buffer_.get(), 0,
                          static_cast<int>(pixel_buffer_->width),
                          static_cast<int>(pixel_buffer_->height));

    pixel_buffer_->buffer = rgb_buffer_.get();
    mutex_.unlock();
    return pixel_buffer_.get();
  }
  mutex_.unlock();
  return nullptr;
}

#ifdef _WINDOWS
// Fallback-Textur fuer nicht-native Frames (Kamera/Remote): eigene plain-SHARED
// BGRA-Textur auf dem Default-Adapter (Adapter 0; ANGLE laeuft auf demselben,
// daher oeffnet das Legacy-Shared-Handle). Kein Keyed-Mutex.
bool FlutterVideoRenderer::EnsureFallbackTexture(int w, int h) const {
  using Microsoft::WRL::ComPtr;
  if (!fb_dev_) {
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION, &fb_dev_, &fl,
                                 &fb_ctx_)))
      return false;
  }
  if (fb_tex_ && fb_w_ == w && fb_h_ == h) return true;
  fb_tex_.Reset();
  fb_handle_ = nullptr;
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  if (FAILED(fb_dev_->CreateTexture2D(&d, nullptr, &fb_tex_))) return false;
  ComPtr<IDXGIResource> res;
  if (FAILED(fb_tex_.As(&res)) ||
      FAILED(res->GetSharedHandle(&fb_handle_)) || !fb_handle_) {
    fb_tex_.Reset();
    fb_handle_ = nullptr;
    return false;
  }
  fb_w_ = w;
  fb_h_ = h;
  fb_cpu_.reset(new uint8_t[static_cast<size_t>(w) * h * 4]);
  return true;
}

const FlutterDesktopGpuSurfaceDescriptor* FlutterVideoRenderer::ObtainGpuSurface(
    size_t width, size_t height) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!frame_.get()) return nullptr;
  const int w = frame_->width();
  const int h = frame_->height();
  if (w <= 0 || h <= 0) return nullptr;

  void* handle = frame_->native_shared_handle();
  dbg_native_ = handle ? 1 : 0;
  if (!handle) {
    // Nicht-nativer Frame (Kamera/Remote, I420): nach BGRA konvertieren und in
    // die eigene Shared-Textur hochladen. Producer (Upload) + Consumer (ANGLE)
    // laufen beide sequenziell auf dem Raster-Thread -> kein Keyed-Mutex noetig.
    if (!EnsureFallbackTexture(w, h)) return nullptr;
    auto _t_fb = std::chrono::steady_clock::now();
    frame_->ConvertToARGB(RTCVideoFrame::Type::kBGRA, fb_cpu_.get(), 0, w, h);
    fb_ctx_->UpdateSubresource(fb_tex_.Get(), 0, nullptr, fb_cpu_.get(),
                               static_cast<UINT>(w) * 4, 0);
    fb_ctx_->Flush();
    dbg_fb_ms_ += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - _t_fb)
                      .count();
    dbg_fb_n_++;
    handle = fb_handle_;
  }

  gpu_descriptor_.struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
  gpu_descriptor_.handle = handle;
  gpu_descriptor_.width = gpu_descriptor_.visible_width = static_cast<size_t>(w);
  gpu_descriptor_.height = gpu_descriptor_.visible_height = static_cast<size_t>(h);
  gpu_descriptor_.format = kFlutterDesktopPixelFormatBGRA8888;
  gpu_descriptor_.release_callback = nullptr;
  gpu_descriptor_.release_context = nullptr;
  {
    int64_t now = NowMs();
    if (dbg_ob_last_ms_ == 0) dbg_ob_last_ms_ = now;
    dbg_ob_calls_++;
    if (now - dbg_ob_last_ms_ >= 2000) {
      double fbavg = dbg_fb_n_ ? dbg_fb_ms_ / dbg_fb_n_ : 0.0;
      RenderLog(texture_id_, is_throttle_ ? 1 : 0, "COMPOSITE",
                dbg_ob_calls_ * 1000.0 / (now - dbg_ob_last_ms_), w, h,
                dbg_native_, fbavg, -1.0);
      dbg_ob_calls_ = 0;
      dbg_ob_last_ms_ = now;
      dbg_fb_ms_ = 0.0;
      dbg_fb_n_ = 0;
    }
  }
  return &gpu_descriptor_;
}
#endif

void FlutterVideoRenderer::OnFrame(scoped_refptr<RTCVideoFrame> frame) {
#ifdef _WINDOWS
  {
    int64_t now = NowMs();
    if (dbg_of_last_ms_ == 0) dbg_of_last_ms_ = now;
    dbg_of_calls_++;
    if (now - dbg_of_last_ms_ >= 2000) {
      double markavg = dbg_mark_n_ ? dbg_mark_ms_ / dbg_mark_n_ : 0.0;
      RenderLog(texture_id_, is_throttle_ ? 1 : 0, "ONFRAME",
                dbg_of_calls_ * 1000.0 / (now - dbg_of_last_ms_),
                frame->width(), frame->height(),
                frame->native_shared_handle() ? 1 : 0, -1.0, markavg);
      dbg_of_calls_ = 0;
      dbg_of_last_ms_ = now;
      dbg_mark_ms_ = 0.0;
      dbg_mark_n_ = 0;
    }
  }
#endif
  if (!first_frame_rendered) {
    EncodableMap params;
    params[EncodableValue("event")] = "didFirstFrameRendered";
    params[EncodableValue("id")] = EncodableValue(texture_id_);
    event_channel_->Success(EncodableValue(params));
    pixel_buffer_.reset(new FlutterDesktopPixelBuffer());
    pixel_buffer_->width = 0;
    pixel_buffer_->height = 0;
    first_frame_rendered = true;
  }
  if (rotation_ != frame->rotation()) {
    EncodableMap params;
    params[EncodableValue("event")] = "didTextureChangeRotation";
    params[EncodableValue("id")] = EncodableValue(texture_id_);
    params[EncodableValue("rotation")] =
        EncodableValue((int32_t)frame->rotation());
    event_channel_->Success(EncodableValue(params));
    rotation_ = frame->rotation();
  }
  if (last_frame_size_.width != frame->width() ||
      last_frame_size_.height != frame->height()) {
    EncodableMap params;
    params[EncodableValue("event")] = "didTextureChangeVideoSize";
    params[EncodableValue("id")] = EncodableValue(texture_id_);
    params[EncodableValue("width")] = EncodableValue((int32_t)frame->width());
    params[EncodableValue("height")] = EncodableValue((int32_t)frame->height());
    event_channel_->Success(EncodableValue(params));

    last_frame_size_ = {(size_t)frame->width(), (size_t)frame->height()};
  }
  mutex_.lock();
  frame_ = frame;
  mutex_.unlock();
#ifdef _WINDOWS
  // Self-View-Drossel: jeder MarkTextureFrameAvailable triggert ein Flutter-
  // Fenster-Composite (ANGLE, teuer bei grossen Fenstern). NUR fuer die eigene
  // Bildschirm-Selbstansicht (is_throttle_); Remote-Kacheln laufen mit voller
  // FPS. Intervall per User-Schalter „Vorschau-Fluessigkeit" (throttle_ms_):
  // 40 = Sparsam (~20 fps), 25 = Normal (~30 fps), 0 = Fluessig (ungedrosselt,
  // mehr GPU/Compositor-Last). frame_ ist bereits aktualisiert -> der naechste
  // Mark zeigt das neueste Bild. Sende-Pfad unberuehrt.
  if (is_gpu_surface_ && is_throttle_ && throttle_ms_ > 0) {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    if (now - last_mark_ms_ < throttle_ms_) return;
    last_mark_ms_ = now;
  }
  auto _t_mark = std::chrono::steady_clock::now();
  registrar_->MarkTextureFrameAvailable(texture_id_);
  dbg_mark_ms_ += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - _t_mark)
                      .count();
  dbg_mark_n_++;
  return;
#endif
  registrar_->MarkTextureFrameAvailable(texture_id_);
}

void FlutterVideoRenderer::SetVideoTrack(scoped_refptr<RTCVideoTrack> track) {
  if (track_ != track) {
    if (track_)
      track_->RemoveRenderer(this);
    track_ = track;
    last_frame_size_ = {0, 0};
    first_frame_rendered = false;
    if (track_)
      track_->AddRenderer(this);
  }
}

bool FlutterVideoRenderer::CheckMediaStream(std::string mediaId) {
  if (0 == mediaId.size() || 0 == media_stream_id.size()) {
    return false;
  }
  return mediaId == media_stream_id;
}

bool FlutterVideoRenderer::CheckVideoTrack(std::string mediaId) {
  if (0 == mediaId.size() || !track_) {
    return false;
  }
  return mediaId == track_->id().std_string();
}

FlutterVideoRendererManager::FlutterVideoRendererManager(
    FlutterWebRTCBase* base)
    : base_(base) {}

void FlutterVideoRendererManager::CreateVideoRendererTexture(
    bool use_gpu_surface, bool throttle, int throttle_ms,
    std::unique_ptr<MethodResultProxy> result) {
  auto texture = new RefCountedObject<FlutterVideoRenderer>();
  // Default: PixelBuffer (Kamera/Remote) -- bewaehrt, fasst nichts an. NUR wenn
  // die App explizit gpuSurface=true setzt (chirurgisch: lokale Bildschirm-
  // Selbst-Kachel auf Windows) wird eine GpuSurfaceTexture (Zero-Copy via
  // DXGI-Shared-Handle) registriert. So trifft ein Fehler dort NIE Kamera/
  // Remote/UI.
  std::unique_ptr<flutter::TextureVariant> textureVariant;
#ifdef _WINDOWS
  if (use_gpu_surface) {
    texture->set_gpu_surface(true);
    texture->set_throttle(throttle);  // nur Self-View drosselt (OnFrame)
    texture->set_throttle_ms(throttle_ms);  // Intervall (0=aus, s. User-Schalter)
    textureVariant = std::make_unique<flutter::TextureVariant>(
        flutter::GpuSurfaceTexture(
            kFlutterDesktopGpuSurfaceTypeDxgiSharedHandle,
            [texture](size_t width, size_t height)
                -> const FlutterDesktopGpuSurfaceDescriptor* {
              return texture->ObtainGpuSurface(width, height);
            }));
  }
#endif
  if (!textureVariant) {
    textureVariant =
        std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
            [texture](size_t width,
                      size_t height) -> const FlutterDesktopPixelBuffer* {
              return texture->CopyPixelBuffer(width, height);
            }));
  }

  auto texture_id = base_->textures_->RegisterTexture(textureVariant.get());
  texture->initialize(base_->textures_, base_->messenger_, base_->task_runner_,
                      std::move(textureVariant), texture_id);
  renderers_[texture_id] = texture;
  EncodableMap params;
  params[EncodableValue("textureId")] = EncodableValue(texture_id);
  result->Success(EncodableValue(params));
}

void FlutterVideoRendererManager::VideoRendererSetSrcObject(
    int64_t texture_id,
    const std::string& stream_id,
    const std::string& owner_tag,
    const std::string& track_id) {
  scoped_refptr<RTCMediaStream> stream =
      base_->MediaStreamForId(stream_id, owner_tag);

  auto it = renderers_.find(texture_id);
  if (it != renderers_.end()) {
    FlutterVideoRenderer* renderer = it->second.get();
    if (stream.get()) {
      auto video_tracks = stream->video_tracks();
      if (video_tracks.size() > 0) {
        if (track_id == std::string()) {
          renderer->SetVideoTrack(video_tracks[0]);
        } else {
          for (auto track : video_tracks.std_vector()) {
            if (track->id().std_string() == track_id) {
              renderer->SetVideoTrack(track);
              break;
            }
          }
        }
        renderer->media_stream_id = stream_id;
      }
    } else {
      renderer->SetVideoTrack(nullptr);
    }
  }
}

void FlutterVideoRendererManager::VideoRendererDispose(
    int64_t texture_id,
    std::unique_ptr<MethodResultProxy> result) {
  auto it = renderers_.find(texture_id);
  if (it != renderers_.end()) {
    scoped_refptr<FlutterVideoRenderer> renderer = it->second;
    renderer->SetVideoTrack(nullptr);
    // Sofort aus der Map: ein spaetes setSrcObject (Attach-Race beim
    // Kachel-Umbau, z. B. wenn die Share-Kachel erscheint) darf den Renderer
    // nicht wiederfinden und erneut an einen Track haengen — sonst wird er
    // spaeter registriert zerstoert (purecall im Capture-Thread). Nebenbei:
    // Doppel-Dispose kann so nicht mehr zweimal denselben Iterator erasen.
    renderers_.erase(it);
#if defined(_WINDOWS)
    // Renderer lebt weiter, bis die Engine die Textur wirklich freigegeben
    // hat (der Callback haelt die letzte Referenz).
    base_->textures_->UnregisterTexture(texture_id, [renderer] {});
#else
    base_->textures_->UnregisterTexture(texture_id);
#endif
    result->Success();
    return;
  }
  result->Error("VideoRendererDisposeFailed",
                "VideoRendererDispose() texture not found!");
}

}  // namespace flutter_webrtc_plugin
