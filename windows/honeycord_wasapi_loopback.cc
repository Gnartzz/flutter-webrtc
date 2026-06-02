// HoneyCord WASAPI-Loopback — Implementierung. Siehe Header.
#include "honeycord_wasapi_loopback.h"

#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <Mmdeviceapi.h>
#include <combaseapi.h>
#include <ksmedia.h>
#include <objbase.h>

#include <algorithm>
#include <cstring>

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
    return ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
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
  if (running_) return true;
  // CoInitialize: Plugin lebt im Flutter-Engine-Thread, dort ist COM
  // i. d. R. schon initialisiert. Wir machen einen lokalen Call, das ist
  // refcounted und schadet nicht.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  HRESULT hr;
  IMMDeviceEnumerator* enumerator = nullptr;
  hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                        kIID_IMMDeviceEnumerator,
                        reinterpret_cast<void**>(&enumerator));
  if (FAILED(hr) || !enumerator) return false;
  hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
  enumerator->Release();
  if (FAILED(hr) || !device_) return false;

  hr = device_->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr,
                         reinterpret_cast<void**>(&client_));
  if (FAILED(hr) || !client_) return false;

  hr = client_->GetMixFormat(&mix_format_);
  if (FAILED(hr) || !mix_format_) return false;

  // Capture-Puffergröße 200 ms — großzügig, damit kein Overrun bei kurzer
  // Thread-Verzögerung.
  const REFERENCE_TIME kBufferDuration100Ns = 200 * 10000;
  hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                           AUDCLNT_STREAMFLAGS_LOOPBACK, kBufferDuration100Ns,
                           0, mix_format_, nullptr);
  if (FAILED(hr)) return false;

  hr = client_->GetService(kIID_IAudioCaptureClient,
                           reinterpret_cast<void**>(&capture_));
  if (FAILED(hr) || !capture_) return false;

  hr = client_->Start();
  if (FAILED(hr)) return false;

  sample_rate_ = static_cast<int>(mix_format_->nSamplesPerSec);
  channels_ = std::min<int>(2, mix_format_->nChannels);
  input_is_float_ = IsFloatFormat(mix_format_);
  chunk_frames_ = sample_rate_ / 100;   // 10 ms
  chunk_buf_.assign(chunk_frames_ * channels_, 0);
  chunk_filled_frames_ = 0;

  running_ = true;
  thread_ = std::thread(&WasapiLoopback::CaptureLoop, this);
  return true;
}

void WasapiLoopback::Stop() {
  bool was_running = running_.exchange(false);
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
  const size_t in_channels = mix_format_ ? mix_format_->nChannels : 0;
  const size_t bytes_per_sample =
      mix_format_ ? mix_format_->wBitsPerSample / 8 : 0;
  if (in_channels == 0 || bytes_per_sample == 0) return;
  const size_t in_frame_bytes = in_channels * bytes_per_sample;

  while (running_) {
    UINT32 packet_size = 0;
    if (FAILED(capture_->GetNextPacketSize(&packet_size))) {
      Sleep(2);
      continue;
    }
    if (packet_size == 0) {
      Sleep(2);
      continue;
    }
    BYTE* data = nullptr;
    UINT32 num_frames = 0;
    DWORD flags = 0;
    HRESULT hr = capture_->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
    if (FAILED(hr) || num_frames == 0) {
      if (SUCCEEDED(hr)) capture_->ReleaseBuffer(num_frames);
      continue;
    }
    const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) || data == nullptr;

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
        if (source_) {
          source_->CaptureFrame(chunk_buf_.data(), 16, sample_rate_,
                                static_cast<size_t>(channels_), chunk_frames_);
        }
        chunk_filled_frames_ = 0;
      }
    }
    capture_->ReleaseBuffer(num_frames);
  }
}

}  // namespace honeycord
