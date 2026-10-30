import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'package:webrtc_interface/webrtc_interface.dart';

import '../helper.dart';
import '../video_renderer_extension.dart' show AudioControl;
import 'utils.dart';

class RTCVideoRenderer extends ValueNotifier<RTCVideoValue>
    implements VideoRenderer, AudioControl {
  RTCVideoRenderer() : super(RTCVideoValue.empty);
  Completer? _initializing;
  int? _textureId;
  bool _disposed = false;
  MediaStream? _srcObject;
  StreamSubscription<dynamic>? _eventSubscription;

  /// honeycord: Windows GPU-Zero-Copy-Vorschau. VOR initialize() auf true
  /// setzen -> der native Renderer registriert eine GpuSurfaceTexture (D3D11-
  /// Shared-Handle) statt eines CPU-PixelBuffers. Nur fuer Tracks sinnvoll, die
  /// native GPU-Frames liefern (lokaler Bildschirm-Share auf Windows/NVIDIA);
  /// fuer alles andere false lassen (Kamera/Remote bleiben PixelBuffer).
  bool gpuSurface = false;

  /// honeycord: nur fuer die EIGENEN Selbstansichten auf true setzen.
  /// Drosselt die Kachel nativ (spart teure Flutter-Composites fuer eine
  /// Vorschau, die man ohnehin sieht). Gilt fuer die GpuSurface-Kachel
  /// (Bildschirm-Selbstansicht, Windows) und seit 10.10.2026 auch fuer
  /// PixelBuffer (eigene Kamera, Windows + Linux). Nur die ANZEIGE verwirft
  /// Bilder — Spur und Encoder bleiben unberuehrt. Remote-Kacheln NICHT
  /// drosseln (false lassen) -> sie sollen mit voller Sender-FPS laufen.
  bool gpuSurfaceThrottle = false;

  /// honeycord: Drossel-Intervall in ms (nur mit gpuSurfaceThrottle).
  /// GpuSurface: User-Schalter „Vorschau-Fluessigkeit": 40 = Sparsam (~20 fps,
  /// Default), 25 = Normal (~30 fps), 0 = Fluessig (ungedrosselt). PixelBuffer:
  /// 67 = hoechstens ~15 Textur-Updates/s. VOR initialize() setzen.
  int gpuSurfaceThrottleMs = 40;

  /// honeycord (10.10.2026): PixelBuffer nur in Kachelgroesse umrechnen
  /// (Windows + Linux). Flutter nennt dem nativen Renderer die Groesse der
  /// Textur in LOGISCHEN Pixeln — hier das devicePixelRatio der Ansicht
  /// eintragen, dann rechnet der Renderer auf Geraete-Pixel der Kachel herunter
  /// (nie hoeher als das Bild). 0 = aus: volle Bildgroesse wie bisher. VOR
  /// initialize() setzen.
  double pixelBufferScale = 0;

  @override
  Future<void> initialize() async {
    if (_initializing != null) {
      await _initializing!.future;
      return;
    }
    _initializing = Completer();
    final response = await WebRTC.invokeMethod('createVideoRenderer', {
      'gpuSurface': gpuSurface,
      'gpuSurfaceThrottle': gpuSurfaceThrottle,
      'gpuSurfaceThrottleMs': gpuSurfaceThrottleMs,
      'pixelBufferScale': pixelBufferScale,
    });
    _textureId = response['textureId'];
    _eventSubscription = EventChannel('FlutterWebRTC/Texture$textureId')
        .receiveBroadcastStream()
        .listen(eventListener, onError: errorListener);
    _initializing!.complete(null);
  }

  @override
  int get videoWidth => value.width.toInt();

  @override
  int get videoHeight => value.height.toInt();

  @override
  int? get textureId => _textureId;

  @override
  MediaStream? get srcObject => _srcObject;

  @override
  Function? onResize;

  @override
  Function? onFirstFrameRendered;

  @override
  set srcObject(MediaStream? stream) {
    if (_disposed) {
      throw 'Can\'t set srcObject: The RTCVideoRenderer is disposed';
    }
    if (textureId == null) throw 'Call initialize before setting the stream';
    _srcObject = stream;
    WebRTC.invokeMethod('videoRendererSetSrcObject', <String, dynamic>{
      'textureId': textureId,
      'streamId': stream?.id ?? '',
      'ownerTag': stream?.ownerTag ?? ''
    }).then((_) {
      value = (stream == null)
          ? RTCVideoValue.empty
          : value.copyWith(renderVideo: renderVideo);
    }).catchError((e) {
      print('Got exception for RTCVideoRenderer::setSrcObject: ${e.message}');
    }, test: (e) => e is PlatformException);
  }

  Future<void> setSrcObject({MediaStream? stream, String? trackId}) async {
    if (_disposed) {
      throw 'Can\'t set srcObject: The RTCVideoRenderer is disposed';
    }
    if (_textureId == null) throw 'Call initialize before setting the stream';
    _srcObject = stream;
    var oldTextureId = _textureId;
    try {
      await WebRTC.invokeMethod('videoRendererSetSrcObject', <String, dynamic>{
        'textureId': _textureId,
        'streamId': stream?.id ?? '',
        'ownerTag': stream?.ownerTag ?? '',
        'trackId': trackId ?? '0'
      });
      value = (stream == null)
          ? RTCVideoValue.empty
          : value.copyWith(renderVideo: renderVideo);
    } on PlatformException catch (e) {
      throw 'Got exception for RTCVideoRenderer::setSrcObject: textureId $oldTextureId [disposed: $_disposed] with stream ${stream?.id}, error: ${e.message}';
    }
  }

  /// honeycord (30.10.2026): Empfangsdrossel zur Laufzeit — hoechstens [fps]
  /// Textur-Meldungen je Sekunde fuer DIESE Kachel, 0 = aus (Vorgabe). Unter
  /// Windows zeichnet jede Meldung das ganze Fenster neu; kleine fremde
  /// Kacheln brauchen keine 60 davon. Nur die Anzeige verwirft Bilder — Spur,
  /// Decoder und Statistik laufen unveraendert. Nach initialize() aufrufen;
  /// vorher oder nach dispose() tut es nichts. Plattformen ohne die Methode
  /// (macOS/iOS/Android) werden still uebergangen.
  Future<void> setMaxFps(int fps) async {
    if (_disposed || _textureId == null) return;
    try {
      await WebRTC.invokeMethod('videoRendererSetMaxFps', <String, dynamic>{
        'textureId': _textureId,
        'fps': fps < 0 ? 0 : fps,
      });
    } on MissingPluginException {
      // Plattform kennt die Drossel nicht — kein Fehler.
    } on PlatformException catch (e) {
      print('RTCVideoRenderer::setMaxFps: ${e.message}');
    }
  }

  @override
  Future<void> dispose() async {
    if (_disposed) return;
    await _eventSubscription?.cancel();
    _eventSubscription = null;
    if (_textureId != null) {
      try {
        await WebRTC.invokeMethod('videoRendererDispose', <String, dynamic>{
          'textureId': _textureId,
        });
        _textureId = null;
        _disposed = true;
      } on PlatformException catch (e) {
        throw 'Failed to RTCVideoRenderer::dispose: ${e.message}';
      }
    }

    return super.dispose();
  }

  void eventListener(dynamic event) {
    if (_disposed) return;
    final Map<dynamic, dynamic> map = event;
    switch (map['event']) {
      case 'didTextureChangeRotation':
        value =
            value.copyWith(rotation: map['rotation'], renderVideo: renderVideo);
        onResize?.call();
        break;
      case 'didTextureChangeVideoSize':
        value = value.copyWith(
            width: 0.0 + map['width'],
            height: 0.0 + map['height'],
            renderVideo: renderVideo);
        onResize?.call();
        break;
      case 'didFirstFrameRendered':
        value = value.copyWith(renderVideo: renderVideo);
        onFirstFrameRendered?.call();
        break;
    }
  }

  void errorListener(Object obj) {
    if (obj is Exception) {
      throw obj;
    }
  }

  @override
  bool get renderVideo => _textureId != null && _srcObject != null;

  @override
  bool get muted => _srcObject?.getAudioTracks()[0].muted ?? true;

  @override
  set muted(bool mute) {
    if (_disposed) {
      throw Exception('Can\'t be muted: The RTCVideoRenderer is disposed');
    }
    if (_srcObject == null) {
      throw Exception('Can\'t be muted: The MediaStream is null');
    }
    if (_srcObject!.ownerTag != 'local') {
      throw Exception(
          'You\'re trying to mute a remote track, this is not supported');
    }
    if (_srcObject!.getAudioTracks().isEmpty) {
      throw Exception('Can\'t be muted: The MediaStreamTrack(audio) is empty');
    }

    Helper.setMicrophoneMute(mute, _srcObject!.getAudioTracks()[0]);
  }

  @override
  Future<bool> audioOutput(String deviceId) async {
    try {
      await Helper.selectAudioOutput(deviceId);
    } catch (e) {
      print('Helper.selectAudioOutput ${e.toString()}');
      return false;
    }
    return true;
  }

  @override
  Future<void> setVolume(double value) async {
    try {
      if (_srcObject == null) {
        throw Exception('Can\'t set volume: The MediaStream is null');
      }
      for (MediaStreamTrack track in _srcObject!.getAudioTracks()) {
        await Helper.setVolume(value, track);
      }
    } catch (e) {
      print('Helper.setVolume ${e.toString()}');
    }
  }
}
