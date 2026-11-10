// Linux: Flutter-Textur, die GPU-Bilder ohne Umweg zeigt (10.11.2026).
//
// ★ GEMESSEN (Intel UHD 630, 1080p, libwebrtc test/empfang_test): Der alte
// Weg kostete 5,0 ms CPU je fremdem Bild (Zurückkopieren, I420→RGBA,
// glTexImage2D in Flutters Pixelpuffer-Textur), dieser 1,4 ms — davon die
// Anzeige 0,14 statt 2,07 ms. Farben gleich (mittlere Abweichung < 2).
//
// Aufbau: Unter Wayland ist Flutters GL-Kontext ein EGL-Kontext. Daneben legt
// dieser Teil einen EIGENEN Kontext an, der seine Texturen mit Flutters teilt.
// Gezeichnet wird nur dort — Flutters GL-Zustand (Skia) bleibt unberührt.
// Danach ein EGL-Fence, auf den Flutters Kontext auf der Grafikkarte wartet.
//
// Rückfälle (je Bild): kein GPU-Bild (Software, gedreht, ältere libwebrtc) →
// das bisherige RGBA aus `CopyPixelBuffer`, hier hochgeladen. Scheitert das
// Einrichten → Pixelweg im Flutter-Kontext, genau wie Flutters eigene
// Pixelpuffer-Textur. Unter X11 (GLX) wird dieser Typ gar nicht erst benutzt.
// Abschalten: HONEYCORD_EMPFANG_GPU=0.
#include "hc_gl_textur.h"

#include <dlfcn.h>
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "hc_gpu_textur.h"

namespace {

// ── EGL über dlopen ──
struct Egl {
  void* (*GetCurrentDisplay)() = nullptr;
  void* (*GetCurrentContext)() = nullptr;
  void* (*GetCurrentSurface)(int) = nullptr;
  unsigned (*MakeCurrent)(void*, void*, void*, void*) = nullptr;
  void* (*CreateContext)(void*, void*, void*, const int*) = nullptr;
  unsigned (*QueryContext)(void*, void*, int, int*) = nullptr;
  unsigned (*ChooseConfig)(void*, const int*, void**, int, int*) = nullptr;
  const char* (*QueryString)(void*, int) = nullptr;
  void* (*GetProcAddress)(const char*) = nullptr;
  void* (*CreateSyncKHR)(void*, unsigned, const int*) = nullptr;
  int (*WaitSyncKHR)(void*, void*, int) = nullptr;
  unsigned (*DestroySyncKHR)(void*, void*) = nullptr;
  bool ok = false;

  bool Laden() {
    void* h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libEGL.so.1", RTLD_NOW);
    if (!h) return false;
#define HC_EGL(n) n = reinterpret_cast<decltype(n)>(dlsym(h, "egl" #n)); if (!n) return false;
    HC_EGL(GetCurrentDisplay) HC_EGL(GetCurrentContext) HC_EGL(GetCurrentSurface)
    HC_EGL(MakeCurrent) HC_EGL(CreateContext) HC_EGL(QueryContext) HC_EGL(ChooseConfig)
    HC_EGL(QueryString) HC_EGL(GetProcAddress)
#undef HC_EGL
    CreateSyncKHR = reinterpret_cast<decltype(CreateSyncKHR)>(GetProcAddress("eglCreateSyncKHR"));
    WaitSyncKHR = reinterpret_cast<decltype(WaitSyncKHR)>(GetProcAddress("eglWaitSyncKHR"));
    DestroySyncKHR = reinterpret_cast<decltype(DestroySyncKHR)>(GetProcAddress("eglDestroySyncKHR"));
    ok = true;
    return true;
  }
};

constexpr int kEglDraw = 0x3059, kEglRead = 0x305A, kEglConfigId = 0x3028, kEglNone = 0x3038,
              kEglExtensions = 0x3055, kEglMajor = 0x3098, kEglMinor = 0x30FB,
              kEglProfileMask = 0x30FD;
constexpr unsigned kEglSyncFence = 0x30F9;
constexpr unsigned kGlVersion = 0x1F02, kGlProfileMask = 0x9126;

// Alles hier läuft auf Flutters Raster-Faden (populate). Abgemeldete Texturen
// kommen vom Plattform-Faden — sie werden vorgemerkt und hier gelöscht.
struct Gpu {
  enum class Zustand { kNeu, kBereit, kNotweg } zustand = Zustand::kNeu;
  Egl egl;
  void* anzeige = nullptr;
  void* eigener = nullptr;
  hcgpu::Zeichner z;
  bool fence = false;
  // Notweg (im Flutter-Kontext): nur Hochladen.
  void (*glGenTextures)(int, unsigned*) = nullptr;
  void (*glBindTexture)(unsigned, unsigned) = nullptr;
  void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*) = nullptr;
  void (*glTexParameteri)(unsigned, unsigned, int) = nullptr;
  void (*glDeleteTextures)(int, const unsigned*) = nullptr;
  void (*glFinish)() = nullptr;

  std::mutex weg_m;
  std::vector<unsigned> zu_loeschen;

  void* f_draw = nullptr;
  void* f_read = nullptr;
  void* f_ctx = nullptr;

  void Einrichten() {
    zustand = Zustand::kNotweg;
    if (!egl.Laden()) return;
    auto gp = reinterpret_cast<hcgpu::GetProc>(egl.GetProcAddress);
    glGenTextures = reinterpret_cast<decltype(glGenTextures)>(gp("glGenTextures"));
    glBindTexture = reinterpret_cast<decltype(glBindTexture)>(gp("glBindTexture"));
    glTexImage2D = reinterpret_cast<decltype(glTexImage2D)>(gp("glTexImage2D"));
    glTexParameteri = reinterpret_cast<decltype(glTexParameteri)>(gp("glTexParameteri"));
    glDeleteTextures = reinterpret_cast<decltype(glDeleteTextures)>(gp("glDeleteTextures"));
    glFinish = reinterpret_cast<decltype(glFinish)>(gp("glFinish"));
    anzeige = egl.GetCurrentDisplay();
    void* flutter = egl.GetCurrentContext();
    if (!anzeige || !flutter) {
      g_message("HoneyCord: Empfang ohne Umweg — kein EGL-Kontext, Pixelweg");
      return;
    }
    // Welcher GL-Dialekt? Das bestimmt Kontext-Attribute und Shader.
    auto glGetString = reinterpret_cast<const char* (*)(unsigned)>(gp("glGetString"));
    auto glGetIntegerv = reinterpret_cast<void (*)(unsigned, int*)>(gp("glGetIntegerv"));
    const char* v = glGetString ? glGetString(kGlVersion) : nullptr;
    if (!v) return;
    hcgpu::Dialekt d = hcgpu::Dialekt::kES;
    std::vector<int> attr;
    if (std::strncmp(v, "OpenGL ES", 9) == 0) {
      int major = 2;
      std::sscanf(v + 9, " %d", &major);
      attr = {kEglMajor, major >= 3 ? 3 : 2, kEglNone};
    } else {
      int major = 2, minor = 1, maske = 0;
      std::sscanf(v, "%d.%d", &major, &minor);
      if (major >= 3 && glGetIntegerv) glGetIntegerv(kGlProfileMask, &maske);
      const bool core = (maske & 1) != 0;
      d = core ? hcgpu::Dialekt::kCore : hcgpu::Dialekt::kLegacy;
      attr = {kEglMajor, major, kEglMinor, minor};
      if (major >= 3) { attr.push_back(kEglProfileMask); attr.push_back(core ? 1 : 2); }
      attr.push_back(kEglNone);
    }
    int id = 0;
    void* konfig = nullptr;
    if (egl.QueryContext(anzeige, flutter, kEglConfigId, &id) && id > 0) {
      const int such[] = {kEglConfigId, id, kEglNone};
      int n = 0;
      egl.ChooseConfig(anzeige, such, &konfig, 1, &n);
      if (n < 1) konfig = nullptr;
    }
    eigener = egl.CreateContext(anzeige, konfig, flutter, attr.data());
    if (!eigener) {
      g_message("HoneyCord: Empfang ohne Umweg — eigener GL-Kontext nicht anzulegen (%s), Pixelweg", v);
      return;
    }
    const char* ext = egl.QueryString(anzeige, kEglExtensions);
    fence = ext && std::strstr(ext, "EGL_KHR_fence_sync") && std::strstr(ext, "EGL_KHR_wait_sync") &&
            egl.CreateSyncKHR && egl.WaitSyncKHR && egl.DestroySyncKHR;
    Betreten();
    const bool zok = z.Einrichten(gp, anzeige, d);
    Verlassen();
    if (!zok) {
      g_message("HoneyCord: Empfang ohne Umweg — GL-Baustein scheitert (%s), Pixelweg", v);
      return;
    }
    zustand = Zustand::kBereit;
    g_message("HoneyCord: Empfang ohne Umweg bereit (%s, %s)", v,
              d == hcgpu::Dialekt::kES ? "ES" : d == hcgpu::Dialekt::kCore ? "Core" : "Legacy");
    // Erst jetzt der Bibliothek sagen, dass GPU-Bilder hier ankommen dürfen.
    using Melden = void (*)(int);
    if (auto m = reinterpret_cast<Melden>(dlsym(RTLD_DEFAULT, "honeycord_empfang_gpu_bereit"))) m(1);
  }

  void Betreten() {
    f_draw = egl.GetCurrentSurface(kEglDraw);
    f_read = egl.GetCurrentSurface(kEglRead);
    f_ctx = egl.GetCurrentContext();
    egl.MakeCurrent(anzeige, nullptr, nullptr, eigener);
    std::vector<unsigned> weg;
    {
      std::lock_guard<std::mutex> l(weg_m);
      weg.swap(zu_loeschen);
    }
    for (unsigned t : weg) z.TexturWeg(&t);
  }

  void Verlassen() {
    void* sync = nullptr;
    if (fence) sync = egl.CreateSyncKHR(anzeige, kEglSyncFence, nullptr);
    else if (glFinish) glFinish();
    egl.MakeCurrent(anzeige, f_draw, f_read, f_ctx);
    if (sync) {
      // Flutters Kontext wartet auf der GRAFIKKARTE, der Faden läuft weiter.
      egl.WaitSyncKHR(anzeige, sync, 0);
      egl.DestroySyncKHR(anzeige, sync);
    }
  }
};

Gpu& gpu() {
  static Gpu* g = new Gpu();  // lebt bis zum Prozessende (Raster-Faden)
  return *g;
}

}  // namespace

struct HcGlTextur {
  FlTextureGL parent_instance;
  flutter::TextureVariant* variante = nullptr;
  unsigned tex = 0;
  int breite = 0, hoehe = 0;
};

struct HcGlTexturClass {
  FlTextureGLClass parent_class;
};

G_DEFINE_TYPE(HcGlTextur, hc_gl_textur, fl_texture_gl_get_type())

#define HC_GL_TEXTUR(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), hc_gl_textur_get_type(), HcGlTextur))

static gboolean hc_gl_textur_populate(FlTextureGL* texture, uint32_t* target, uint32_t* name,
                                      uint32_t* width, uint32_t* height, GError** error) {
  HcGlTextur* self = HC_GL_TEXTUR(texture);
  Gpu& g = gpu();
  if (g.zustand == Gpu::Zustand::kNeu) g.Einrichten();
  auto& pb = std::get<flutter::PixelBufferTexture>(*self->variante);

  if (g.zustand == Gpu::Zustand::kBereit) {
    g.Betreten();
    bool ok = false;
    hcgpu::Bild b;
    if (pb.GpuBild(&b)) ok = g.z.Zeichnen(b, &self->tex, &self->breite, &self->hoehe);
    if (!ok) {
      const FlutterDesktopPixelBuffer* p = pb.CopyPixelBuffer(*width, *height);
      if (p && p->buffer)
        ok = g.z.Hochladen(p->buffer, static_cast<int>(p->width), static_cast<int>(p->height),
                           &self->tex, &self->breite, &self->hoehe);
    }
    if (!self->tex) {
      // Noch kein Bild: durchsichtig statt eines Fehlers je Bild.
      static const uint8_t leer[4] = {0, 0, 0, 0};
      g.z.Hochladen(leer, 1, 1, &self->tex, &self->breite, &self->hoehe);
    }
    g.Verlassen();
  } else if (g.glGenTextures && g.glTexImage2D) {
    // Notweg: wie Flutters eigene Pixelpuffer-Textur, im Flutter-Kontext.
    static const uint8_t leer[4] = {0, 0, 0, 0};
    static const FlutterDesktopPixelBuffer kLeer = {leer, 1, 1, nullptr, nullptr};
    const FlutterDesktopPixelBuffer* p = pb.CopyPixelBuffer(*width, *height);
    if ((!p || !p->buffer) && !self->tex) p = &kLeer;
    if (p && p->buffer) {
      if (!self->tex) {
        g.glGenTextures(1, &self->tex);
        g.glBindTexture(hcgpu::kTexture2D, self->tex);
        g.glTexParameteri(hcgpu::kTexture2D, hcgpu::kMinFilter, hcgpu::kLinear);
        g.glTexParameteri(hcgpu::kTexture2D, hcgpu::kMagFilter, hcgpu::kLinear);
        g.glTexParameteri(hcgpu::kTexture2D, hcgpu::kWrapS, hcgpu::kClampToEdge);
        g.glTexParameteri(hcgpu::kTexture2D, hcgpu::kWrapT, hcgpu::kClampToEdge);
      }
      g.glBindTexture(hcgpu::kTexture2D, self->tex);
      g.glTexImage2D(hcgpu::kTexture2D, 0, hcgpu::kRGBA, static_cast<int>(p->width),
                     static_cast<int>(p->height), 0, hcgpu::kRGBA, hcgpu::kUByte, p->buffer);
      self->breite = static_cast<int>(p->width);
      self->hoehe = static_cast<int>(p->height);
    }
  }
  if (!self->tex) {
    // Noch kein Bild — wie der Pixelpuffer ohne Puffer: nichts zeichnen.
    g_set_error(error, g_quark_from_static_string("honeycord"), 1, "noch kein Bild");
    return FALSE;
  }
  *target = hcgpu::kTexture2D;
  *name = self->tex;
  *width = static_cast<uint32_t>(self->breite);
  *height = static_cast<uint32_t>(self->hoehe);
  return TRUE;
}

static void hc_gl_textur_dispose(GObject* obj) {
  HcGlTextur* self = HC_GL_TEXTUR(obj);
  if (self->tex) {
    // Nicht hier löschen: kein GL-Kontext auf diesem Faden.
    Gpu& g = gpu();
    std::lock_guard<std::mutex> l(g.weg_m);
    g.zu_loeschen.push_back(self->tex);
    self->tex = 0;
  }
  G_OBJECT_CLASS(hc_gl_textur_parent_class)->dispose(obj);
}

static void hc_gl_textur_class_init(HcGlTexturClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = hc_gl_textur_populate;
  G_OBJECT_CLASS(klass)->dispose = hc_gl_textur_dispose;
}

static void hc_gl_textur_init(HcGlTextur*) {}

bool hc_gl_textur_moeglich() {
  static const bool an = [] {
    const char* aus = std::getenv("HONEYCORD_EMPFANG_GPU");
    if (aus && std::strcmp(aus, "0") == 0) return false;
#ifdef GDK_WINDOWING_WAYLAND
    GdkDisplay* d = gdk_display_get_default();
    if (d == nullptr || !GDK_IS_WAYLAND_DISPLAY(d)) return false;
    // Ohne ladbares EGL gar nicht erst — sonst bliebe die Kachel leer.
    void* h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libEGL.so.1", RTLD_NOW);
    return h != nullptr;
#else
    return false;
#endif
  }();
  return an;
}

FlTexture* hc_gl_textur_neu(flutter::TextureVariant* variante) {
  HcGlTextur* t = HC_GL_TEXTUR(g_object_new(hc_gl_textur_get_type(), nullptr));
  t->variante = variante;
  return FL_TEXTURE(t);
}
