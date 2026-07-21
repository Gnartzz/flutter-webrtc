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
  // GpuSurface-Vorschau: Callback fuer Flutters GpuSurfaceTexture.
  // Remote-Kacheln (unthrottled, native HW-Decode) -> Handle direkt an ANGLE.
  // Self-View (is_throttle_, nativ) -> kohaerenter CPU-Weg in die statische
  // fb_tex_ (Bind-once; s. Self-View-Fix unten). Nicht-native Frames
  // (Kamera/SW-Remote, I420) -> derselbe CPU-Weg (Fallback).
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
  // Fallback-Textur (statischer Handle an ANGLE): fuer nicht-native Frames
  // (Kamera/SW-Remote) UND die Self-View. Befuellt per ConvertToARGB(kARGB)+
  // CPU-Upload — kARGB, weil libyuv I420ToARGB Speicher-Bytes B,G,R,A schreibt
  // (= DXGI B8G8R8A8); das fruehere kBGRA (I420ToBGRA = A,R,G,B) legte Alpha
  // in den Blaukanal = Blaustich. Kein Keyed-Mutex.
  mutable Microsoft::WRL::ComPtr<ID3D11Device> fb_dev_;
  mutable Microsoft::WRL::ComPtr<ID3D11DeviceContext> fb_ctx_;
  mutable Microsoft::WRL::ComPtr<ID3D11Texture2D> fb_tex_;
  mutable void* fb_handle_ = nullptr;
  mutable int fb_w_ = 0;
  mutable int fb_h_ = 0;
  mutable std::shared_ptr<uint8_t> fb_cpu_;
  bool EnsureFallbackTexture(int w, int h) const;
  bool UploadFrameCpuToFallback(int w, int h) const;

  // Self-View-Fix (2026-07-21, Probe-verifiziert auf AIX1/RTX 2070S): der
  // rotierende Capturer-Handle zwingt ANGLE zu einem Re-Bind PRO FRAME
  // (eglBindTexImage) -> Fenster-Compositor ~20 fps (zieht Vorschau, App UND
  // Remote-Kacheln runter). Fix: Frame kohaerent auf dem Capturer-Geraet lesen
  // (ConvertToARGB/ToI420) und per CPU-Write in die STATISCHE fb_tex_ (Bind-
  // once; gemessen 18,7 -> 31 fps, korrekte Farben). Nur bei neuem Frame +
  // >= 40 ms (~25 Konv./s). GPU-Zero-Copy unmoeglich: Ring-Texturen sind nur
  // auf dem Capturer-Geraet / via ANGLEs EGL-Import lesbar (Cross-Device =
  // schwarz, 2.6.7/9/10), und ANGLEs D3D-Geraet ist aus dem Plugin
  // unerreichbar (flutter_windows.dll exportiert keine egl*-Symbole).
  mutable void* sv_last_handle_ = nullptr;
  mutable int64_t sv_last_conv_ms_ = 0;
  mutable bool sv_ready_ = false;
  mutable bool sv_fail_logged_ = false;

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