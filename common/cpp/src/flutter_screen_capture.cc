#include "flutter_screen_capture.h"

#if defined(_WIN32)
#include "../../../windows/honeycord_wasapi_loopback.h"
#endif

namespace flutter_webrtc_plugin {

FlutterScreenCapture::FlutterScreenCapture(FlutterWebRTCBase* base)
    : base_(base) {}

FlutterScreenCapture::~FlutterScreenCapture() = default;

bool FlutterScreenCapture::BuildDesktopSourcesList(const EncodableList& types,
                                                   bool force_reload) {
  size_t size = types.size();
  sources_.clear();
  for (size_t i = 0; i < size; i++) {
    std::string type_str = GetValue<std::string>(types[i]);
    DesktopType desktop_type = DesktopType::kScreen;
    if (type_str == "screen") {
      desktop_type = DesktopType::kScreen;
    } else if (type_str == "window") {
      desktop_type = DesktopType::kWindow;
    } else {
      // std::cout << "Unknown type " << type_str << std::endl;
      return false;
    }
    scoped_refptr<RTCDesktopMediaList> source_list;
    auto it = medialist_.find(desktop_type);
    if (it != medialist_.end()) {
      source_list = (*it).second;
    } else {
      source_list = base_->desktop_device_->GetDesktopMediaList(desktop_type);
      source_list->RegisterMediaListObserver(this);
      medialist_[desktop_type] = source_list;
    }
    source_list->UpdateSourceList(force_reload);
    int count = source_list->GetSourceCount();
    for (int j = 0; j < count; j++) {
      sources_.push_back(source_list->GetSource(j));
    }
  }
  return true;
}

void FlutterScreenCapture::GetDesktopSources(
    const EncodableList& types,
    std::unique_ptr<MethodResultProxy> result) {
  if (!BuildDesktopSourcesList(types, true)) {
    result->Error("Bad Arguments", "Failed to get desktop sources");
    return;
  }

  EncodableList sources;
  for (auto source : sources_) {
    EncodableMap info;
    info[EncodableValue("id")] = EncodableValue(source->id().std_string());
    info[EncodableValue("name")] = EncodableValue(source->name().std_string());
    info[EncodableValue("type")] =
        EncodableValue(source->type() == kWindow ? "window" : "screen");
    // TODO "thumbnailSize"
    info[EncodableValue("thumbnailSize")] = EncodableMap{
        {EncodableValue("width"), EncodableValue(0)},
        {EncodableValue("height"), EncodableValue(0)},
    };
    sources.push_back(EncodableValue(info));
  }

  std::cout << " sources: " << sources.size() << std::endl;
  auto map = EncodableMap();
  map[EncodableValue("sources")] = sources;
  result->Success(EncodableValue(map));
}

void FlutterScreenCapture::UpdateDesktopSources(
    const EncodableList& types,
    std::unique_ptr<MethodResultProxy> result) {
  if (!BuildDesktopSourcesList(types, false)) {
    result->Error("Bad Arguments", "Failed to update desktop sources");
    return;
  }
  auto map = EncodableMap();
  map[EncodableValue("result")] = true;
  result->Success(EncodableValue(map));
}

void FlutterScreenCapture::OnMediaSourceAdded(
    scoped_refptr<MediaSource> source) {
  std::cout << " OnMediaSourceAdded: " << source->id().std_string()
            << std::endl;

  EncodableMap info;
  info[EncodableValue("event")] = "desktopSourceAdded";
  info[EncodableValue("id")] = EncodableValue(source->id().std_string());
  info[EncodableValue("name")] = EncodableValue(source->name().std_string());
  info[EncodableValue("type")] =
      EncodableValue(source->type() == kWindow ? "window" : "screen");
  // TODO "thumbnailSize"
  info[EncodableValue("thumbnailSize")] = EncodableMap{
      {EncodableValue("width"), EncodableValue(0)},
      {EncodableValue("height"), EncodableValue(0)},
  };
  base_->event_channel()->Success(EncodableValue(info));
}

void FlutterScreenCapture::OnMediaSourceRemoved(
    scoped_refptr<MediaSource> source) {
  std::cout << " OnMediaSourceRemoved: " << source->id().std_string()
            << std::endl;

  EncodableMap info;
  info[EncodableValue("event")] = "desktopSourceRemoved";
  info[EncodableValue("id")] = EncodableValue(source->id().std_string());
  base_->event_channel()->Success(EncodableValue(info));
}

void FlutterScreenCapture::OnMediaSourceNameChanged(
    scoped_refptr<MediaSource> source) {
  std::cout << " OnMediaSourceNameChanged: " << source->id().std_string()
            << std::endl;

  EncodableMap info;
  info[EncodableValue("event")] = "desktopSourceNameChanged";
  info[EncodableValue("id")] = EncodableValue(source->id().std_string());
  info[EncodableValue("name")] = EncodableValue(source->name().std_string());
  base_->event_channel()->Success(EncodableValue(info));
}

void FlutterScreenCapture::OnMediaSourceThumbnailChanged(
    scoped_refptr<MediaSource> source) {
  std::cout << " OnMediaSourceThumbnailChanged: " << source->id().std_string()
            << std::endl;

  EncodableMap info;
  info[EncodableValue("event")] = "desktopSourceThumbnailChanged";
  info[EncodableValue("id")] = EncodableValue(source->id().std_string());
  info[EncodableValue("thumbnail")] =
      EncodableValue(source->thumbnail().std_vector());
  base_->event_channel()->Success(EncodableValue(info));
}

void FlutterScreenCapture::OnStart(scoped_refptr<RTCDesktopCapturer> capturer) {
  // std::cout << " OnStart: " << capturer->source()->id().std_string()
  //          << std::endl;
}

void FlutterScreenCapture::OnPaused(
    scoped_refptr<RTCDesktopCapturer> capturer) {
  // std::cout << " OnPaused: " << capturer->source()->id().std_string()
  //          << std::endl;
}

void FlutterScreenCapture::OnStop(scoped_refptr<RTCDesktopCapturer> capturer) {
  // std::cout << " OnStop: " << capturer->source()->id().std_string()
  //          << std::endl;
}

void FlutterScreenCapture::OnError(scoped_refptr<RTCDesktopCapturer> capturer) {
  // std::cout << " OnError: " << capturer->source()->id().std_string()
  //          << std::endl;
}

void FlutterScreenCapture::GetDesktopSourceThumbnail(
    std::string source_id,
    int width,
    int height,
    std::unique_ptr<MethodResultProxy> result) {
  scoped_refptr<MediaSource> source;
  for (auto src : sources_) {
    if (src->id().std_string() == source_id) {
      source = src;
    }
  }
  if (source.get() == nullptr) {
    result->Error("Bad Arguments", "Failed to get desktop source thumbnail");
    return;
  }
  std::cout << " GetDesktopSourceThumbnail: " << source->id().std_string()
            << std::endl;
  source->UpdateThumbnail();
  result->Success(EncodableValue(source->thumbnail().std_vector()));
}

void FlutterScreenCapture::GetDisplayMedia(
    const EncodableMap& constraints,
    std::unique_ptr<MethodResultProxy> result) {
  std::string source_id = "0";
  // DesktopType source_type = kScreen;
  double fps = 30.0;
  uint32_t max_width = 0;
  uint32_t max_height = 0;

  const EncodableMap video = findMap(constraints, "video");
  if (video != EncodableMap()) {
    const EncodableMap deviceId = findMap(video, "deviceId");
    if (deviceId != EncodableMap()) {
      source_id = findString(deviceId, "exact");
      if (source_id.empty()) {
        result->Error("Bad Arguments", "Incorrect video->deviceId->exact");
        return;
      }
      if (source_id != "0") {
        // source_type = DesktopType::kWindow;
      }
    }
    const EncodableMap mandatory = findMap(video, "mandatory");
    if (mandatory != EncodableMap()) {
      double frameRate = findDouble(mandatory, "frameRate");
      if (frameRate != 0.0) {
        fps = frameRate;
      }
      // honeycord-fork: max resolution clamp (clamped via libyuv I420Scale
      // in libwebrtc's RTCDesktopCapturerImpl before the encoder pipeline).
      EncodableValue mwv = findEncodableValue(mandatory, "maxWidth");
      if (!mwv.IsNull()) max_width = static_cast<uint32_t>(GetValue<int>(mwv));
      EncodableValue mhv = findEncodableValue(mandatory, "maxHeight");
      if (!mhv.IsNull()) max_height = static_cast<uint32_t>(GetValue<int>(mhv));
    }
  }

  std::string uuid = base_->GenerateUUID();

  scoped_refptr<RTCMediaStream> stream =
      base_->factory_->CreateStream(uuid.c_str());

  // System-Audio-Capture des freigegebenen Render-Devices (HoneyCord). Nur
  // dann aktiv, wenn die Dart-Seite getDisplayMedia mit `audio: true`
  // aufgerufen hat. WASAPI shared-mode Loopback → Int16-interleaved →
  // libwebrtc::RTCAudioSource(kCustom) → eigener RTCAudioTrack mit
  // TrackSource.screenShareAudio (LiveKit-side). Pendant zum Mac-SCK-Pfad
  // (FlutterRTCDesktopCapturer.m / HoneycordScreenAudioRelay).
  bool capture_audio = false;
  {
    auto it_audio = constraints.find(EncodableValue("audio"));
    if (it_audio != constraints.end()) {
      if (TypeIs<bool>(it_audio->second)) {
        capture_audio = GetValue<bool>(it_audio->second);
      } else if (TypeIs<EncodableMap>(it_audio->second)) {
        capture_audio = true;
      }
    }
  }

  EncodableMap params;
  params[EncodableValue("streamId")] = EncodableValue(uuid);

  // AUDIO

  EncodableList audio_tracks;
#if defined(_WIN32)
  if (capture_audio) {
    scoped_refptr<RTCAudioSource> audio_source =
        base_->factory_->CreateAudioSource(
            "screen-share-audio", RTCAudioSource::SourceType::kCustom);
    if (audio_source.get()) {
      auto audio_uuid = base_->GenerateUUID();
      scoped_refptr<RTCAudioTrack> audio_track =
          base_->factory_->CreateAudioTrack(audio_source, audio_uuid.c_str());
      if (audio_track.get()) {
        auto loopback = std::make_unique<honeycord::WasapiLoopback>(audio_source);
        if (loopback->Start()) {
          EncodableMap a_info;
          a_info[EncodableValue("id")] = EncodableValue(audio_track->id().std_string());
          a_info[EncodableValue("label")] = EncodableValue(audio_track->id().std_string());
          a_info[EncodableValue("kind")] = EncodableValue(audio_track->kind().std_string());
          a_info[EncodableValue("enabled")] = EncodableValue(audio_track->enabled());
          audio_tracks.push_back(EncodableValue(a_info));
          stream->AddTrack(audio_track);
          base_->local_tracks_[audio_track->id().std_string()] = audio_track;
          wasapi_loopbacks_[uuid] = std::move(loopback);
        }
      }
    }
  }
#else
  (void)capture_audio;
#endif
  params[EncodableValue("audioTracks")] = EncodableValue(audio_tracks);

  // VIDEO

  EncodableMap video_constraints;
  auto it = constraints.find(EncodableValue("video"));
  if (it != constraints.end() && TypeIs<EncodableMap>(it->second)) {
    video_constraints = GetValue<EncodableMap>(it->second);
  }

  scoped_refptr<MediaSource> source;
  for (auto src : sources_) {
    if (src->id().std_string() == source_id) {
      source = src;
    }
  }

  if (!source.get()) {
    result->Error("Bad Arguments", "source not found!");
    return;
  }

  scoped_refptr<RTCDesktopCapturer> desktop_capturer =
      base_->desktop_device_->CreateDesktopCapturer(source);

  if (!desktop_capturer.get()) {
    result->Error("Bad Arguments", "CreateDesktopCapturer failed!");
    return;
  }

  desktop_capturer->RegisterDesktopCapturerObserver(this);

  const char* video_source_label = "screen_capture_input";

  scoped_refptr<RTCVideoSource> video_source =
      base_->factory_->CreateDesktopSource(
          desktop_capturer, video_source_label,
          base_->ParseMediaConstraints(video_constraints));

  // TODO: RTCVideoSource -> RTCVideoTrack

  scoped_refptr<RTCVideoTrack> track =
      base_->factory_->CreateVideoTrack(video_source, uuid.c_str());

  EncodableList videoTracks;
  EncodableMap info;
  info[EncodableValue("id")] = EncodableValue(track->id().std_string());
  info[EncodableValue("label")] = EncodableValue(track->id().std_string());
  info[EncodableValue("kind")] = EncodableValue(track->kind().std_string());
  info[EncodableValue("enabled")] = EncodableValue(track->enabled());
  videoTracks.push_back(EncodableValue(info));
  params[EncodableValue("videoTracks")] = EncodableValue(videoTracks);

  stream->AddTrack(track);

  base_->local_tracks_[track->id().std_string()] = track;

  base_->local_streams_[uuid] = stream;

  if (max_width > 0 && max_height > 0) {
    desktop_capturer->SetMaxResolution(max_width, max_height);
  }
  desktop_capturer->Start(uint32_t(fps));

  result->Success(EncodableValue(params));
}

}  // namespace flutter_webrtc_plugin
