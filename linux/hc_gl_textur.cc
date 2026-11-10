// Linux: Flutter-Textur, die GPU-Bilder ohne Umweg zeigt (10.11.2026).
//
// ★ GEMESSEN (Intel UHD 630, 1080p, libwebrtc test/empfang_test): Der alte
// Weg kostete 5,0 ms CPU je fremdem Bild (Zurückkopieren, I420→RGBA,
// glTexImage2D in Flutters Pixelpuffer-Textur), dieser 1,4 ms — davon die
// Anzeige 0,14 statt 2,07 ms. Farben gleich (mittlere Abweichung < 2).
//
// Aufbau: Unter Wayland ist Flutters GL-Kontext ein EGL-Kontext (OpenGL ES).
// Je Engine legt dieser Teil daneben einen EIGENEN Kontext an, der seine Texturen mit Flutters teilt.
// Gezeichnet wird nur dort — Flutters GL-Zustand (Skia) bleibt unberührt.
// Danach ein EGL-Fence, auf den Flutters Kontext auf der Grafikkarte wartet.
//
// Pixelbilder (Software, gedreht, ältere libwebrtc, GPU-Weg aus) lädt diese
// Textur wie Flutters eigene Pixelpuffer-Textur in Flutters Kontext hoch —
// den eigenen Kontext betritt nur ein echtes GPU-Bild. Scheitert das
// Einrichten, ein Kontextwechsel oder 30-mal das Zeichnen, ist der GPU-Weg
// für den Prozess aus (und die Bibliothek erfährt es). Gezeichnet wird auf
// Kachelgröße (Flutter nennt sie in populate). Unter X11 (GLX) wird dieser
// Typ gar nicht erst benutzt.
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
              kEglExtensions = 0x3055, kEglMajor = 0x3098;
constexpr unsigned kEglSyncFence = 0x30F9;
constexpr unsigned kGlVersion = 0x1F02;

// So oft hintereinander darf das Zeichnen eines GPU-Bilds scheitern, bevor
// der GPU-Weg für diesen Prozess aus ist (sonst je Bild: GPU-Kopie, Export,
// Fehlschlag UND Zurücklesen — teurer als der alte Weg).
constexpr int kFehlerBremse = 30;

using Melden = void (*)(int);
Melden Bibliothek() {
  static const Melden m =
      reinterpret_cast<Melden>(dlsym(RTLD_DEFAULT, "honeycord_empfang_gpu_bereit"));
  return m;
}

// Je Flutter-GL-Kontext (= je Engine) einer: Texturnamen gelten nur in der
// Teilungsgruppe dieses Kontexts. Alles hier läuft auf dem Raster-Faden der
// Engine (populate); abgemeldete Texturen kommen vom Plattform-Faden — sie
// werden vorgemerkt und beim nächsten populate in Flutters Kontext gelöscht.
struct Gpu {
  void* flutter = nullptr;  // Flutters Kontext, zu dem dieser gehört
  bool gpu_an = false;      // eigener Kontext + Zeichner bereit
  Egl egl;
  void* anzeige = nullptr;
  void* eigener = nullptr;
  hcgpu::Zeichner z;
  bool fence = false;
  int fehler_in_folge = 0;
  // Pixelweg (in Flutters Kontext, wie Flutters eigene Pixelpuffer-Textur).
  void (*glGenTextures)(int, unsigned*) = nullptr;
  void (*glBindTexture)(unsigned, unsigned) = nullptr;
  void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*) = nullptr;
  void (*glTexParameteri)(unsigned, unsigned, int) = nullptr;
  void (*glDeleteTextures)(int, const unsigned*) = nullptr;
  void (*glPixelStorei)(unsigned, int) = nullptr;
  void (*glFinish)() = nullptr;

  std::mutex weg_m;
  std::vector<unsigned> zu_loeschen;

  void* f_draw = nullptr;
  void* f_read = nullptr;

  bool PixelwegBereit() const { return glGenTextures && glBindTexture && glTexImage2D && glTexParameteri && glDeleteTextures && glPixelStorei; }

  void Einrichten(void* flutter_ctx) {
    flutter = flutter_ctx;
    if (!egl.Laden()) return;
    auto gp = reinterpret_cast<hcgpu::GetProc>(egl.GetProcAddress);
    glGenTextures = reinterpret_cast<decltype(glGenTextures)>(gp("glGenTextures"));
    glBindTexture = reinterpret_cast<decltype(glBindTexture)>(gp("glBindTexture"));
    glTexImage2D = reinterpret_cast<decltype(glTexImage2D)>(gp("glTexImage2D"));
    glTexParameteri = reinterpret_cast<decltype(glTexParameteri)>(gp("glTexParameteri"));
    glDeleteTextures = reinterpret_cast<decltype(glDeleteTextures)>(gp("glDeleteTextures"));
    glPixelStorei = reinterpret_cast<decltype(glPixelStorei)>(gp("glPixelStorei"));
    glFinish = reinterpret_cast<decltype(glFinish)>(gp("glFinish"));
    anzeige = egl.GetCurrentDisplay();
    if (!anzeige || !flutter) {
      g_message("HoneyCord: Empfang ohne Umweg — kein EGL-Kontext, Pixelweg");
      return;
    }
    // Flutter legt unter Wayland OpenGL-ES-Kontexte an (fl_opengl_manager).
    // Desktop-GL (ältere Engines) bleibt beim Pixelweg: dafür müsste die
    // EGL-API des Fadens umgestellt werden, und das gehört Flutter.
    auto glGetString = reinterpret_cast<const char* (*)(unsigned)>(gp("glGetString"));
    const char* v = glGetString ? glGetString(kGlVersion) : nullptr;
    if (!v || std::strncmp(v, "OpenGL ES", 9) != 0) {
      g_message("HoneyCord: Empfang ohne Umweg — kein OpenGL-ES-Kontext (%s), Pixelweg", v ? v : "?");
      return;
    }
    int major = 2;
    std::sscanf(v + 9, " %d", &major);
    const int attr[] = {kEglMajor, major >= 3 ? 3 : 2, kEglNone};
    int id = 0;
    void* konfig = nullptr;
    if (egl.QueryContext(anzeige, flutter, kEglConfigId, &id) && id > 0) {
      const int such[] = {kEglConfigId, id, kEglNone};
      int n = 0;
      egl.ChooseConfig(anzeige, such, &konfig, 1, &n);
      if (n < 1) konfig = nullptr;
    }
    eigener = egl.CreateContext(anzeige, konfig, flutter, attr);
    if (!eigener) {
      g_message("HoneyCord: Empfang ohne Umweg — eigener GL-Kontext nicht anzulegen (%s), Pixelweg", v);
      return;
    }
    const char* ext = egl.QueryString(anzeige, kEglExtensions);
    fence = ext && std::strstr(ext, "EGL_KHR_fence_sync") && std::strstr(ext, "EGL_KHR_wait_sync") &&
            egl.CreateSyncKHR && egl.WaitSyncKHR && egl.DestroySyncKHR;
    if (!Betreten()) return;
    const bool zok = z.Einrichten(gp, anzeige, hcgpu::Dialekt::kES);
    if (!Verlassen() || !zok) {
      g_message("HoneyCord: Empfang ohne Umweg — GL-Baustein scheitert (%s), Pixelweg", v);
      return;
    }
    gpu_an = true;
    g_message("HoneyCord: Empfang ohne Umweg bereit (%s, Fence %s)", v, fence ? "ja" : "nein");
    // Erst jetzt der Bibliothek sagen, dass GPU-Bilder hier ankommen dürfen.
    if (Bibliothek()) Bibliothek()(1);
  }

  // Den GPU-Weg dauerhaft beenden — und der Bibliothek sagen, dass sie
  // keine GPU-Bilder mehr ausgeben soll (-1 = gesperrt, nicht nur aus).
  void Aufgeben(const char* grund) {
    if (!gpu_an) return;
    gpu_an = false;
    g_message("HoneyCord: Empfang ohne Umweg aus — %s, ab jetzt Pixelweg", grund);
    if (Bibliothek()) Bibliothek()(-1);
  }

  // Wechsel in den eigenen Kontext. false = Wechsel scheiterte; dann ist
  // (nach Möglichkeit) wieder Flutters Kontext aktiv, und nichts wurde
  // gezeichnet — in Flutters Kontext zu zeichnen hieße Skias GL-Zustand
  // (Framebuffer, Programm, Viewport) zu zerstören.
  bool Betreten() {
    f_draw = egl.GetCurrentSurface(kEglDraw);
    f_read = egl.GetCurrentSurface(kEglRead);
    if (!egl.MakeCurrent(anzeige, nullptr, nullptr, eigener)) {
      egl.MakeCurrent(anzeige, f_draw, f_read, flutter);
      return false;
    }
    return true;
  }

  bool Verlassen() {
    void* sync = nullptr;
    if (fence) sync = egl.CreateSyncKHR(anzeige, kEglSyncFence, nullptr);
    if (!sync && glFinish) glFinish();
    if (!egl.MakeCurrent(anzeige, f_draw, f_read, flutter)) {
      if (sync) egl.DestroySyncKHR(anzeige, sync);
      return false;
    }
    if (sync) {
      // Flutters Kontext wartet auf der GRAFIKKARTE, der Faden läuft weiter.
      egl.WaitSyncKHR(anzeige, sync, 0);
      egl.DestroySyncKHR(anzeige, sync);
    }
    return true;
  }

  // In Flutters Kontext (Texturnamen sind geteilt).
  void Aufraeumen() {
    std::vector<unsigned> weg;
    {
      std::lock_guard<std::mutex> l(weg_m);
      weg.swap(zu_loeschen);
    }
    if (!weg.empty() && glDeleteTextures) glDeleteTextures(static_cast<int>(weg.size()), weg.data());
  }

  void Hochladen(const FlutterDesktopPixelBuffer* p, unsigned* tex, int* b, int* h) {
    if (!*tex) {
      glGenTextures(1, tex);
      glBindTexture(hcgpu::kTexture2D, *tex);
      glTexParameteri(hcgpu::kTexture2D, hcgpu::kMinFilter, hcgpu::kLinear);
      glTexParameteri(hcgpu::kTexture2D, hcgpu::kMagFilter, hcgpu::kLinear);
      glTexParameteri(hcgpu::kTexture2D, hcgpu::kWrapS, hcgpu::kClampToEdge);
      glTexParameteri(hcgpu::kTexture2D, hcgpu::kWrapT, hcgpu::kClampToEdge);
    }
    glBindTexture(hcgpu::kTexture2D, *tex);
    glPixelStorei(hcgpu::kUnpackAlignment, 4);
    glTexImage2D(hcgpu::kTexture2D, 0, hcgpu::kRGBA, static_cast<int>(p->width),
                 static_cast<int>(p->height), 0, hcgpu::kRGBA, hcgpu::kUByte, p->buffer);
    *b = static_cast<int>(p->width);
    *h = static_cast<int>(p->height);
  }
};

// Je Flutter-Kontext ein Gpu; lebt bis zum Prozessende (die Texturen der
// Engine können ihn noch referenzieren, wenn sie abgebaut wird).
Gpu* GpuFuer(void* flutter_ctx) {
  static std::mutex m;
  static std::vector<Gpu*>* alle = new std::vector<Gpu*>();
  std::lock_guard<std::mutex> l(m);
  for (Gpu* g : *alle)
    if (g->flutter == flutter_ctx) return g;
  Gpu* g = new Gpu();
  g->Einrichten(flutter_ctx);
  alle->push_back(g);
  return g;
}

void* FlutterKontext() {
  static void* (*aktuell)() = [] {
    void* h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libEGL.so.1", RTLD_NOW);
    return h ? reinterpret_cast<void* (*)()>(dlsym(h, "eglGetCurrentContext")) : nullptr;
  }();
  return aktuell ? aktuell() : nullptr;
}

}  // namespace

struct HcGlTextur {
  FlTextureGL parent_instance;
  flutter::TextureVariant* variante = nullptr;
  Gpu* gpu = nullptr;  // der Kontext-Gruppe, in der tex lebt
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
  if (!self->gpu) self->gpu = GpuFuer(FlutterKontext());
  Gpu& g = *self->gpu;
  auto& pb = std::get<flutter::PixelBufferTexture>(*self->variante);
  // Kachelgröße (Pixel), wie sie Flutter fordert — dorthin wird verkleinert.
  const int max_b = static_cast<int>(*width), max_h = static_cast<int>(*height);
  g.Aufraeumen();

  bool fertig = false;
  if (g.gpu_an) {
    hcgpu::Bild b;
    if (pb.GpuBild(&b)) {
      // Nur für GPU-Bilder in den eigenen Kontext.
      if (!g.Betreten()) {
        hcgpu::FdsSchliessen(b);
        g.Aufgeben("GL-Kontextwechsel scheitert");
      } else {
        fertig = g.z.Zeichnen(b, &self->tex, &self->breite, &self->hoehe, max_b, max_h);
        const char* grund = g.z.letzter_fehler();
        if (!g.Verlassen()) {
          fertig = false;
          g.Aufgeben("GL-Kontextwechsel zurück scheitert");
        } else if (fertig) {
          g.fehler_in_folge = 0;
        } else if (++g.fehler_in_folge == 1) {
          g_message("HoneyCord: Empfang ohne Umweg — Bild nicht zu zeichnen (%s), dieses Bild über den Pixelweg", grund);
        } else if (g.fehler_in_folge >= kFehlerBremse) {
          g.Aufgeben("Zeichnen scheitert wiederholt");
        }
      }
    }
  }
  if (!fertig && g.PixelwegBereit()) {
    // Pixelweg in Flutters Kontext — wie Flutters eigene Pixelpuffer-Textur.
    const FlutterDesktopPixelBuffer* p = pb.CopyPixelBuffer(*width, *height);
    if (p && p->buffer && p->width > 0 && p->height > 0) {
      g.Hochladen(p, &self->tex, &self->breite, &self->hoehe);
    } else if (!self->tex) {
      // Noch kein Bild: durchsichtig statt eines Fehlers je Bild.
      static const uint8_t leer[4] = {0, 0, 0, 0};
      static const FlutterDesktopPixelBuffer kLeer = {leer, 1, 1, nullptr, nullptr};
      g.Hochladen(&kLeer, &self->tex, &self->breite, &self->hoehe);
    }
  }
  if (!self->tex) {
    g_set_error(error, g_quark_from_static_string("honeycord"), 1, "keine GL-Textur");
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
  if (self->tex && self->gpu) {
    // Nicht hier löschen: kein GL-Kontext auf diesem Faden.
    std::lock_guard<std::mutex> l(self->gpu->weg_m);
    self->gpu->zu_loeschen.push_back(self->tex);
  }
  self->tex = 0;
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
