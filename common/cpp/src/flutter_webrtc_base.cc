#include "flutter_webrtc_base.h"

#include "flutter_data_channel.h"
#include "flutter_peerconnection.h"

#include "helper.h"

#include <chrono>
#include <cstdio>
#include <ctime>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // GetCurrentProcessId, GetEnvironmentVariableA
#endif

namespace flutter_webrtc_plugin {

namespace {

// HoneyCord 27.10.2026: eine Zeile nach %LOCALAPPDATA%\HoneyCord\exit.log je
// Abbau-Schritt, damit ein Folgefall belegbar ist ("start" ohne "ende" =
// Abbau haengt oder stuerzt). Jede Zeile oeffnet und schliesst die Datei
// selbst — kein offener Griff, der den Prozess ueberleben muesste. Nur
// Windows; anderswo No-Op. Scheitert still.
void ExitLog(const char* phase,
             size_t pcs,
             size_t beobachter,
             size_t kanaele,
             long long ms) {
#if defined(_WIN32)
  char base[MAX_PATH] = {};
  DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  char path[MAX_PATH + 32] = {};
  if (std::snprintf(path, sizeof(path), "%s\\HoneyCord\\exit.log", base) <= 0)
    return;
  // Deckel 256 KB: dann neu anfangen statt endlos wachsen.
  const char* mode = "a";
  WIN32_FILE_ATTRIBUTE_DATA info;
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &info) &&
      (info.nFileSizeHigh != 0 || info.nFileSizeLow > 256 * 1024)) {
    mode = "w";
  }
  FILE* f = nullptr;
  if (fopen_s(&f, path, mode) != 0 || f == nullptr) return;
  std::time_t now = std::time(nullptr);
  std::tm tm_local = {};
  localtime_s(&tm_local, &now);
  char ts[32] = {};
  std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_local);
  std::fprintf(f,
               "[exit] %s pid=%lu %s pcs=%llu beobachter=%llu kanaele=%llu "
               "ms=%lld\n",
               ts, static_cast<unsigned long>(GetCurrentProcessId()), phase,
               static_cast<unsigned long long>(pcs),
               static_cast<unsigned long long>(beobachter),
               static_cast<unsigned long long>(kanaele), ms);
  std::fclose(f);
#else
  (void)phase;
  (void)pcs;
  (void)beobachter;
  (void)kanaele;
  (void)ms;
#endif
}

}  // namespace

const char* kEventChannelName = "FlutterWebRTC.Event";

FlutterWebRTCBase::FlutterWebRTCBase(BinaryMessenger* messenger,
                                     TextureRegistrar* textures,
                                     TaskRunner *task_runner)
    : messenger_(messenger), task_runner_(task_runner), textures_(textures) {
  LibWebRTC::Initialize();
  factory_ = LibWebRTC::CreateRTCPeerConnectionFactory();
  factory_->Initialize();
  audio_device_ = factory_->GetAudioDevice();
  video_device_ = factory_->GetVideoDevice();
  desktop_device_ = factory_->GetDesktopDevice();
  audio_processing_ = factory_->GetAudioProcessing();
  event_channel_ = EventChannelProxy::Create(messenger_, task_runner_, kEventChannelName);
}

FlutterWebRTCBase::~FlutterWebRTCBase() {
  // Normalerweise hat ~FlutterWebRTC() das schon erledigt (dort laufen noch
  // alle anderen Teilobjekte); hier nur als Netz, falls die Basis allein
  // zerstoert wird. Auf jeden Fall VOR Terminate(): Das setzt SSL und den
  // ThreadManager zurueck, und erst danach fielen sonst die PeerConnections.
  TeardownForExit();
  LibWebRTC::Terminate();
}

void FlutterWebRTCBase::TeardownForExit() {
  if (torn_down_) return;
  torn_down_ = true;

  // Befund 27.10.2026 (drei Abstuerze okunoku, 17.10. zweimal, 23.10.):
  // Bisher rief der Destruktor nur Terminate(). Danach fiel zuerst
  // `peerconnection_observers_` (Beobachter meldeten sich nicht ab), dann
  // `factory_` mit seiner eigenen PeerConnection-Liste, und
  // ~RTCPeerConnectionImpl -> Close() rief OnRemoveStream fuer jeden fremden
  // Strom auf dem geloeschten Beobachter auf -> _purecall -> abort().
  //
  // Thread: Plattform-Thread, derselbe wie RTCPeerConnectionClose() im
  // normalen Auflegen. Close() gibt den nativen PeerConnection-Proxy frei;
  // dessen Destruktor marshalt synchron auf den Signalisierungs-Thread
  // (PROXY_PRIMARY_THREAD_DESTRUCTOR). Der Signalisierungs-Thread wartet nie
  // auf den Plattform-Thread (Ereignisse gehen per PostMessage), also
  // dieselbe Verklemmungsfreiheit wie beim normalen Auflegen.
  const auto t0 = std::chrono::steady_clock::now();
  size_t kanaele = 0;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    kanaele = data_channel_observers_.size();
  }
  ExitLog("start", peerconnections_.size(), peerconnection_observers_.size(),
          kanaele, 0);

  // (1) Alle Beobachter abmelden, solange sie noch leben. Ab hier ruft
  //     libwebrtc nichts Neues mehr in Plugin-Objekte (alle Aufrufstellen in
  //     RTCPeerConnectionImpl / RTCDataChannelImpl pruefen `observer_` auf
  //     nullptr). Auch die Ereigniskanaele bekommen nichts mehr.
  for (auto& kv : peerconnection_observers_) {
    if (kv.second) kv.second->Detach();
  }
  for (auto& kv : peerconnections_) {
    // Doppelt haelt besser: falls ein Beobachter fehlt, der PC aber noch da ist.
    if (kv.second) kv.second->RegisterRTCPeerConnectionObserver(nullptr);
  }
  {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto& kv : data_channel_observers_) {
      if (kv.second && kv.second->data_channel()) {
        kv.second->data_channel()->UnregisterObserver();
      }
    }
  }

  // (2) Schliessen, solange Fabrik, Threads und SSL leben. Close() ist mit
  //     `observer_ == nullptr` sicher: die Schleife ueber die fremden Stroeme
  //     ruft nur `if (observer_) observer_->OnRemoveStream(...)`. Ein zweites
  //     Close() (aus ~RTCPeerConnectionImpl) ist ein No-Op, weil
  //     `rtc_peerconnection_` dann schon nullptr ist. Delete() nimmt die
  //     Referenz der Fabrik-Liste weg, damit der PC hier — und nicht erst
  //     nach Terminate() mit der Fabrik — verschwindet.
  for (auto& kv : peerconnections_) {
    if (!kv.second) continue;
    kv.second->Close();
    if (factory_) factory_->Delete(kv.second);
  }

  // (3) Erst jetzt die Container leeren. Ein Rueckruf, der vor (1) schon auf
  //     dem Signalisierungs-Thread lief, ist durch das synchrone Marshalling
  //     in Close() abgeschlossen, bevor die Beobachter hier fallen.
  const size_t pcs = peerconnections_.size();
  const size_t beobachter = peerconnection_observers_.size();
  peerconnections_.clear();
  {
    // Ausserhalb der Sperre zerstoeren: der Abbau eines RTCDataChannelImpl
    // marshalt auf den Signalisierungs-Thread, und der nimmt in
    // OnDataChannel dieselbe Sperre (dorthin kommt nach (1) zwar nichts mehr,
    // aber eine Sperre ueber einem blockierenden Aufruf ist unnoetig).
    std::map<std::string, std::shared_ptr<FlutterRTCDataChannelObserver>> weg;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      weg.swap(data_channel_observers_);
    }
  }
  peerconnection_observers_.clear();

  const long long ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();
  ExitLog("ende", pcs, beobachter, kanaele, ms);
}

EventChannelProxy* FlutterWebRTCBase::event_channel() {
  return event_channel_ ? event_channel_.get() : nullptr;
}

std::string FlutterWebRTCBase::GenerateUUID() {
  return libwebrtc::Helper::CreateRandomUuid().std_string();
}

RTCPeerConnection* FlutterWebRTCBase::PeerConnectionForId(
    const std::string& id) {
  auto it = peerconnections_.find(id);

  if (it != peerconnections_.end())
    return (*it).second.get();

  return nullptr;
}

void FlutterWebRTCBase::RemovePeerConnectionForId(const std::string& id) {
  auto it = peerconnections_.find(id);
  if (it != peerconnections_.end())
    peerconnections_.erase(it);
}

scoped_refptr<RTCMediaTrack> FlutterWebRTCBase ::MediaTrackForId(const std::string& id) {
  auto it = local_tracks_.find(id);

  if (it != local_tracks_.end())
    return (*it).second;

  for (auto kv : peerconnection_observers_) {
    auto pco = kv.second.get();
    auto track = pco->MediaTrackForId(id);
    if (track != nullptr)
      return track;
  }

  return nullptr;
}

void FlutterWebRTCBase::RemoveMediaTrackForId(const std::string& id) {
  auto it = local_tracks_.find(id);
  if (it != local_tracks_.end())
    local_tracks_.erase(it);
}

FlutterPeerConnectionObserver* FlutterWebRTCBase::PeerConnectionObserversForId(
    const std::string& id) {
  auto it = peerconnection_observers_.find(id);

  if (it != peerconnection_observers_.end())
    return (*it).second.get();

  return nullptr;
}

void FlutterWebRTCBase::RemovePeerConnectionObserversForId(
    const std::string& id) {
  auto it = peerconnection_observers_.find(id);
  if (it != peerconnection_observers_.end())
    peerconnection_observers_.erase(it);
}

scoped_refptr<RTCMediaStream> FlutterWebRTCBase::MediaStreamForId(
    const std::string& id, std::string ownerTag) {
  if (!ownerTag.empty()) {
    if (ownerTag == "local") {
      auto it = local_streams_.find(id);
      if (it != local_streams_.end()) {
        return (*it).second;
      }
    } else {
      auto pco = peerconnection_observers_.find(ownerTag);
      if (peerconnection_observers_.end() != pco) {
        auto stream = pco->second->MediaStreamForId(id);
        if (stream != nullptr) {
          return stream;
        }
      }
    }
  }

  auto it = local_streams_.find(id);
  if (it != local_streams_.end()) {
    return (*it).second;
  }

  return nullptr;
}

void FlutterWebRTCBase::RemoveStreamForId(const std::string& id) {
  auto it = local_streams_.find(id);
  if (it != local_streams_.end())
    local_streams_.erase(it);
}

bool FlutterWebRTCBase::ParseConstraints(const EncodableMap& constraints,
                                         RTCConfiguration* configuration) {
  memset(&configuration->ice_servers, 0, sizeof(configuration->ice_servers));
  return false;
}

void FlutterWebRTCBase::ParseConstraints(
    const EncodableMap& src,
    scoped_refptr<RTCMediaConstraints> mediaConstraints,
    ParseConstraintType type /*= kMandatory*/) {
  for (auto kv : src) {
    EncodableValue k = kv.first;
    EncodableValue v = kv.second;
    std::string key = GetValue<std::string>(k);
    std::string value;
    if (TypeIs<EncodableList>(v) || TypeIs<EncodableMap>(v)) {
    } else if (TypeIs<std::string>(v)) {
      value = GetValue<std::string>(v);
    } else if (TypeIs<double>(v)) {
      value = std::to_string(GetValue<double>(v));
    } else if (TypeIs<int>(v)) {
      value = std::to_string(GetValue<int>(v));
    } else if (TypeIs<bool>(v)) {
      value = GetValue<bool>(v) ? RTCMediaConstraints::kValueTrue
                                : RTCMediaConstraints::kValueFalse;
    } else {
      value = std::to_string(GetValue<int>(v));
    }
    if (type == kMandatory) {
      mediaConstraints->AddMandatoryConstraint(key.c_str(), value.c_str());
    } else {
      mediaConstraints->AddOptionalConstraint(key.c_str(), value.c_str());
      if (key == "DtlsSrtpKeyAgreement") {
        configuration_.srtp_type = GetValue<bool>(v)
                                       ? MediaSecurityType::kDTLS_SRTP
                                       : MediaSecurityType::kSDES_SRTP;
      }
    }
  }
}

scoped_refptr<RTCMediaConstraints> FlutterWebRTCBase::ParseMediaConstraints(
    const EncodableMap& constraints) {
  scoped_refptr<RTCMediaConstraints> media_constraints =
      RTCMediaConstraints::Create();

  if (constraints.find(EncodableValue("mandatory")) != constraints.end()) {
    auto it = constraints.find(EncodableValue("mandatory"));
    const EncodableMap mandatory = GetValue<EncodableMap>(it->second);
    ParseConstraints(mandatory, media_constraints, kMandatory);
  } else {
    // Log.d(TAG, "mandatory constraints are not a map");
  }

  auto it = constraints.find(EncodableValue("optional"));
  if (it != constraints.end()) {
    const EncodableValue optional = it->second;
    if (TypeIs<EncodableMap>(optional)) {
      ParseConstraints(GetValue<EncodableMap>(optional), media_constraints,
                       kOptional);
    } else if (TypeIs<EncodableList>(optional)) {
      const EncodableList list = GetValue<EncodableList>(optional);
      for (size_t i = 0; i < list.size(); i++) {
        ParseConstraints(GetValue<EncodableMap>(list[i]), media_constraints,
                         kOptional);
      }
    }
  } else {
    // Log.d(TAG, "optional constraints are not an array");
  }

  return media_constraints;
}

bool FlutterWebRTCBase::CreateIceServers(const EncodableList& iceServersArray,
                                         IceServer* ice_servers) {
  size_t size = iceServersArray.size();
  for (size_t i = 0; i < size; i++) {
    IceServer& ice_server = ice_servers[i];
    EncodableMap iceServerMap = GetValue<EncodableMap>(iceServersArray[i]);

    if (iceServerMap.find(EncodableValue("username")) != iceServerMap.end()) {
      ice_server.username = GetValue<std::string>(
          iceServerMap.find(EncodableValue("username"))->second);
    }
    if (iceServerMap.find(EncodableValue("credential")) != iceServerMap.end()) {
      ice_server.password = GetValue<std::string>(
          iceServerMap.find(EncodableValue("credential"))->second);
    }

    auto it = iceServerMap.find(EncodableValue("url"));
    if (it != iceServerMap.end() && TypeIs<std::string>(it->second)) {
      ice_server.uri = GetValue<std::string>(it->second);
    }
    it = iceServerMap.find(EncodableValue("urls"));
    if (it != iceServerMap.end()) {
      if (TypeIs<std::string>(it->second)) {
        ice_server.uri = GetValue<std::string>(it->second);
      }
      if (TypeIs<EncodableList>(it->second)) {
        const EncodableList urls = GetValue<EncodableList>(it->second);
        for (auto url : urls) {
          if (TypeIs<EncodableMap>(url)) {
            const EncodableMap map = GetValue<EncodableMap>(url);
            std::string value;
            auto it2 = map.find(EncodableValue("url"));
            if (it2 != map.end()) {
              ice_server.uri = GetValue<std::string>(it2->second);
            }
          } else if (TypeIs<std::string>(url)) {
            ice_server.uri = GetValue<std::string>(url);
          }
        }
      }
    }
  }
  return size > 0;
}

bool FlutterWebRTCBase::ParseRTCConfiguration(const EncodableMap& map,
                                              RTCConfiguration& conf) {
  auto it = map.find(EncodableValue("iceServers"));
  if (it != map.end()) {
    const EncodableList iceServersArray = GetValue<EncodableList>(it->second);
    CreateIceServers(iceServersArray, conf.ice_servers);
  }
  // iceTransportPolicy (public API)
  it = map.find(EncodableValue("iceTransportPolicy"));
  if (it != map.end() && TypeIs<std::string>(it->second)) {
    std::string v = GetValue<std::string>(it->second);
    if (v == "all")  // public
      conf.type = IceTransportsType::kAll;
    else if (v == "relay")
      conf.type = IceTransportsType::kRelay;
    else if (v == "nohost")
      conf.type = IceTransportsType::kNoHost;
    else if (v == "none")
      conf.type = IceTransportsType::kNone;
  }

  // bundlePolicy (public api)
  it = map.find(EncodableValue("bundlePolicy"));
  if (it != map.end() && TypeIs<std::string>(it->second)) {
    std::string v = GetValue<std::string>(it->second);
    if (v == "balanced")  // public
      conf.bundle_policy = kBundlePolicyBalanced;
    else if (v == "max-compat")  // public
      conf.bundle_policy = kBundlePolicyMaxCompat;
    else if (v == "max-bundle")  // public
      conf.bundle_policy = kBundlePolicyMaxBundle;
  }

  // rtcpMuxPolicy (public api)
  it = map.find(EncodableValue("rtcpMuxPolicy"));
  if (it != map.end() && TypeIs<std::string>(it->second)) {
    std::string v = GetValue<std::string>(it->second);
    if (v == "negotiate")  // public
      conf.rtcp_mux_policy = RtcpMuxPolicy::kRtcpMuxPolicyNegotiate;
    else if (v == "require")  // public
      conf.rtcp_mux_policy = RtcpMuxPolicy::kRtcpMuxPolicyRequire;
  }

  // FIXME: peerIdentity of type DOMString (public API)
  // FIXME: certificates of type sequence<RTCCertificate> (public API)
  // iceCandidatePoolSize of type unsigned short, defaulting to 0
  it = map.find(EncodableValue("iceCandidatePoolSize"));
  if (it != map.end()) {
    conf.ice_candidate_pool_size = GetValue<int>(it->second);
  }

  // sdpSemantics (public api)
  it = map.find(EncodableValue("sdpSemantics"));
  if (it != map.end() && TypeIs<std::string>(it->second)) {
    std::string v = GetValue<std::string>(it->second);
    if (v == "plan-b")  // public
      conf.sdp_semantics = SdpSemantics::kPlanB;
    else if (v == "unified-plan")  // public
      conf.sdp_semantics = SdpSemantics::kUnifiedPlan;
  } else {
    conf.sdp_semantics = SdpSemantics::kUnifiedPlan;
  }

  it = map.find(EncodableValue("enableDscp"));
  if (it != map.end() && TypeIs<bool>(it->second)) {
    conf.enable_dscp = GetValue<bool>(it->second);
  }

  // maxIPv6Networks
  it = map.find(EncodableValue("maxIPv6Networks"));
  if (it != map.end()) {
    conf.max_ipv6_networks = GetValue<int>(it->second);
  }
  return true;
}

scoped_refptr<RTCMediaTrack> FlutterWebRTCBase::MediaTracksForId(
    const std::string& id) {
  auto it = local_tracks_.find(id);
  if (it != local_tracks_.end()) {
    return (*it).second;
  }

  for (auto it2 : peerconnection_observers_) {
    auto pco = it2.second;
    auto t = pco->MediaTrackForId(id);
    if (t != nullptr) {
      return t;
    }
  }

  return nullptr;
}

void FlutterWebRTCBase::RemoveTracksForId(const std::string& id) {
  auto it = local_tracks_.find(id);
  if (it != local_tracks_.end())
    local_tracks_.erase(it);
}

libwebrtc::scoped_refptr<libwebrtc::RTCRtpSender>
FlutterWebRTCBase::GetRtpSenderById(RTCPeerConnection* pc, std::string id) {
  libwebrtc::scoped_refptr<libwebrtc::RTCRtpSender> result;
  auto senders = pc->senders();
  for (scoped_refptr<RTCRtpSender> item : senders.std_vector()) {
    std::string itemId = item->id().std_string();
    if (nullptr == result.get() && 0 == id.compare(itemId)) {
      result = item;
    }
  }
  return result;
}

libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver>
FlutterWebRTCBase::GetRtpReceiverById(RTCPeerConnection* pc,
                                          std::string id) {
  libwebrtc::scoped_refptr<libwebrtc::RTCRtpReceiver> result;
  auto receivers = pc->receivers();
  for (scoped_refptr<RTCRtpReceiver> item : receivers.std_vector()) {
    std::string itemId = item->id().std_string();
    if (nullptr == result.get() && 0 == id.compare(itemId)) {
      result = item;
    }
  }
  return result;
}

libwebrtc::scoped_refptr<libwebrtc::KeyProvider> FlutterWebRTCBase::GetKeyProviderForId(
      const std::string& keyProviderId) {
  auto it = key_providers_.find(keyProviderId);
  if (it != key_providers_.end()) {
    return it->second;
  }
  return nullptr;
}

}  // namespace flutter_webrtc_plugin
