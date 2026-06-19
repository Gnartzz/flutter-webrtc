#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html
#
Pod::Spec.new do |s|
  s.name             = 'flutter_webrtc'
  s.version          = '1.4.0'
  s.summary          = 'Flutter WebRTC plugin for macOS.'
  s.description      = <<-DESC
A new flutter plugin project.
                       DESC
  s.homepage         = 'https://github.com/cloudwebrtc/flutter-webrtc'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'CloudWebRTC' => 'duanweiwei1982@gmail.com' }
  s.source           = { :path => '.' }
  s.source_files     = ['Classes/**/*']

  s.dependency 'FlutterMacOS'
  s.weak_frameworks = 'ScreenCaptureKit'
  # CoreImage/Metal fuer den GPU-Vorschau-Fast-Path (NV12-IOSurface -> BGRA ohne
  # CPU-Readback) in FlutterRTCVideoRenderer.
  s.frameworks = 'CoreImage', 'Metal'
  # WebRTC-SDK 144.7559.01 wird im App-Podfile per `:path =>` auf einen lokalen
  # HoneyCord-Build umgelenkt, der webrtc::HoneycordCustomAudioSource +
  # RTCCustomAudioSource für separates Screen-Share-Audio enthält
  # (Mac: SCK / Win: WASAPI-Loopback).
  s.dependency 'WebRTC-SDK', '144.7559.01'
  s.osx.deployment_target = '10.15'
end
