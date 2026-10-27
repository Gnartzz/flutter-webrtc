// HoneyCord 27.10.2026 (Absturzfamilie „Beobachter ueberlebt sein Objekt
// nicht“, okunoku SIGABRT beim Schliessen): Beobachter OHNE Wirkung, die nie
// zerstoert werden.
//
// Warum nicht einfach nullptr: Die Aufrufstellen in der libwebrtc-DLL lesen
// `observer_` ohne Sperre, und RTCDesktopCapturerImpl prueft `if (observer_)`
// auf dem Aufnahme-Thread, liest den Zeiger dann aber IM Lambda auf dem
// Signalisierungs-Thread erneut (rtc_desktop_capturer_impl.cc, OnCaptureResult/
// Start/Stop). Wird zwischen Pruefung und Aufruf auf nullptr gesetzt, ruft er
// auf nullptr. Ein Abmelden auf diese Objekte statt auf nullptr schliesst das
// Fenster ganz: Ein verspaeteter Rueckruf trifft immer ein gueltiges Objekt,
// das nichts tut. Eine neue DLL ist dafuer nicht noetig.
//
// Die Objekte werden absichtlich nie freigegeben (`new` ohne `delete`), damit
// auch Rueckrufe waehrend der atexit-Phase noch ein gueltiges Ziel haben.
#ifndef HONEYCORD_INERT_OBSERVERS_H_
#define HONEYCORD_INERT_OBSERVERS_H_

#include "rtc_data_channel.h"
#include "rtc_desktop_capturer.h"
#include "rtc_peerconnection.h"

namespace flutter_webrtc_plugin {
namespace inert {

class InertPeerConnectionObserver : public libwebrtc::RTCPeerConnectionObserver {
 public:
  void OnSignalingState(libwebrtc::RTCSignalingState) override {}
  void OnPeerConnectionState(libwebrtc::RTCPeerConnectionState) override {}
  void OnIceGatheringState(libwebrtc::RTCIceGatheringState) override {}
  void OnIceConnectionState(libwebrtc::RTCIceConnectionState) override {}
  void OnIceCandidate(
      libwebrtc::scoped_refptr<libwebrtc::RTCIceCandidate>) override {}
  void OnAddStream(
      libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
  void OnRemoveStream(
      libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>) override {}
  void OnDataChannel(
      libwebrtc::scoped_refptr<libwebrtc::RTCDataChannel>) override {}
  void OnRenegotiationNeeded() override {}
  void OnTrack(
      libwebrtc::scoped_refptr<libwebrtc::RTCRtpTransceiver>) override {}
  void OnAddTrack(
      libwebrtc::vector<libwebrtc::scoped_refptr<libwebrtc::RTCMediaStream>>,
      libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}
  void OnRemoveTrack(
      libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>) override {}
};

class InertDataChannelObserver : public libwebrtc::RTCDataChannelObserver {
 public:
  void OnStateChange(libwebrtc::RTCDataChannelState) override {}
  void OnMessage(const char*, int, bool) override {}
};

class InertDesktopCapturerObserver : public libwebrtc::DesktopCapturerObserver {
 public:
  void OnStart(libwebrtc::scoped_refptr<libwebrtc::RTCDesktopCapturer>) override {}
  void OnPaused(libwebrtc::scoped_refptr<libwebrtc::RTCDesktopCapturer>) override {}
  void OnStop(libwebrtc::scoped_refptr<libwebrtc::RTCDesktopCapturer>) override {}
  void OnError(libwebrtc::scoped_refptr<libwebrtc::RTCDesktopCapturer>) override {}
};

inline libwebrtc::RTCPeerConnectionObserver* InertPeerConnection() {
  static auto* inert = new InertPeerConnectionObserver();
  return inert;
}

inline libwebrtc::RTCDataChannelObserver* InertDataChannel() {
  static auto* inert = new InertDataChannelObserver();
  return inert;
}

inline libwebrtc::DesktopCapturerObserver* InertDesktopCapturer() {
  static auto* inert = new InertDesktopCapturerObserver();
  return inert;
}

}  // namespace inert
}  // namespace flutter_webrtc_plugin

#endif  // HONEYCORD_INERT_OBSERVERS_H_
