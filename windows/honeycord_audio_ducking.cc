// HoneyCord Windows-Audio-Ducking-Opt-out — Implementierung. Siehe Header.
#include "honeycord_audio_ducking.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl/client.h>

#include <atomic>
#include <thread>

namespace honeycord {
namespace {

using Microsoft::WRL::ComPtr;

// Fuer EIN Datenfluss (eCapture = Mikro, eRender = Wiedergabe) das Standard-
// Kommunikationsgeraet holen und dessen Prozess-Standard-Session (GUID NULL,
// dieselbe die webrtcs ADM nutzt) vom Ducking abmelden.
void OptOutOne(IMMDeviceEnumerator* enumerator, EDataFlow flow) {
  ComPtr<IMMDevice> device;
  // eCommunications: genau die Rolle, ueber die libwebrtcs ADM das Geraet waehlt.
  if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eCommunications, &device)) ||
      !device) {
    return;
  }
  ComPtr<IAudioSessionManager2> manager;
  if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                              nullptr, &manager)) ||
      !manager) {
    return;
  }
  ComPtr<IAudioSessionControl> control;
  // sessionGuid = NULL, flags = 0 -> Prozess-Standard-Session (identisch zu
  // webrtcs IAudioClient::Initialize(..., NULL)).
  if (FAILED(manager->GetAudioSessionControl(nullptr, 0, &control)) || !control) {
    return;
  }
  ComPtr<IAudioSessionControl2> control2;
  if (FAILED(control.As(&control2)) || !control2) {
    return;
  }
  // TRUE = diese Session opt-tet aus dem Ducking aus (duckt nichts, wird nicht
  // gedueckt). Muss vor IAudioClient::Start gesetzt sein -> daher Plugin-Init.
  control2->SetDuckingPreference(TRUE);
}

}  // namespace

void OptOutCommunicationDuckingOnce() {
  static std::atomic<bool> done{false};
  if (done.exchange(true)) return;

  // Auf eigenem Thread mit eigener COM-Apartment, damit wir das Threading-Modell
  // des Aufrufers nicht stoeren.
  std::thread([] {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
        enumerator) {
      OptOutOne(enumerator.Get(), eCapture);  // Mikro = der Ducking-Ausloeser
      OptOutOne(enumerator.Get(), eRender);   // Wiedergabe zur Sicherheit mit
    }
    enumerator.Reset();
    if (SUCCEEDED(co)) CoUninitialize();
  }).detach();
}

}  // namespace honeycord
