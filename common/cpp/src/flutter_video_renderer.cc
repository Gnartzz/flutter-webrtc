#include "flutter_video_renderer.h"

#ifdef _WINDOWS
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <dxgi.h>
#include <d3d11_4.h>
#ifdef _MSC_VER
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#endif
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
               int native, double fb_ms, double mark_ms, int phase, double lum,
               double nonblack) {
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
                   "fbconv=%.2f ms mark=%.1f ms phase=%d lum=%.1f nonblack=%.1f%%\n",
                   static_cast<long long>(tex), throttled, what, fps, w, h, native,
                   fb_ms, mark_ms, phase, lum, nonblack);
      std::fclose(f);
    }
  }
}

// Freie Textzeile nach render.log (Selfview-/Probe-Verdikte, Fehlerklassen).
void DiagLogA(const char* fmt, ...) {
  if (const char* base = std::getenv("LOCALAPPDATA")) {
    std::string path = std::string(base) + "\\HoneyCord\\render.log";
    if (FILE* f = std::fopen(path.c_str(), "a")) {
      va_list ap;
      va_start(ap, fmt);
      std::vfprintf(f, fmt, ap);
      va_end(ap);
      std::fputc('\n', f);
      std::fclose(f);
    }
  }
}

#pragma warning(pop)

// ===== EGL/ANGLE-Bootstrap =====
// EGL-Zugriff ohne Link-Abhaengigkeit: libEGL.dll (ANGLE) ist von
// flutter_windows.dll bereits im Prozess geladen. GetModuleHandleExW+PIN statt
// LoadLibrary: laedt NIE eine zweite Kopie ueber den Suchpfad (schlaegt sauber
// fehl, wenn die Engine kein ANGLE nutzt -> Fallback Direkt-Handle) und pinnt
// das Modul, damit die Funktionszeiger nie dangeln. Die EXT-Einstiege kommen —
// wie in Flutters engine/egl/manager.cc (InitializeDevice) — per
// eglGetProcAddress.
using EglDisplay = void*;
using EglDeviceEXT = void*;
using EglAttrib = intptr_t;
using EglBoolean = unsigned int;
using EglInt = int32_t;
constexpr EglInt kEGL_DEVICE_EXT = 0x322C;
constexpr EglInt kEGL_D3D11_DEVICE_ANGLE = 0x33A1;
constexpr EglBoolean kEGL_TRUE = 1;
typedef EglDisplay(__stdcall* PfnEglGetCurrentDisplay)(void);
typedef void*(__stdcall* PfnEglGetProcAddress)(const char*);
typedef EglBoolean(__stdcall* PfnEglQueryDisplayAttribEXT)(EglDisplay, EglInt,
                                                           EglAttrib*);
typedef EglBoolean(__stdcall* PfnEglQueryDeviceAttribEXT)(EglDeviceEXT, EglInt,
                                                          EglAttrib*);
struct EglAngleApi {
  PfnEglGetCurrentDisplay GetCurrentDisplay = nullptr;
  PfnEglQueryDisplayAttribEXT QueryDisplayAttribEXT = nullptr;
  PfnEglQueryDeviceAttribEXT QueryDeviceAttribEXT = nullptr;
  bool ok = false;
};
const EglAngleApi& GetEglAngleApi() {  // einmalig, thread-safe (magic static)
  static const EglAngleApi api = [] {
    EglAngleApi a;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"libEGL.dll",
                            &mod) ||
        !mod)
      return a;
    a.GetCurrentDisplay = reinterpret_cast<PfnEglGetCurrentDisplay>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "eglGetCurrentDisplay")));
    auto gpa = reinterpret_cast<PfnEglGetProcAddress>(
        reinterpret_cast<void*>(::GetProcAddress(mod, "eglGetProcAddress")));
    if (!a.GetCurrentDisplay || !gpa) return a;
    a.QueryDisplayAttribEXT = reinterpret_cast<PfnEglQueryDisplayAttribEXT>(
        gpa("eglQueryDisplayAttribEXT"));
    a.QueryDeviceAttribEXT = reinterpret_cast<PfnEglQueryDeviceAttribEXT>(
        gpa("eglQueryDeviceAttribEXT"));
    a.ok = a.QueryDisplayAttribEXT && a.QueryDeviceAttribEXT;
    return a;
  }();
  return api;
}
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

// Nicht-nativer Fallback (Kamera/SW-Remote) + Probe-P3: Frame KOHAERENT lesen
// (ConvertToARGB -> ToI420-Readback auf dem Capturer-/Decoder-eigenen Geraet)
// und per CPU-Write in fb_tex_ laden. kARGB ist der korrekte Typ: libyuv
// I420ToARGB schreibt Speicher-Bytes B,G,R,A = DXGI B8G8R8A8. (Vorher kBGRA =
// I420ToBGRA = Speicher A,R,G,B -> Alpha landete im Blaukanal = Blaustich.)
bool FlutterVideoRenderer::UploadFrameCpuToFallback(int w, int h) const {
  if (!EnsureFallbackTexture(w, h)) return false;
  auto _t_fb = std::chrono::steady_clock::now();
  frame_->ConvertToARGB(RTCVideoFrame::Type::kARGB, fb_cpu_.get(), 0, w, h);
  fb_ctx_->UpdateSubresource(fb_tex_.Get(), 0, nullptr, fb_cpu_.get(),
                             static_cast<UINT>(w) * 4, 0);
  fb_ctx_->Flush();
  dbg_fb_ms_ += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - _t_fb)
                    .count();
  dbg_fb_n_++;
  return true;
}

// Fehlerklasse einmalig protokollieren (Produktion: Feld-Diagnose, warum der
// Zero-Copy-Pfad auf einer Maschine in den Modus-0-Fallback ging).
void FlutterVideoRenderer::AngleLogOnce(unsigned bit, const char* what,
                                        long hr) const {
  if (angle_fail_logged_ & bit) return;
  angle_fail_logged_ |= bit;
  DiagLogA("[selfview] FAIL %s hr=0x%08lx", what, static_cast<unsigned long>(hr));
}

// ANGLEs EIGENES D3D11-Geraet per EGL erfragen (einmalig, Raster-Thread; exakt
// das Muster von Flutters engine/egl/manager.cc InitializeDevice). Der Zeiger
// ist ein BORROW -> ComPtr-Zuweisung AddRef't = eigene starke Referenz;
// Freigabe im Renderer-Destruktor (laeuft ueber den UnregisterTexture-Callback,
// solange Engine+ANGLE leben). NIE in Prozess-Statiken halten (Release nach
// ANGLE-Teardown beim CRT-Exit = Crash).
bool FlutterVideoRenderer::EnsureAngleDevice() const {
  if (angle_dev_) {
    if (GetCurrentThreadId() != angle_tid_) {
      AngleLogOnce(kAfWrongThread, "wrong_thread", 0);
      return false;
    }
    return true;
  }
  const EglAngleApi& api = GetEglAngleApi();
  if (!api.ok) {
    AngleLogOnce(kAfEglApi, "egl_api_missing", 0);
    return false;  // dauerhaft (kein ANGLE) -> Modus 0
  }
  EglDisplay dpy = api.GetCurrentDisplay();
  if (!dpy) return false;  // Kontext (noch) nicht current: KEIN Latch,
                           // naechster Composite versucht es erneut.
  EglAttrib egl_device = 0, d3d = 0;
  if (api.QueryDisplayAttribEXT(dpy, kEGL_DEVICE_EXT, &egl_device) !=
          kEGL_TRUE ||
      !egl_device) {
    AngleLogOnce(kAfQueryDisplay, "egl_query_display_fail", 0);
    return false;
  }
  if (api.QueryDeviceAttribEXT(reinterpret_cast<EglDeviceEXT>(egl_device),
                               kEGL_D3D11_DEVICE_ANGLE, &d3d) != kEGL_TRUE ||
      !d3d) {
    AngleLogOnce(kAfQueryDevice, "egl_query_device_fail", 0);
    return false;
  }
  angle_dev_ = reinterpret_cast<ID3D11Device*>(d3d);  // AddRef via ComPtr
  angle_dev_->GetImmediateContext(&angle_ctx_);
  // Unsere D3D-Calls laufen auf demselben Raster-Thread wie ANGLEs Rendering,
  // aber ANGLE kann sein Geraet auch von anderen Threads nutzen (Resource-
  // Kontext) -> Multithread-Schutz einschalten (gleiches Muster wie der
  // Capturer auf g_ctx_). QI-Fehler -> fail-closed (lieber Modus 0 als
  // ungeschuetzte Calls).
  {
    Microsoft::WRL::ComPtr<ID3D11Multithread> mt;
    if (FAILED(angle_ctx_.As(&mt))) {
      AngleLogOnce(kAfMultithread, "multithread_qi_fail", 0);
      angle_ctx_.Reset();
      angle_dev_.Reset();
      return false;
    }
    mt->SetMultithreadProtected(TRUE);
  }
  angle_tid_ = GetCurrentThreadId();
  LUID luid{};
  {
    Microsoft::WRL::ComPtr<IDXGIDevice> dx;
    Microsoft::WRL::ComPtr<IDXGIAdapter> ad;
    DXGI_ADAPTER_DESC de{};
    if (SUCCEEDED(angle_dev_.As(&dx)) && SUCCEEDED(dx->GetAdapter(&ad)) &&
        SUCCEEDED(ad->GetDesc(&de)))
      luid = de.AdapterLuid;
  }
  DiagLogA("[selfview] angle_dev ok luid=%08lx:%08lx fl=0x%x",
           static_cast<unsigned long>(luid.HighPart),
           static_cast<unsigned long>(luid.LowPart),
           static_cast<unsigned>(angle_dev_->GetFeatureLevel()));
  return true;
}

// Statische Ziel-Textur AUF ANGLEs Geraet (B8G8R8A8, legacy-SHARED — exakt das
// Muster, das die Engine in ihrem eigenen Unit-Test nutzt). Neuer Handle nur
// bei Groessenwechsel -> genau ein Engine-Re-Bind.
bool FlutterVideoRenderer::EnsureAngleDest(int w, int h) const {
  if (angle_dest_tex_ && angle_w_ == w && angle_h_ == h) return true;
  angle_dest_tex_.Reset();
  angle_dest_handle_ = nullptr;
  angle_copied_handle_ = nullptr;
  for (auto& s : angle_src_) {
    s.handle = nullptr;
    s.tex.Reset();
  }
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  HRESULT hr = angle_dev_->CreateTexture2D(&d, nullptr, &angle_dest_tex_);
  if (FAILED(hr)) {
    AngleLogOnce(kAfDestCreate, "dest_create_fail", hr);
    return false;
  }
  Microsoft::WRL::ComPtr<IDXGIResource> res;
  hr = angle_dest_tex_.As(&res);
  if (SUCCEEDED(hr)) hr = res->GetSharedHandle(&angle_dest_handle_);
  if (FAILED(hr) || !angle_dest_handle_) {
    AngleLogOnce(kAfDestHandle, "dest_handle_fail", hr);
    angle_dest_tex_.Reset();
    angle_dest_handle_ = nullptr;
    return false;
  }
  angle_w_ = w;
  angle_h_ = h;
  return true;
}

// Ring-Handle auf ANGLEs Geraet oeffnen (3-Slot-Cache; die D3D-Runtime dedupt
// Opens pro Geraet ohnehin -> Cache spart nur API-Overhead). Eviction rundum;
// bei Groessenwechsel raeumt EnsureAngleDest den Cache.
ID3D11Texture2D* FlutterVideoRenderer::AngleOpenSrc(void* handle) const {
  for (auto& s : angle_src_)
    if (s.handle == handle && s.tex) return s.tex.Get();
  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  HRESULT hr = angle_dev_->OpenSharedResource(
      reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Texture2D),
      reinterpret_cast<void**>(tex.GetAddressOf()));
  if (FAILED(hr) || !tex) {
    AngleLogOnce(kAfSrcOpen, "src_open_fail", hr);
    return nullptr;
  }
  AngleSrc& slot = angle_src_[angle_src_evict_];
  angle_src_evict_ = (angle_src_evict_ + 1) % 3;
  slot.handle = handle;
  slot.tex = tex;
  return slot.tex.Get();
}

// Self-View-Zero-Copy AUF ANGLEs EIGENEM Geraet: rotierendes Capturer-Handle
// dort oeffnen (ANGLEs eigener Modus-0-Import beweist, dass Lesen der Ring-
// Texturen AUF DIESEM Geraet funktioniert), CopyResource in die statische
// Ziel-Textur, deren Handle an ANGLE (Bind-once statt Re-Bind pro Frame =
// der ~20-fps-Engpass). Same-Device-Write -> ANGLE sampelt live. ~0 CPU,
// 1 GPU-Copy. Jeder Fehler -> false -> Direkt-Handle (sichtbar, heutiges
// Verhalten). Die 3 frueheren Schwarz-Fehlschlaege (2.6.7/9/10) lasen alle
// auf FREMDEN Geraeten (fb_dev_/copy_dev_) — genau das vermeidet dieser Pfad.
bool FlutterVideoRenderer::CopySelfViewAngleDevice(int w, int h,
                                                   void* handle) const {
  if (!EnsureAngleDevice() || !EnsureAngleDest(w, h)) return false;
  if (handle == angle_copied_handle_)
    return true;  // gleicher Frame (Ring rotiert pro Frame -> Handle-Wechsel
                  // == neuer Frame); Composite ohne neuen Frame kopiert nicht.
  ID3D11Texture2D* src = AngleOpenSrc(handle);
  if (!src) return false;
  D3D11_TEXTURE2D_DESC sd{};
  src->GetDesc(&sd);
  if (sd.Width == static_cast<UINT>(w) && sd.Height == static_cast<UINT>(h)) {
    angle_ctx_->CopyResource(angle_dest_tex_.Get(), src);
  } else {
    AngleLogOnce(kAfDescMismatch, "src_desc_mismatch", 0);
    D3D11_BOX box{0, 0, 0,
                  sd.Width < static_cast<UINT>(w) ? sd.Width
                                                  : static_cast<UINT>(w),
                  sd.Height < static_cast<UINT>(h) ? sd.Height
                                                   : static_cast<UINT>(h),
                  1};
    angle_ctx_->CopySubresourceRegion(angle_dest_tex_.Get(), 0, 0, 0, 0, src, 0,
                                      &box);
  }
  angle_ctx_->Flush();
  angle_copied_handle_ = handle;
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
  if (handle && is_throttle_) {
#ifdef HONEYCORD_SELFVIEW_PROBE
    // === PROBE #2 (Re-Bind-Drossel): 3 Phasen je 20 s, Endloszyklus 0->1->2.
    // ALLE Phasen nutzen ausschliesslich den bewaehrten Mode-0-Import
    // (frisches eglBindTexImage pro Handle-WECHSEL) -> schwarz ist
    // konstruktionsbedingt unmoeglich; nur die Wechsel-RATE variiert.
    // Gemessen wird die Compositor-Entlastung (COMPOSITE-fps; lum-Spalte =
    // Re-Binds im 2-s-Fenster). Erkenntnis-Hintergrund: einmal gebundene
    // Texturen werden durch KEINE Cross-Device-Aenderung aktualisiert
    // (Gate-B-Schwarz + Capturer-Beige-Freeze 832b8f2) -> nur die Import-
    // Frequenz ist verhandelbar, nicht der Mechanismus.
    const int64_t nowm = NowMs();
    if (!probe_start_ms_) probe_start_ms_ = nowm;
    const int phase =
        static_cast<int>(((nowm - probe_start_ms_) / kProbeDwellMs) % 3);
    if (phase != probe_phase_) {
      DiagLogA("[probe2] phase %d -> %d (t=%llds)", probe_phase_, phase,
               static_cast<long long>((nowm - probe_start_ms_) / 1000));
      probe_phase_ = phase;
      probe_held_ = nullptr;  // Phasenstart: sofort frisch binden
    }
    // Handle-Historie (neuester zuerst) pflegen — fuer den AGED-Pick.
    if (handle != probe_hist_[0].handle) {
      probe_hist_[2] = probe_hist_[1];
      probe_hist_[1] = probe_hist_[0];
      probe_hist_[0] = {handle, nowm};
    }
    switch (phase) {
      case 0:  // Baseline: jeder neue Frame = neuer Handle = Re-Bind (heute)
        probe_rebinds_win_++;  // obere Schranke (zaehlt Composites mit
                               // potenziellem Bind; Baseline-Vergleichswert)
        break;
      case 1:  // HOLD 66 ms: Handle festhalten -> <=15 Re-Binds/s
        if (!probe_held_ || nowm - probe_held_since_ >= 66) {
          probe_held_ = handle;
          probe_held_since_ = nowm;
          probe_rebinds_win_++;
        }
        handle = probe_held_;
        break;
      case 2:  // AGED+HOLD 40 ms: 2.-neuesten Handle binden (Import wartet
               // nicht auf den frisch beschriebenen Slot) -> <=25 Re-Binds/s
        if (!probe_held_ || nowm - probe_held_since_ >= 40) {
          probe_held_ =
              probe_hist_[1].handle ? probe_hist_[1].handle : handle;
          probe_held_since_ = nowm;
          probe_rebinds_win_++;
        }
        handle = probe_held_;
        break;
    }
#else
    // Produktion (nach Probe #2 festzuziehen): Re-Bind-Drossel.
    if (CopySelfViewAngleDevice(w, h, handle)) handle = angle_dest_handle_;
#endif
  }
  if (!handle) {
    // Nicht-nativer Frame (Kamera/SW-Remote, I420) -> kohaerenter CPU-Weg
    // (kARGB, korrekte Farben) in die statische fb_tex_.
    if (!UploadFrameCpuToFallback(w, h)) return nullptr;
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
#ifdef HONEYCORD_SELFVIEW_PROBE
      // lum-Spalte = Re-Binds im 2-s-Fenster (Probe #2), nonblack ungenutzt.
      RenderLog(texture_id_, is_throttle_ ? 1 : 0, "COMPOSITE",
                dbg_ob_calls_ * 1000.0 / (now - dbg_ob_last_ms_), w, h,
                dbg_native_, fbavg, -1.0, probe_phase_,
                static_cast<double>(probe_rebinds_win_), -1.0);
      probe_rebinds_win_ = 0;
#else
      RenderLog(texture_id_, is_throttle_ ? 1 : 0, "COMPOSITE",
                dbg_ob_calls_ * 1000.0 / (now - dbg_ob_last_ms_), w, h,
                dbg_native_, fbavg, -1.0, -1, -1.0, -1.0);
#endif
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
                frame->native_shared_handle() ? 1 : 0, -1.0, markavg, -1, -1.0,
                -1.0);
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
  // Self-View-Drossel auf ~25 fps (40 ms): jeder MarkTextureFrameAvailable
  // triggert ein Flutter-Fenster-Composite (ANGLE, teuer bei grossen Fenstern).
  // NUR fuer die eigene Bildschirm-Selbstansicht (is_throttle_); Remote-Kacheln
  // nutzen denselben GpuSurface-Pfad, sollen aber mit voller FPS laufen ->
  // nicht drosseln. frame_ ist bereits aktualisiert -> der naechste Mark zeigt
  // das neueste Bild (max. 40 ms Verzug, unmerklich). Sende-Pfad unberuehrt.
  if (is_gpu_surface_ && is_throttle_) {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    if (now - last_mark_ms_ < 40) return;
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
    bool use_gpu_surface, bool throttle,
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
