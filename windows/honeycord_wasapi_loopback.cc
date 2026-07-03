// HoneyCord WASAPI-Loopback — Implementierung. Siehe Header.
// Wir nutzen ein einfaches File-Log via fopen/strncat — der Plugin-Build
// laeuft mit /WX (warnings-as-errors); deshalb hier _CRT_SECURE_NO_WARNINGS
// statt die Variante mit den _s-Helpern, die unter MSYS/clang-cl Probleme
// machen.
#define _CRT_SECURE_NO_WARNINGS 1
#include "honeycord_wasapi_loopback.h"

// WinSDK 10.0.26100 hat in `Functiondiscoverykeys_devpkey.h` den Include von
// `devpropdef.h` (definiert DEFINE_PROPERTYKEY) auskommentiert. Wir ziehen
// es manuell vor mmdeviceapi.h ein, sonst bricht der Compile mit
// "DEFINE_PROPERTYKEY: Mehrfachinitialisierung / nichtdeklarierter Bezeichner".
#include <objbase.h>
#include <devpropdef.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
// Process-Loopback-Aktivierung (AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
// VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK) — WinSDK >= 10.0.19041.
#include <audioclientactivationparams.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

// Log-Rotation: haelt ein Log unter ~1 MB. Voll -> nach <path>.old verschoben
// (ersetzt das vorherige Archiv); ein Archiv aelter als 14 Tage wird geloescht.
// Einmal pro Prozess und Log gerufen — der Zuwachs einer Session ist klein,
// die Rotation verhindert nur das Anwachsen ueber viele Sessions (Datenmuell).
static void RotateLogIfNeededA(const char* path) {
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

// Diagnose-Logger nach %TEMP%\honeycord-wasapi.log. WASAPI auf Win laeuft
// im Plugin-Prozess ohne stdout/file-log; OutputDebugString koennte mit
// DebugView gelesen werden, ein simples File-Log ist aber robuster (auch
// wenn DebugView nicht offen ist).
//
// Implementiert als statische Funktion, plus extern-Wrapper
// honeycord::WasapiLogFromPlugin (siehe Header) damit auch das
// Plugin-Glue von ausserhalb dieses TU loggen kann.
static void HcLogV(const char* fmt, va_list ap) {
  static std::mutex mu;
  static FILE* f = nullptr;
  std::lock_guard<std::mutex> lock(mu);
  if (!f) {
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) return;
    std::strncat(path, "honeycord-wasapi.log", MAX_PATH - n - 1);
    RotateLogIfNeededA(path);
    f = std::fopen(path, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(f,
                 "\n=== HoneyCord WASAPI log opened %04d-%02d-%02d %02d:%02d:%02d ===\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond);
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::fprintf(f, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds);
  std::vfprintf(f, fmt, ap);
  std::fputc('\n', f);
  std::fflush(f);
}

static void HcLog(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  HcLogV(fmt, ap);
  va_end(ap);
}

namespace honeycord {
void WasapiLogFromPlugin(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  HcLogV(fmt, ap);
  va_end(ap);
}
}  // namespace honeycord

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT braucht ksmedia.h, aber das definiert
// PROPERTYKEY-Konstanten doppelt mit dem WinSDK 10.0.26100. Wir definieren
// das einzige Subformat das wir brauchen manuell und sparen uns ksmedia.h.
#ifndef HONEYCORD_DEFINE_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
#define HONEYCORD_DEFINE_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
static const GUID HC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
#endif

namespace honeycord {

namespace {

const CLSID kCLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID kIID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID kIID_IAudioClient = __uuidof(IAudioClient);
const IID kIID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

// WASAPI gibt das Render-Mix-Format zurück; bei modernen Windows-Endpoints
// üblicherweise WAVE_FORMAT_EXTENSIBLE mit Subformat IEEE-Float (32 Bit) und
// 2 Kanälen. Helper, der entscheidet, ob der Buffer als float oder int
// interpretiert werden muss.
bool IsFloatFormat(const WAVEFORMATEX* fmt) {
  if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
  if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
    return std::memcmp(&ext->SubFormat, &HC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
                       sizeof(GUID)) == 0;
  }
  return false;
}

inline int16_t SaturatingFloatToInt16(float s) {
  if (s > 1.0f) s = 1.0f;
  if (s < -1.0f) s = -1.0f;
  return static_cast<int16_t>(s * 32767.0f);
}

// Minimaler Completion-Handler fuer ActivateAudioInterfaceAsync. IAgileObject
// als Marker, damit der Callback ohne COM-Marshalling aus jedem Apartment
// feuern darf (sonst haengt die Aktivierung je nach Thread-Kontext).
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler,
                        public IAgileObject {
 public:
  ActivateHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
      *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
    } else if (riid == __uuidof(IAgileObject)) {
      *ppv = static_cast<IAgileObject*>(this);
    } else {
      *ppv = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    ULONG r = --ref_;
    if (r == 0) delete this;
    return r;
  }

  // IActivateAudioInterfaceCompletionHandler
  STDMETHODIMP ActivateCompleted(
      IActivateAudioInterfaceAsyncOperation* /*op*/) override {
    if (done_) SetEvent(done_);
    return S_OK;
  }

  bool Wait(DWORD timeout_ms) {
    return done_ && WaitForSingleObject(done_, timeout_ms) == WAIT_OBJECT_0;
  }

 private:
  ~ActivateHandler() {
    if (done_) CloseHandle(done_);
  }
  std::atomic<ULONG> ref_{1};
  HANDLE done_ = nullptr;
};

}  // namespace

WasapiLoopback::WasapiLoopback(
    libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source,
    unsigned long include_pid)
    : source_(std::move(source)), include_pid_(include_pid) {}

WasapiLoopback::~WasapiLoopback() { Stop(); }

bool WasapiLoopback::Start() {
  HcLog("Start: called (running=%d, source=%p)", running_.load(),
        source_.get());
  if (running_) return true;
  if (!source_.get()) {
    HcLog("Start: ABBRUCH source=nullptr");
    return false;
  }
  // CoInitialize: Plugin lebt im Flutter-Engine-Thread, dort ist COM
  // i. d. R. schon initialisiert. Wir machen einen lokalen Call, das ist
  // refcounted und schadet nicht.
  HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  HcLog("Start: CoInitializeEx = 0x%08lx", (long)co_hr);

  // Stufenkette:
  // 1) include_pid_ gesetzt (Fenster-/Game-Share): NUR das Audio der
  //    geteilten App (id16 Per-App-Audio).
  // 2) Process-Loopback MIT Selbst-Ausschluss = System-Mix ohne uns
  //    (id13-Echo-Fix; Standard fuer Screen-Shares).
  // 3) Fallback: klassisches Endpoint-Loopback (Status quo inkl. eigener
  //    Wiedergabe) — lieber Echo als gar kein Stream-Audio.
  const char* mode_desc = nullptr;
  bool ok = false;
  if (include_pid_ != 0) {
    ok = TryStartProcessLoopback(/*include_mode=*/true);
    if (ok) {
      mode_desc = "process-include (nur geteilte App)";
    } else {
      ReleaseCom();
      HcLog("Start: include-Modus (pid=%lu) fehlgeschlagen -> exclude",
            include_pid_);
    }
  }
  if (!ok) {
    ok = TryStartProcessLoopback(/*include_mode=*/false);
    if (ok) {
      mode_desc = "process-exclude (ohne eigene Wiedergabe)";
    } else {
      ReleaseCom();
      HcLog("Start: FALLBACK auf Endpoint-Loopback (inkl. eigener Wiedergabe!)");
      if (!StartEndpointLoopback()) {
        ReleaseCom();
        return false;
      }
      mode_desc = "endpoint-fallback";
    }
  }
  HcLog("Start: Loopback-Modus = %s", mode_desc);

  HRESULT hr = client_->GetService(kIID_IAudioCaptureClient,
                                   reinterpret_cast<void**>(&capture_));
  HcLog("Start: GetService(IAudioCaptureClient) = 0x%08lx capture=%p",
        (long)hr, capture_);
  if (FAILED(hr) || !capture_) return false;

  hr = client_->Start();
  HcLog("Start: client->Start = 0x%08lx", (long)hr);
  if (FAILED(hr)) return false;

  in_sample_rate_ = static_cast<int>(mix_format_->nSamplesPerSec);
  // libwebrtc/Opus akzeptiert keine 44100 Hz; wir resampeln intern auf
  // 48 kHz. Bei einem Input-Format das eh schon 48 kHz ist (z. B. wenn
  // der Nutzer 48-kHz-Default-Render hat), wird das Resampling zu einer
  // 1:1-Kopie.
  out_sample_rate_ = 48000;
  channels_ = std::min<int>(2, mix_format_->nChannels);
  input_is_float_ = IsFloatFormat(mix_format_);
  chunk_frames_ = out_sample_rate_ / 100;   // 480 frames @ 48 kHz = 10 ms
  chunk_buf_.assign(chunk_frames_ * channels_, 0);
  chunk_filled_frames_ = 0;
  resample_phase_ = 0.0;
  prev_left_ = 0;
  prev_right_ = 0;
  HcLog("Start: ready in_sr=%d out_sr=%d ch=%d float=%d chunk_frames=%zu",
        in_sample_rate_, out_sample_rate_, channels_,
        input_is_float_ ? 1 : 0, chunk_frames_);

  running_ = true;
  thread_ = std::thread(&WasapiLoopback::CaptureLoop, this);
  HcLog("Start: capture thread launched");
  return true;
}

bool WasapiLoopback::TryStartProcessLoopback(bool include_mode) {
  // Virtuelles Loopback-Device. include_mode=false: ALLES System-Audio AUSSER
  // dem eigenen Prozessbaum (eigene Voice-Wiedergabe, Soundboard, Notifys ->
  // genau das, was NICHT in den Stream soll). include_mode=true: NUR der
  // Prozessbaum der geteilten App (Per-App-Audio beim Fenster-/Game-Share).
  AUDIOCLIENT_ACTIVATION_PARAMS params = {};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params.ProcessLoopbackParams.TargetProcessId =
      include_mode ? static_cast<DWORD>(include_pid_) : GetCurrentProcessId();
  params.ProcessLoopbackParams.ProcessLoopbackMode =
      include_mode ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                   : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
  HcLog("ProcLoopback: Modus=%s targetPid=%lu",
        include_mode ? "INCLUDE" : "EXCLUDE",
        (unsigned long)params.ProcessLoopbackParams.TargetProcessId);
  PROPVARIANT pv = {};
  pv.vt = VT_BLOB;
  pv.blob.cbSize = sizeof(params);
  pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

  ActivateHandler* handler = new ActivateHandler();
  IActivateAudioInterfaceAsyncOperation* op = nullptr;
  HRESULT hr = ActivateAudioInterfaceAsync(
      VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, kIID_IAudioClient, &pv, handler,
      &op);
  HcLog("ProcLoopback: ActivateAudioInterfaceAsync = 0x%08lx op=%p", (long)hr,
        op);
  if (FAILED(hr) || !op) {
    if (op) op->Release();
    handler->Release();
    return false;
  }
  if (!handler->Wait(2000)) {
    HcLog("ProcLoopback: Aktivierung TIMEOUT (2s)");
    op->Release();
    handler->Release();
    return false;
  }
  HRESULT hr_activate = E_FAIL;
  IUnknown* unk = nullptr;
  hr = op->GetActivateResult(&hr_activate, &unk);
  op->Release();
  handler->Release();
  HcLog("ProcLoopback: GetActivateResult = 0x%08lx / activate=0x%08lx unk=%p",
        (long)hr, (long)hr_activate, unk);
  if (FAILED(hr) || FAILED(hr_activate) || !unk) {
    if (unk) unk->Release();
    return false;
  }
  hr = unk->QueryInterface(kIID_IAudioClient,
                           reinterpret_cast<void**>(&client_));
  unk->Release();
  HcLog("ProcLoopback: QI(IAudioClient) = 0x%08lx client=%p", (long)hr,
        client_);
  if (FAILED(hr) || !client_) return false;

  // Das virtuelle Device hat KEIN GetMixFormat — das Format geben WIR vor.
  // 48 kHz/16 Bit/stereo = exakt das libwebrtc-Zielformat -> der
  // Linear-Resampler im CaptureLoop wird zur 1:1-Kopie.
  mix_format_ =
      static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
  if (!mix_format_) return false;
  std::memset(mix_format_, 0, sizeof(WAVEFORMATEX));
  mix_format_->wFormatTag = WAVE_FORMAT_PCM;
  mix_format_->nChannels = 2;
  mix_format_->nSamplesPerSec = 48000;
  mix_format_->wBitsPerSample = 16;
  mix_format_->nBlockAlign = 4;                 // 2 ch * 2 Byte
  mix_format_->nAvgBytesPerSec = 48000 * 4;
  mix_format_->cbSize = 0;

  // Process-Loopback verlangt Event-getriebenes Capture (EVENTCALLBACK).
  const REFERENCE_TIME kBufferDuration100Ns = 200 * 10000;  // 200 ms
  hr = client_->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
      kBufferDuration100Ns, 0, mix_format_, nullptr);
  HcLog("ProcLoopback: Initialize(LOOPBACK|EVENT, 48k/16/2) = 0x%08lx",
        (long)hr);
  if (FAILED(hr)) return false;

  capture_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!capture_event_) {
    HcLog("ProcLoopback: CreateEvent fehlgeschlagen");
    return false;
  }
  hr = client_->SetEventHandle(static_cast<HANDLE>(capture_event_));
  HcLog("ProcLoopback: SetEventHandle = 0x%08lx", (long)hr);
  return SUCCEEDED(hr);
}

bool WasapiLoopback::StartEndpointLoopback() {
  HRESULT hr;
  IMMDeviceEnumerator* enumerator = nullptr;
  hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                        kIID_IMMDeviceEnumerator,
                        reinterpret_cast<void**>(&enumerator));
  HcLog("Start: CoCreateInstance(MMDeviceEnumerator) = 0x%08lx enum=%p",
        (long)hr, enumerator);
  if (FAILED(hr) || !enumerator) return false;
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
  enumerator->Release();
  HcLog("Start: GetDefaultAudioEndpoint(eRender) = 0x%08lx dev=%p",
        (long)hr, device_);
  if (FAILED(hr) || !device_) return false;

  hr = device_->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr,
                         reinterpret_cast<void**>(&client_));
  HcLog("Start: device->Activate(IAudioClient) = 0x%08lx client=%p",
        (long)hr, client_);
  if (FAILED(hr) || !client_) return false;

  hr = client_->GetMixFormat(&mix_format_);
  HcLog("Start: GetMixFormat = 0x%08lx fmt=%p", (long)hr, mix_format_);
  if (FAILED(hr) || !mix_format_) return false;
  HcLog("Start: mix_format: tag=0x%x ch=%u sr=%u bits=%u",
        (unsigned)mix_format_->wFormatTag, (unsigned)mix_format_->nChannels,
        (unsigned)mix_format_->nSamplesPerSec,
        (unsigned)mix_format_->wBitsPerSample);

  // Capture-Puffergröße 200 ms — großzügig, damit kein Overrun bei kurzer
  // Thread-Verzögerung.
  const REFERENCE_TIME kBufferDuration100Ns = 200 * 10000;
  hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                           AUDCLNT_STREAMFLAGS_LOOPBACK, kBufferDuration100Ns,
                           0, mix_format_, nullptr);
  HcLog("Start: client->Initialize(LOOPBACK) = 0x%08lx", (long)hr);
  return SUCCEEDED(hr);
}

void WasapiLoopback::ReleaseCom() {
  if (client_) {
    client_->Release();
    client_ = nullptr;
  }
  if (capture_) {
    capture_->Release();
    capture_ = nullptr;
  }
  if (device_) {
    device_->Release();
    device_ = nullptr;
  }
  if (mix_format_) {
    CoTaskMemFree(mix_format_);
    mix_format_ = nullptr;
  }
  if (capture_event_) {
    CloseHandle(static_cast<HANDLE>(capture_event_));
    capture_event_ = nullptr;
  }
}

void WasapiLoopback::Stop() {
  bool was_running = running_.exchange(false);
  HcLog("Stop: was_running=%d", was_running ? 1 : 0);
  if (thread_.joinable()) thread_.join();
  if (client_) client_->Stop();
  ReleaseCom();
  // CoInitialize hatten wir per-Aufrufer refcounted gemacht; das passende
  // CoUninitialize lassen wir weg, weil der Plugin-Thread sonst andere
  // COM-Nutzer stört.
  (void)was_running;
}

void WasapiLoopback::CaptureLoop() {
  HcLog("CaptureLoop: entered");
  const size_t in_channels = mix_format_ ? mix_format_->nChannels : 0;
  const size_t bytes_per_sample =
      mix_format_ ? mix_format_->wBitsPerSample / 8 : 0;
  if (in_channels == 0 || bytes_per_sample == 0) {
    HcLog("CaptureLoop: ABBRUCH in_channels=%zu bps=%zu", in_channels,
          bytes_per_sample);
    return;
  }
  const size_t in_frame_bytes = in_channels * bytes_per_sample;

  uint64_t loop_iters = 0;
  uint64_t pushed_chunks = 0;
  uint64_t total_in_frames = 0;
  uint64_t silent_packets = 0;
  uint64_t empty_polls = 0;
  uint64_t last_log_ms = GetTickCount64();

  // Event-Modus: nur warten, wenn der Puffer LEER war — nach einem
  // verarbeiteten Paket koennten weitere anstehen (Auto-Reset-Event ist dann
  // schon konsumiert; erneutes Warten wuerde bis 100 ms stottern).
  bool wait_next = true;
  while (running_) {
    ++loop_iters;
    // Process-Loopback-Modus ist Event-getrieben: aufs Capture-Event warten
    // (Timeout als Watchdog), dann unten die anstehenden Pakete abholen.
    // Fallback-Modus pollt wie bisher (Sleep unten bei leerem Puffer).
    if (capture_event_ && wait_next) {
      WaitForSingleObject(static_cast<HANDLE>(capture_event_), 100);
      if (!running_) break;
    }
    UINT32 packet_size = 0;
    if (FAILED(capture_->GetNextPacketSize(&packet_size))) {
      Sleep(2);
      continue;
    }
    if (packet_size == 0) {
      ++empty_polls;
      wait_next = true;
      if (!capture_event_) Sleep(2);
      // Periodisches "lebt noch"-Log alle 2 s, damit man auch dann sieht
      // dass der Thread laeuft wenn gerade nichts spielt.
      uint64_t now = GetTickCount64();
      if (now - last_log_ms > 2000) {
        HcLog("CaptureLoop tick: iters=%llu empty=%llu silent=%llu "
              "in_frames=%llu pushed=%llu",
              (unsigned long long)loop_iters,
              (unsigned long long)empty_polls,
              (unsigned long long)silent_packets,
              (unsigned long long)total_in_frames,
              (unsigned long long)pushed_chunks);
        last_log_ms = now;
      }
      continue;
    }
    wait_next = false;  // Paket da -> danach ohne Warten weiter drainen
    BYTE* data = nullptr;
    UINT32 num_frames = 0;
    DWORD flags = 0;
    HRESULT hr = capture_->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
    if (FAILED(hr) || num_frames == 0) {
      HcLog("CaptureLoop: GetBuffer hr=0x%08lx num_frames=%u", (long)hr,
            (unsigned)num_frames);
      if (SUCCEEDED(hr)) capture_->ReleaseBuffer(num_frames);
      continue;
    }
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || data == nullptr;
    if (silent) ++silent_packets;
    total_in_frames += num_frames;

    // Linear-Interpolation-Resampler. Pro Input-Frame skalieren wir den
    // Phasen-Zähler um (in_sr / out_sr); jedes Mal wenn Phase >= 1 wird,
    // emittieren wir ein Output-Sample, das eine lineare Mischung von
    // (prev, current) ist. Phase bleibt zwischen Calls erhalten, damit
    // 44100→48000 sauber durchläuft (Verhältnis = 0.91875).
    const double step = static_cast<double>(in_sample_rate_) /
                        static_cast<double>(out_sample_rate_);
    for (UINT32 f = 0; f < num_frames; ++f) {
      const BYTE* in_frame = data + f * in_frame_bytes;
      int16_t left = 0, right = 0;
      if (!silent) {
        if (input_is_float_) {
          const float* fl = reinterpret_cast<const float*>(in_frame);
          float l = fl[0];
          float r = (in_channels >= 2) ? fl[1] : l;
          left = SaturatingFloatToInt16(l);
          right = SaturatingFloatToInt16(r);
        } else if (bytes_per_sample == 2) {
          const int16_t* s = reinterpret_cast<const int16_t*>(in_frame);
          left = s[0];
          right = (in_channels >= 2) ? s[1] : s[0];
        } else {
          left = 0;
          right = 0;
        }
      }
      // Phase < 1 → kein Output dieses Frame; >= 1 → eines (oder mehrere)
      // Output-Samples zwischen prev und current interpolieren.
      resample_phase_ += 1.0;
      while (resample_phase_ >= step) {
        resample_phase_ -= step;
        const double t = 1.0 - (resample_phase_ / step);  // 0..1
        const double out_l = prev_left_ + t * (left - prev_left_);
        const double out_r = prev_right_ + t * (right - prev_right_);
        int16_t* out = chunk_buf_.data() + chunk_filled_frames_ * channels_;
        out[0] = static_cast<int16_t>(out_l);
        if (channels_ >= 2) out[1] = static_cast<int16_t>(out_r);
        ++chunk_filled_frames_;
        if (chunk_filled_frames_ >= chunk_frames_) {
          if (source_.get()) {
            source_->CaptureFrame(chunk_buf_.data(), 16, out_sample_rate_,
                                  static_cast<size_t>(channels_),
                                  chunk_frames_);
            ++pushed_chunks;
          } else if (pushed_chunks == 0) {
            HcLog("CaptureLoop: source_ ist nullptr, kann nicht pushen");
          }
          chunk_filled_frames_ = 0;
        }
      }
      prev_left_ = left;
      prev_right_ = right;
    }
    capture_->ReleaseBuffer(num_frames);
  }
  HcLog("CaptureLoop: exiting (iters=%llu pushed=%llu in_frames=%llu)",
        (unsigned long long)loop_iters, (unsigned long long)pushed_chunks,
        (unsigned long long)total_in_frames);
}

}  // namespace honeycord
