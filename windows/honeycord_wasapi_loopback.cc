// HoneyCord WASAPI-Loopback — Implementierung. Siehe Header.
#include "honeycord_wasapi_loopback.h"

// WinSDK 10.0.26100 hat in `Functiondiscoverykeys_devpkey.h` den Include von
// `devpropdef.h` (definiert DEFINE_PROPERTYKEY) auskommentiert. Wir ziehen
// es manuell vor mmdeviceapi.h ein, sonst bricht der Compile mit
// "DEFINE_PROPERTYKEY: Mehrfachinitialisierung / nichtdeklarierter Bezeichner".
#include <objbase.h>
#include <devpropdef.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

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

}  // namespace

WasapiLoopback::WasapiLoopback(
    libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source)
    : source_(std::move(source)) {}

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
  if (FAILED(hr)) return false;

  hr = client_->GetService(kIID_IAudioCaptureClient,
                           reinterpret_cast<void**>(&capture_));
  HcLog("Start: GetService(IAudioCaptureClient) = 0x%08lx capture=%p",
        (long)hr, capture_);
  if (FAILED(hr) || !capture_) return false;

  hr = client_->Start();
  HcLog("Start: client->Start = 0x%08lx", (long)hr);
  if (FAILED(hr)) return false;

  sample_rate_ = static_cast<int>(mix_format_->nSamplesPerSec);
  channels_ = std::min<int>(2, mix_format_->nChannels);
  input_is_float_ = IsFloatFormat(mix_format_);
  chunk_frames_ = sample_rate_ / 100;   // 10 ms
  chunk_buf_.assign(chunk_frames_ * channels_, 0);
  chunk_filled_frames_ = 0;
  HcLog("Start: ready sr=%d out_ch=%d float=%d chunk_frames=%zu",
        sample_rate_, channels_, input_is_float_ ? 1 : 0, chunk_frames_);

  running_ = true;
  thread_ = std::thread(&WasapiLoopback::CaptureLoop, this);
  HcLog("Start: capture thread launched");
  return true;
}

void WasapiLoopback::Stop() {
  bool was_running = running_.exchange(false);
  HcLog("Stop: was_running=%d", was_running ? 1 : 0);
  if (thread_.joinable()) thread_.join();
  if (client_) {
    client_->Stop();
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

  while (running_) {
    ++loop_iters;
    UINT32 packet_size = 0;
    if (FAILED(capture_->GetNextPacketSize(&packet_size))) {
      Sleep(2);
      continue;
    }
    if (packet_size == 0) {
      ++empty_polls;
      Sleep(2);
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

    // Pro Input-Frame: in den 10-ms-Akkumulator schreiben (mit Down-Mix
    // auf 2 Kanäle falls Quelle mehr hat). Drainen, sobald 10 ms voll.
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
          // unbekanntes Format → silence
          left = 0;
          right = 0;
        }
      }
      int16_t* out = chunk_buf_.data() + chunk_filled_frames_ * channels_;
      out[0] = left;
      if (channels_ >= 2) out[1] = right;
      ++chunk_filled_frames_;
      if (chunk_filled_frames_ >= chunk_frames_) {
        if (source_.get()) {
          source_->CaptureFrame(chunk_buf_.data(), 16, sample_rate_,
                                static_cast<size_t>(channels_), chunk_frames_);
          ++pushed_chunks;
        } else if (pushed_chunks == 0) {
          HcLog("CaptureLoop: source_ ist nullptr, kann nicht pushen");
        }
        chunk_filled_frames_ = 0;
      }
    }
    capture_->ReleaseBuffer(num_frames);
  }
  HcLog("CaptureLoop: exiting (iters=%llu pushed=%llu in_frames=%llu)",
        (unsigned long long)loop_iters, (unsigned long long)pushed_chunks,
        (unsigned long long)total_in_frames);
}

}  // namespace honeycord
