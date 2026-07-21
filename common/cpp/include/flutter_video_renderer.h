#ifndef FLUTTER_WEBRTC_RTC_VIDEO_RENDERER_HXX
#define FLUTTER_WEBRTC_RTC_VIDEO_RENDERER_HXX

#include "flutter_common.h"
#include "flutter_webrtc_base.h"

#include "rtc_video_frame.h"
#include "rtc_video_renderer.h"

#include <mutex>
#ifdef _WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace flutter_webrtc_plugin {

using namespace libwebrtc;

class FlutterVideoRenderer
    : public RTCVideoRenderer<scoped_refptr<RTCVideoFrame>>,
      public RefCountInterface {
 public:
  FlutterVideoRenderer() = default;
  ~FlutterVideoRenderer();

  void initialize(TextureRegistrar* registrar,
                  BinaryMessenger* messenger,
                  TaskRunner* task_runner,
                  std::unique_ptr<flutter::TextureVariant> texture,
                  int64_t texture_id);

  virtual const FlutterDesktopPixelBuffer* CopyPixelBuffer(size_t width,
                                                           size_t height) const;

  virtual void OnFrame(scoped_refptr<RTCVideoFrame> frame) override;

  void SetVideoTrack(scoped_refptr<RTCVideoTrack> track);

  int64_t texture_id() { return texture_id_; }

  bool CheckMediaStream(std::string mediaId);

  bool CheckVideoTrack(std::string mediaId);

  std::string media_stream_id;

#ifdef _WINDOWS
  // GPU-Zero-Copy-Vorschau: Callback fuer Flutters GpuSurfaceTexture. Native
  // (GPU) Frames -> DXGI-Shared-Handle des Capturers direkt an ANGLE (kein
  // Readback). Nicht-native Frames (Kamera/Remote, I420) -> in eine eigene
  // plain-SHARED-BGRA-Textur konvertieren+hochladen (Fallback, Layer 3b).
  const FlutterDesktopGpuSurfaceDescriptor* ObtainGpuSurface(
      size_t width, size_t height) const;
  void set_gpu_surface(bool v) { is_gpu_surface_ = v; }
  void set_throttle(bool v) { is_throttle_ = v; }
#endif

 private:
  struct FrameSize {
    size_t width;
    size_t height;
  };
  FrameSize last_frame_size_ = {0, 0};
  bool first_frame_rendered = false;
  TextureRegistrar* registrar_ = nullptr;
  std::unique_ptr<EventChannelProxy> event_channel_;
  int64_t texture_id_ = -1;
  scoped_refptr<RTCVideoTrack> track_ = nullptr;
  scoped_refptr<RTCVideoFrame> frame_;
  std::unique_ptr<flutter::TextureVariant> texture_;
  std::shared_ptr<FlutterDesktopPixelBuffer> pixel_buffer_;
  mutable std::shared_ptr<uint8_t> rgb_buffer_;
  mutable std::mutex mutex_;
  RTCVideoFrame::VideoRotation rotation_ = RTCVideoFrame::kVideoRotation_0;
#ifdef _WINDOWS
  mutable FlutterDesktopGpuSurfaceDescriptor gpu_descriptor_ = {};
  // Fallback fuer nicht-native Frames (Kamera/SW-Remote): eigene plain-SHARED
  // BGRA-Textur (kein Keyed-Mutex), in die per ConvertToARGB(kARGB)+Upload
  // geschrieben wird (libyuv I420ToARGB = Speicher B,G,R,A = B8G8R8A8; das
  // fruehere kBGRA schrieb A,R,G,B -> Blaustich); Legacy-Handle geht an ANGLE.
  mutable Microsoft::WRL::ComPtr<ID3D11Device> fb_dev_;
  mutable Microsoft::WRL::ComPtr<ID3D11DeviceContext> fb_ctx_;
  mutable Microsoft::WRL::ComPtr<ID3D11Texture2D> fb_tex_;
  mutable void* fb_handle_ = nullptr;
  mutable int fb_w_ = 0;
  mutable int fb_h_ = 0;
  mutable std::shared_ptr<uint8_t> fb_cpu_;
  bool EnsureFallbackTexture(int w, int h) const;

  // Self-View-Zero-Copy AUF ANGLES EIGENEM GERAET (2026-07-21): den rotierenden
  // Capturer-Handle NICHT direkt an ANGLE geben — ANGLE re-bindet bei jedem
  // Handle-Wechsel (eglBindTexImage) und Flutters Fenster-Compositor bricht auf
  // ~20 fps ein (zieht Vorschau, App-Responsivitaet UND Remote-Kacheln runter).
  // Fix: ANGLEs eigenes ID3D11Device per EGL erfragen (Muster aus Flutters
  // engine/egl/manager.cc InitializeDevice; ObtainGpuSurface laeuft auf dem
  // Raster-Thread mit aktuellem EGL-Kontext), das Ring-Handle DORT oeffnen
  // (ANGLEs eigener Import beweist, dass die Ring-Texturen auf DIESEM Geraet
  // lesbar sind) und per CopyResource in eine statische Ziel-Textur kopieren;
  // deren Handle geht an ANGLE (Bind-once, Same-Device-Write = live gesampelt).
  // ~0 CPU, 1 GPU-Copy, volle Qualitaet. WICHTIG (3 Schwarz-Fehlschlaege
  // 2.6.7/2.6.9/2.6.10): die Ring-Texturen sind auf FREMDEN Geraeten
  // (fb_dev_/copy_dev_) NICHT lesbar (kein Keyed-Mutex-Sync) — nur auf g_dev_
  // (via ToI420) oder auf ANGLEs Geraet. Jeder Fehler -> Direkt-Handle
  // (sichtbar). Nur Self-View (is_throttle_); Sende-/Remote-Pfad unberuehrt.
  static constexpr unsigned kAfEglApi = 1u << 0;
  static constexpr unsigned kAfQueryDisplay = 1u << 1;
  static constexpr unsigned kAfQueryDevice = 1u << 2;
  static constexpr unsigned kAfWrongThread = 1u << 3;
  static constexpr unsigned kAfMultithread = 1u << 4;
  static constexpr unsigned kAfDestCreate = 1u << 5;
  static constexpr unsigned kAfDestHandle = 1u << 6;
  static constexpr unsigned kAfSrcOpen = 1u << 7;
  static constexpr unsigned kAfDescMismatch = 1u << 8;
  mutable Microsoft::WRL::ComPtr<ID3D11Device> angle_dev_;
  mutable Microsoft::WRL::ComPtr<ID3D11DeviceContext> angle_ctx_;  // immediate
  mutable Microsoft::WRL::ComPtr<ID3D11Texture2D> angle_dest_tex_;
  mutable void* angle_dest_handle_ = nullptr;  // STATISCH (nur Groessenwechsel)
  mutable int angle_w_ = 0, angle_h_ = 0;
  mutable void* angle_copied_handle_ = nullptr;  // new-frame-Erkennung
  struct AngleSrc {
    void* handle = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  };
  mutable AngleSrc angle_src_[3];  // Ring-Cache (Capturer rotiert 3 Handles)
  mutable int angle_src_evict_ = 0;
  mutable DWORD angle_tid_ = 0;            // Raster-Thread-Assertion
  mutable unsigned angle_fail_logged_ = 0;  // je Fehlerklasse 1x loggen
  void AngleLogOnce(unsigned bit, const char* what, long hr) const;
  bool EnsureAngleDevice() const;
  bool EnsureAngleDest(int w, int h) const;
  ID3D11Texture2D* AngleOpenSrc(void* handle) const;
  bool CopySelfViewAngleDevice(int w, int h, void* handle) const;
  bool UploadFrameCpuToFallback(int w, int h) const;  // kARGB (korrekte Farben)

#ifdef HONEYCORD_SELFVIEW_PROBE
  // Probe-Bau (nur Diagnose, CMake-Define): 4 Phasen je 20 s, 0->3->1->2.
  static constexpr int64_t kProbeDwellMs = 20000;
  mutable int64_t probe_start_ms_ = 0;
  mutable int probe_phase_ = 0;
  mutable void* probe_p3_last_handle_ = nullptr;
  mutable int64_t probe_lum_last_ms_ = 0;
  mutable double probe_lum_ = -1.0, probe_nonblack_ = -1.0;
  mutable bool probe_dumped_[4] = {};
  mutable Microsoft::WRL::ComPtr<ID3D11Texture2D> probe_staging_;  // ANGLE-Dev!
  mutable int probe_stg_w_ = 0, probe_stg_h_ = 0;
  mutable std::unique_ptr<uint8_t[]> probe_cpu_;
  mutable size_t probe_cpu_size_ = 0;
  mutable int64_t probe_p2_last_ms_ = 0;
  bool ProbeFillP2Pattern(int w, int h) const;
  bool ProbeReadbackAngle(ID3D11Texture2D* tex, int w, int h) const;
  void ProbeMaybeSample(int phase, int w, int h, void* ring_handle) const;
#endif

  // Self-View-Drossel: die EIGENE Bildschirm-Selbstansicht zeigt den Schirm, den
  // man eh sieht -> auf ~25 fps drosseln, damit nicht jeder 60-fps-Frame ein
  // teures Flutter-Fenster-Composite (ANGLE) ausloest. Der Sende-Pfad (Encoder)
  // ist davon unberuehrt. Greift NUR wenn is_throttle_ gesetzt ist; Remote-
  // Kacheln nutzen denselben GpuSurface-Pfad, aber OHNE Drossel (volle FPS).
  bool is_gpu_surface_ = false;
  bool is_throttle_ = false;
  int64_t last_mark_ms_ = 0;
  // Diagnose (2026-07-02): echte Render-fps messen. COMPOSITE = wie oft Flutter die
  // Kachel wirklich zeichnet (ObtainGpuSurface/s); ONFRAME = wie oft ein Frame an
  // den Sink geliefert wird. Selbst-Ansicht (throttled) sagt: schafft der Composite
  // ~25 fps (Render schnell) oder haengt er bei ~8 (Render = der Engpass)?
  mutable unsigned dbg_ob_calls_ = 0;
  mutable int64_t dbg_ob_last_ms_ = 0;
  unsigned dbg_of_calls_ = 0;
  int64_t dbg_of_last_ms_ = 0;
  // native=1: HW-Frame kam als GPU-Shared-Handle an (Zero-Copy-Pfad). native=0:
  // I420-Fallback (frame->native_shared_handle()==null) -> jeder Composite
  // konvertiert+laedt hoch (dbg_fb_ms_ = Summe dieser Kosten im 2s-Fenster).
  // Beantwortet: ueberlebt die HW-Textur bis zum Renderer, und was kostet der Pfad?
  mutable int dbg_native_ = -1;
  mutable double dbg_fb_ms_ = 0.0;
  mutable unsigned dbg_fb_n_ = 0;
  // Dauer von MarkTextureFrameAvailable (im ONFRAME-Log als mark=X). Blockiert das
  // ~140ms, ist Flutters Composite der Decoder-Textur der 7-fps-Deckel (Sink-
  // Backpressure) -> WebRTCs Render-Pacing wirft die restlichen Frames weg.
  double dbg_mark_ms_ = 0.0;
  unsigned dbg_mark_n_ = 0;
#endif
};

class FlutterVideoRendererManager {
 public:
  FlutterVideoRendererManager(FlutterWebRTCBase* base);

  void CreateVideoRendererTexture(bool use_gpu_surface, bool throttle,
                                  std::unique_ptr<MethodResultProxy> result);

  void VideoRendererSetSrcObject(int64_t texture_id,
                                 const std::string& stream_id,
                                 const std::string& owner_tag,
                                 const std::string& track_id);

  void VideoRendererDispose(int64_t texture_id,
                            std::unique_ptr<MethodResultProxy> result);

 private:
  FlutterWebRTCBase* base_;
  std::map<int64_t, scoped_refptr<FlutterVideoRenderer>> renderers_;
};

}  // namespace flutter_webrtc_plugin

#endif  // !FLUTTER_WEBRTC_RTC_VIDEO_RENDERER_HXX