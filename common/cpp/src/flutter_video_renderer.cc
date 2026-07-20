#include "flutter_video_renderer.h"

#ifdef _WINDOWS
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <dxgi.h>
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
               int native, double fb_ms, double mark_ms, int mode, double lum,
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
                   "fbconv=%.2f ms mark=%.1f ms mode=%d lum=%.1f nonblack=%.1f%%\n",
                   static_cast<long long>(tex), throttled, what, fps, w, h, native,
                   fb_ms, mark_ms, mode, lum, nonblack);
      std::fclose(f);
    }
  }
}

// DIAGNOSE: freie Textzeile nach render.log (MODE1-Adapter/Sync-Verdikte).
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

// DIAGNOSE: kleines Grid abtasten -> Durchschnitts-Luminanz (0..255) +
// Nicht-Schwarz-Anteil (%). O(4096) unabhaengig von der Aufloesung. BGRA-Order.
void SampleLuminance(const uint8_t* bgra, int w, int h, double* lum,
                     double* nonblack) {
  const int N = 64;
  double sum = 0.0;
  int nb = 0, cnt = 0;
  for (int gy = 0; gy < N; ++gy) {
    int y = (h * gy) / N;
    if (y >= h) y = h - 1;
    for (int gx = 0; gx < N; ++gx) {
      int x = (w * gx) / N;
      if (x >= w) x = w - 1;
      const uint8_t* p = bgra + (static_cast<size_t>(y) * w + x) * 4;
      double L = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
      sum += L;
      if (L > 8.0) ++nb;
      ++cnt;
    }
  }
  *lum = cnt ? sum / cnt : 0.0;
  *nonblack = cnt ? 100.0 * nb / cnt : 0.0;
}

// DIAGNOSE: 32-bit top-down BI_RGB BMP (DXGI-BGRA == BMP-Byte-Order -> memcpy).
void DumpBmp(const char* path, const uint8_t* bgra, int w, int h) {
#pragma pack(push, 1)
  struct FileHdr {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t r1, r2;
    uint32_t bfOffBits;
  } fh;
  struct InfoHdr {
    uint32_t biSize;
    int32_t biW, biH;
    uint16_t biPlanes, biBpp;
    uint32_t biComp, biImg;
    int32_t biX, biY;
    uint32_t biClr, biImp;
  } ih;
#pragma pack(pop)
  const uint32_t data = static_cast<uint32_t>(w) * static_cast<uint32_t>(h) * 4u;
  fh.bfType = 0x4D42;  // 'BM'
  fh.bfSize = 54u + data;
  fh.r1 = fh.r2 = 0;
  fh.bfOffBits = 54u;
  ih.biSize = 40u;
  ih.biW = w;
  ih.biH = -h;  // top-down
  ih.biPlanes = 1;
  ih.biBpp = 32;
  ih.biComp = 0;  // BI_RGB
  ih.biImg = data;
  ih.biX = ih.biY = 0;
  ih.biClr = ih.biImp = 0;
  if (FILE* f = std::fopen(path, "wb")) {
    std::fwrite(&fh, 1, 14, f);
    std::fwrite(&ih, 1, 40, f);
    std::fwrite(bgra, 1, data, f);
    std::fclose(f);
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

// Self-View-Fix: das per Handle referenzierte Capturer-Bild auf der GPU in
// fb_tex_ (ANGLEs Adapter) kopieren, statt den fremden Handle direkt an ANGLE zu
// geben. Handle gecacht (nur bei Wechsel neu oeffnen). Kein Keyed-Mutex -> in der
// VORSCHAU minimales Tearing moeglich; der Sende-Pfad ist davon unberuehrt.
bool FlutterVideoRenderer::CopyNativeToOwn(void* handle, int w, int h) const {
  if (!EnsureFallbackTexture(w, h)) return false;
  if (handle != own_last_handle_ || !own_src_tex_) {
    own_src_tex_.Reset();
    if (FAILED(fb_dev_->OpenSharedResource(
            reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(own_src_tex_.GetAddressOf())))) {
      own_last_handle_ = nullptr;
      return false;  // Open fehlgeschlagen -> Aufrufer nutzt den Direkt-Handle
    }
    own_last_handle_ = handle;
  }
  fb_ctx_->CopyResource(fb_tex_.Get(), own_src_tex_.Get());
  fb_ctx_->Flush();
  return true;
}

// Modus 2 / regulaerer Fallback: nativen (oder I420-) Frame per Capturer-eigenem
// Readback nach BGRA holen und in fb_tex_ (ANGLEs Adapter) hochladen. Der Readback
// (ConvertToARGB->ToI420) laeuft auf dem Capturer-Device g_dev_ = kohaerent,
// adapter-unabhaengig, immer korrekt (= bewaehrter Remote-Pfad).
bool FlutterVideoRenderer::ConvertNativeCpu(int w, int h) const {
  if (!EnsureFallbackTexture(w, h)) return false;
  auto _t_fb = std::chrono::steady_clock::now();
  frame_->ConvertToARGB(RTCVideoFrame::Type::kBGRA, fb_cpu_.get(), 0, w, h);
  fb_ctx_->UpdateSubresource(fb_tex_.Get(), 0, nullptr, fb_cpu_.get(),
                             static_cast<UINT>(w) * 4, 0);
  fb_ctx_->Flush();
  dbg_fb_ms_ += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - _t_fb)
                    .count();
  dbg_fb_n_++;
  return true;
}

// Modus 1: Kopiergeraet auf dem NVIDIA-Adapter des Capturers anlegen (dessen
// Enumeration spiegeln), damit OpenSharedResource des Capturer-Legacy-Handles
// same-adapter (= gueltig) ist. LUID gegen den Default-Adapter (= ANGLE) pruefen:
// nur wenn gleich, kann ANGLE das Ziel-Handle spaeter oeffnen.
bool FlutterVideoRenderer::EnsureCopyDevice() const {
  using Microsoft::WRL::ComPtr;
  if (copy_dev_) return true;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(
          __uuidof(IDXGIFactory1),
          reinterpret_cast<void**>(factory.GetAddressOf())))) {
    diag_mode1_state_ = 3;
    return false;
  }
  ComPtr<IDXGIAdapter1> nvidia;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC1 d{};
    if (SUCCEEDED(a->GetDesc1(&d)) && d.VendorId == 0x10DE) {
      nvidia = a;
      break;
    }
  }
  D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDevice(
      nvidia.Get(), nvidia ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
      nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &copy_dev_, &fl, &copy_ctx_);
  if (FAILED(hr)) {
    diag_mode1_state_ = 3;
    copy_dev_.Reset();
    copy_ctx_.Reset();
    DiagLogA("MODE1 create_fail hr=0x%08lx", static_cast<unsigned long>(hr));
    return false;
  }
  LUID copy_luid{}, def_luid{};
  {
    ComPtr<IDXGIDevice> dxdev;
    ComPtr<IDXGIAdapter> ad;
    DXGI_ADAPTER_DESC de{};
    if (SUCCEEDED(copy_dev_.As(&dxdev)) &&
        SUCCEEDED(dxdev->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&de)))
      copy_luid = de.AdapterLuid;
    ComPtr<IDXGIAdapter1> a0;
    DXGI_ADAPTER_DESC1 d0{};
    if (factory->EnumAdapters1(0, &a0) != DXGI_ERROR_NOT_FOUND &&
        SUCCEEDED(a0->GetDesc1(&d0)))
      def_luid = d0.AdapterLuid;
  }
  copy_adapter_ok_ = (copy_luid.LowPart == def_luid.LowPart &&
                      copy_luid.HighPart == def_luid.HighPart);
  diag_mode1_state_ = copy_adapter_ok_ ? 0 : 1;
  DiagLogA("MODE1 copy_dev created nvidia=%d copy_luid=%08lx:%08lx "
           "angle_luid=%08lx:%08lx adapter_ok=%d",
           nvidia ? 1 : 0, static_cast<unsigned long>(copy_luid.HighPart),
           copy_luid.LowPart, static_cast<unsigned long>(def_luid.HighPart),
           def_luid.LowPart, copy_adapter_ok_ ? 1 : 0);
  return true;
}

bool FlutterVideoRenderer::CopyNativeSameAdapter(void* handle, int w,
                                                 int h) const {
  using Microsoft::WRL::ComPtr;
  if (!EnsureCopyDevice()) return false;
  if (!copy_tex_ || copy_w_ != w || copy_h_ != h) {
    copy_tex_.Reset();
    copy_handle_ = nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(copy_dev_->CreateTexture2D(&d, nullptr, &copy_tex_))) {
      diag_mode1_state_ = 3;
      return false;
    }
    ComPtr<IDXGIResource> res;
    if (FAILED(copy_tex_.As(&res)) ||
        FAILED(res->GetSharedHandle(&copy_handle_)) || !copy_handle_) {
      copy_tex_.Reset();
      copy_handle_ = nullptr;
      diag_mode1_state_ = 3;
      return false;
    }
    copy_w_ = w;
    copy_h_ = h;
    copy_last_handle_ = nullptr;  // Ziel neu -> Quell-Open erzwingen
  }
  if (handle != copy_last_handle_ || !copy_src_tex_) {
    copy_src_tex_.Reset();
    if (FAILED(copy_dev_->OpenSharedResource(
            reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(copy_src_tex_.GetAddressOf())))) {
      copy_last_handle_ = nullptr;
      diag_mode1_state_ = 2;
      DiagLogA("MODE1 open_fail");
      return false;
    }
    copy_last_handle_ = handle;
  }
  copy_ctx_->CopyResource(copy_tex_.Get(), copy_src_tex_.Get());
  copy_ctx_->Flush();
  return true;
}

// DIAGNOSE-ONLY: den Handle, den ANGLE bekaeme, auf fb_dev_ (= ANGLEs Adapter)
// oeffnen, per Staging nach CPU lesen -> diag_cpu_. Scheitert das Open, ist es das
// ehrliche Adapter-Schwarz. NIE im Produktionspfad (nur alle ~2 s in MaybeDiagnose).
bool FlutterVideoRenderer::ReadbackAngleView(void* handle, int w, int h) const {
  using Microsoft::WRL::ComPtr;
  if (!EnsureFallbackTexture(w, h)) return false;  // legt fb_dev_/fb_ctx_ an
  ComPtr<ID3D11Texture2D> src;
  if (FAILED(fb_dev_->OpenSharedResource(
          reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Texture2D),
          reinterpret_cast<void**>(src.GetAddressOf())))) {
    DiagLogA("MODE%d angle_open_fail", diag_mode_);
    return false;
  }
  if (!diag_staging_ || diag_w_ != w || diag_h_ != h) {
    diag_staging_.Reset();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(fb_dev_->CreateTexture2D(&d, nullptr, &diag_staging_)))
      return false;
    diag_w_ = w;
    diag_h_ = h;
    diag_cpu_.reset(new uint8_t[static_cast<size_t>(w) * h * 4]);
  }
  fb_ctx_->CopyResource(diag_staging_.Get(), src.Get());
  fb_ctx_->Flush();
  D3D11_MAPPED_SUBRESOURCE ms{};
  if (FAILED(fb_ctx_->Map(diag_staging_.Get(), 0, D3D11_MAP_READ, 0, &ms)))
    return false;
  const uint8_t* sp = static_cast<const uint8_t*>(ms.pData);
  for (int y = 0; y < h; ++y)
    std::memcpy(diag_cpu_.get() + static_cast<size_t>(y) * w * 4,
                sp + static_cast<size_t>(y) * ms.RowPitch,
                static_cast<size_t>(w) * 4);
  fb_ctx_->Unmap(diag_staging_.Get(), 0);
  return true;
}

// DIAGNOSE: alle ~2 s die Luminanz + den Nicht-Schwarz-Anteil des Bildes messen,
// das ANGLE bekaeme, und einmalig je Modus ein BMP dumpen. Modus 2 nutzt fb_cpu_
// direkt (gratis); Modus 0/1 lesen ueber ReadbackAngleView zurueck.
void FlutterVideoRenderer::MaybeDiagnose(int w, int h, void* angle_handle,
                                         bool have_cpu) const {
  int64_t now = NowMs();
  if (diag_lum_last_ == 0) diag_lum_last_ = now;
  if (now - diag_lum_last_ < 2000) return;
  diag_lum_last_ = now;
  const uint8_t* px = nullptr;
  if (have_cpu)
    px = fb_cpu_.get();
  else if (ReadbackAngleView(angle_handle, w, h))
    px = diag_cpu_.get();
  if (!px) {
    diag_lum_ = 0.0;  // ehrliches Schwarz (Open/Readback fehlgeschlagen)
    diag_nonblack_ = 0.0;
    return;
  }
  SampleLuminance(px, w, h, &diag_lum_, &diag_nonblack_);
  if (diag_mode_ >= 0 && diag_mode_ < 3 && !diag_dumped_[diag_mode_]) {
    if (const char* base = std::getenv("LOCALAPPDATA")) {
      char p[MAX_PATH];
      std::snprintf(p, sizeof(p), "%s\\HoneyCord\\selfview_mode%d.bmp", base,
                    diag_mode_);
      DumpBmp(p, px, w, h);
      diag_dumped_[diag_mode_] = true;
    }
  }
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

  // === Self-View-DIAGNOSE: 3 Modi mess-vergleichen, Auto-Wechsel ~15 s. Greift
  // NUR fuer die Self-View (is_throttle_) mit nativem Handle; Remote/Sende-Pfad
  // unberuehrt. Bildkorrektheit wird verifiziert (Luminanz + BMP), nicht nur fps
  // (Lehre aus 2.6.7 = 63 fps SCHWARZ).
  bool diag_have_cpu = false;        // Modus 2 -> fb_cpu_ direkt sampeln
  void* diag_angle_handle = handle;  // was ANGLE letztlich bekaeme (Readback)
  if (handle && is_throttle_) {
    int64_t nowm = NowMs();
    if (diag_mode_since_ == 0) diag_mode_since_ = nowm;
    if (nowm - diag_mode_since_ >= kDiagDwellMs) {
      diag_mode_ = (diag_mode_ + 1) % 3;
      diag_mode_since_ = nowm;
    }
    switch (diag_mode_) {
      case 0:  // Baseline: rotierender Capturer-Handle direkt an ANGLE (= heute)
        break;
      case 1:  // GPU-Zero-Copy auf dem NVIDIA-Adapter des Capturers
        if (CopyNativeSameAdapter(handle, w, h)) {
          handle = copy_handle_;
          diag_angle_handle = copy_handle_;
        }  // sonst: Direkt-Handle (Fallback), Zustand in render.log geloggt
        break;
      case 2:  // CPU-Weg (bewaehrter Remote-Pfad)
        if (ConvertNativeCpu(w, h)) {
          handle = fb_handle_;
          diag_have_cpu = true;
        }
        break;
    }
    MaybeDiagnose(w, h, diag_angle_handle, diag_have_cpu);
  }

  if (!handle) {
    // Nicht-nativer Frame (Kamera/Remote, I420) -> bewaehrter CPU-Fallback.
    if (!ConvertNativeCpu(w, h)) return nullptr;
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
                dbg_native_, fbavg, -1.0, diag_mode_, diag_lum_, diag_nonblack_);
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
                frame->native_shared_handle() ? 1 : 0, -1.0, markavg,
                diag_mode_, diag_lum_, diag_nonblack_);
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
