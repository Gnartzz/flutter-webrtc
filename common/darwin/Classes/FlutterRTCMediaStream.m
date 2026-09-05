#import <objc/runtime.h>
#import "AudioUtils.h"
#import "CameraUtils.h"
#import "FlutterRTCFrameCapturer.h"
#import "FlutterRTCMediaStream.h"
#import "FlutterRTCDesktopCapturer.h"
#import "FlutterRTCPeerConnection.h"
#import "VideoProcessingAdapter.h"
#import "LocalVideoTrack.h"
#import "LocalAudioTrack.h"

@implementation RTCMediaStreamTrack (Flutter)

- (id)settings {
  return objc_getAssociatedObject(self, _cmd);
}

- (void)setSettings:(id)settings {
  objc_setAssociatedObject(self, @selector(settings), settings, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
@end

@implementation AVCaptureDevice (Flutter)

- (NSString*)positionString {
  switch (self.position) {
    case AVCaptureDevicePositionUnspecified:
      return @"unspecified";
    case AVCaptureDevicePositionBack:
      return @"back";
    case AVCaptureDevicePositionFront:
      return @"front";
  }
  return nil;
}

@end


// ★ HoneyCord 05.09.2026 (Kartenton, GEMESSEN an Tims UGREEN 35871 + OBS-Vergleich):
// OBS startet den UVC-Strom GENAU EINMAL mit der Zielrate. Wir starteten ihn mit
// der Format-Vorgabe (1080p = 120 fps) und 50 ms spaeter fuer den Pin NEU. Ohne
// laufende Ton-Schnittstelle verkraftet die Karte den Neustart, mit laufendem
// UAC-Ton bleibt ihr Bild danach stehen (0 Pakete; 21:58 und 22:14). Deshalb
// wird die Bildrate jetzt VOR dem Start gesetzt und der Pin nach dem Start
// nur noch angefasst, wenn die Rate NICHT schon stimmt (kein Neustart).
static AVFrameRateRange* HoneycordBereichFuer(AVCaptureDeviceFormat* format, NSInteger pinFps) {
  if (format == nil || pinFps <= 0) return nil;
  NSArray<AVFrameRateRange*>* bereiche = format.videoSupportedFrameRateRanges;
  AVFrameRateRange* bereich = nil;
  for (AVFrameRateRange* r in bereiche) {
    if (r.minFrameRate - 0.75 <= (double)pinFps && (double)pinFps <= r.maxFrameRate + 0.75 &&
        (bereich == nil || r.maxFrameRate < bereich.maxFrameRate)) bereich = r;
  }
  if (bereich == nil) {
    AVFrameRateRange* unter = nil; AVFrameRateRange* kleinster = nil;
    for (AVFrameRateRange* r in bereiche) {
      if (kleinster == nil || r.maxFrameRate < kleinster.maxFrameRate) kleinster = r;
      if (r.maxFrameRate <= (double)pinFps + 0.75 && (unter == nil || r.maxFrameRate > unter.maxFrameRate)) unter = r;
    }
    bereich = unter ?: kleinster;
  }
  return bereich;
}

static double HoneycordAktiveFps(AVCaptureDevice* d) {
  CMTime mn = d.activeVideoMinFrameDuration;
  return mn.value > 0 ? ((double)mn.timescale / (double)mn.value) : 0.0;
}

/// Setzt Ober- UND Untergrenze der Bildrate am Geraet (Sperre haelt der Aufrufer).
/// Diskreter Bereich (UVC/Tundra): exakte Bereichsdauer; variabler Bereich: 1/fps.
/// Rueckgabe: die jetzt aktive Rate (0 = nichts gesetzt).
static double HoneycordPinnen(AVCaptureDevice* d, AVCaptureDeviceFormat* format, NSInteger pinFps, NSString* wo) {
  NSString* cls = NSStringFromClass([d class]);
  AVFrameRateRange* bereich = HoneycordBereichFuer(format, pinFps);
  BOOL gepinnt = NO;
  if (bereich != nil) {
    BOOL diskret  = fabs(bereich.maxFrameRate - bereich.minFrameRate) < 0.001;
    BOOL zielDrin = (double)pinFps >= bereich.minFrameRate - 1e-6 && (double)pinFps <= bereich.maxFrameRate + 1e-6;
    if (!diskret && zielDrin) {
      @try {
        d.activeVideoMinFrameDuration = CMTimeMake(1, (int32_t)pinFps);
        d.activeVideoMaxFrameDuration = CMTimeMake(1, (int32_t)pinFps);
        gepinnt = YES;
        NSLog(@"[hc-fps] %@: %ld fps fest im Bereich %.3f-%.3f (device %@)", wo, (long)pinFps, bereich.minFrameRate, bereich.maxFrameRate, cls);
      } @catch (NSException* eV) {
        NSLog(@"[hc-fps] %@: 1/%ld im variablen Bereich abgelehnt (device %@): %@", wo, (long)pinFps, cls, eV.reason);
      }
    }
    if (!gepinnt) {
      @try {
        d.activeVideoMinFrameDuration = bereich.minFrameDuration;
        d.activeVideoMaxFrameDuration = bereich.minFrameDuration;
        gepinnt = YES;
        NSLog(@"[hc-fps] %@: %.3f fps via range (Ziel %ld, device %@)", wo, bereich.maxFrameRate, (long)pinFps, cls);
      } @catch (NSException* e0) {
        NSLog(@"[hc-fps] %@: range-pin %.3f fps threw (device %@): %@", wo, bereich.maxFrameRate, cls, e0.reason);
      }
    }
  }
  if (!gepinnt) {
    @try {
      d.activeVideoMinFrameDuration = CMTimeMake(1, (int32_t)pinFps);
      d.activeVideoMaxFrameDuration = CMTimeMake(1, (int32_t)pinFps);
      gepinnt = YES;
      NSLog(@"[hc-fps] %@: %ld fps OK (device %@)", wo, (long)pinFps, cls);
    } @catch (NSException* e1) {
      @try {
        d.activeVideoMinFrameDuration = CMTimeMake(1, (int32_t)pinFps);
        gepinnt = YES;
        NSLog(@"[hc-fps] %@: %ld fps MIN-only, max threw (device %@)", wo, (long)pinFps, cls);
      } @catch (NSException* e2) {
        NSLog(@"[hc-fps] %@: FAILED to pin %ld fps (device %@): %@", wo, (long)pinFps, cls, e2.userInfo);
      }
    }
  }
  return gepinnt ? HoneycordAktiveFps(d) : 0.0;
}


// ★ GEMESSEN (Auric 22:26, Alpha 2.6.148): der Vorab-Pin (59,94 fps) ueberlebt den
// Capturer-Start NICHT — `updateDeviceCaptureFormat` setzt `activeFormat = format`,
// und macOS setzt dabei die Frame-Dauern auf die Format-Vorgabe zurueck (120 fps);
// erst unser Nach-Pin holte 59,94 zurueck = der zweite UVC-Start, der die Karte
// neben laufendem Ton anhaelt. Deshalb jetzt ein KVO-Beobachter auf `activeFormat`:
// er feuert SYNCHRON im Setter des Capturers (auf dessen Queue, Geraet bereits
// gesperrt) und setzt die exakte Bereichsdauer, BEVOR `startRunning` laeuft.
// Ergebnis: EIN UVC-Start mit der Zielrate, wie OBS.
@interface HoneycordFormatWaechter : NSObject
@property(nonatomic, strong) AVCaptureDevice* geraet;
@property(nonatomic, weak) AVCaptureSession* session;
@property(nonatomic) NSInteger zielFps;
@property(nonatomic) BOOL aktiv;
@property(nonatomic) NSUInteger treffer;
- (instancetype)initMitGeraet:(AVCaptureDevice*)geraet session:(AVCaptureSession*)session zielFps:(NSInteger)fps;
- (void)beenden;
@end

@implementation HoneycordFormatWaechter
- (instancetype)initMitGeraet:(AVCaptureDevice*)geraet session:(AVCaptureSession*)session zielFps:(NSInteger)fps {
  if ((self = [super init])) {
    _geraet = geraet;
    _session = session;
    _zielFps = fps;
    @try {
      [geraet addObserver:self forKeyPath:@"activeFormat" options:NSKeyValueObservingOptionNew context:NULL];
      _aktiv = YES;
    } @catch (NSException* e) {
      NSLog(@"[hc-fps] Waechter: addObserver warf: %@", e.reason);
    }
  }
  return self;
}
- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)object change:(NSDictionary*)change context:(void*)context {
  if (![keyPath isEqualToString:@"activeFormat"] || !self.aktiv) return;
  AVCaptureDevice* d = (AVCaptureDevice*)object;
  self.treffer++;
  // Das Geraet ist im Setter des Capturers gesperrt (lockForConfiguration im
  // startCaptureWithDevice-Block). Ohne Sperre wirft der Setter — @try in HoneycordPinnen.
  double jetzt = HoneycordPinnen(d, d.activeFormat, self.zielFps, @"waechter");
  NSLog(@"[hc-fps] waechter #%lu: activeFormat gesetzt -> %.3f fps", (unsigned long)self.treffer, jetzt);
  // ★ GEMESSEN (2.6.149): auch DAS ueberlebt den Start nicht — der CMIO-Stream-
  // Start einer DAL-Kamera nimmt die Format-Vorgabe (120 fps), die AVFoundation-
  // Dauer greift erst danach (= Neustart). OBS setzt die Rate an der VERBINDUNG
  // zum Ausgang (im OBS-Log: „ConfigureToBestMatch…MinimumFrameDuration" vor dem
  // Start). Also hier ebenso: videoMin/MaxFrameDuration an jeder Verbindung der
  // Session, die es unterstuetzt — die Verbindung besteht, weil der Capturer den
  // Input VOR dem Format setzt.
#if TARGET_OS_OSX
  AVFrameRateRange* bereich = HoneycordBereichFuer(d.activeFormat, self.zielFps);
  if (bereich != nil) {
    BOOL diskret = fabs(bereich.maxFrameRate - bereich.minFrameRate) < 0.001;
    CMTime dauer = diskret ? bereich.minFrameDuration : CMTimeMake(1, (int32_t)self.zielFps);
    for (AVCaptureConnection* c in self.session.connections) {
      if (![c.output isKindOfClass:[AVCaptureVideoDataOutput class]]) continue;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      @try {
        if (c.isVideoMinFrameDurationSupported) c.videoMinFrameDuration = dauer;
        if (c.isVideoMaxFrameDurationSupported) c.videoMaxFrameDuration = dauer;
        NSLog(@"[hc-fps] waechter: Verbindung min=%d max=%d -> %.3f fps",
              c.isVideoMinFrameDurationSupported ? 1 : 0, c.isVideoMaxFrameDurationSupported ? 1 : 0,
              dauer.value > 0 ? (double)dauer.timescale / (double)dauer.value : 0.0);
      } @catch (NSException* e) {
        NSLog(@"[hc-fps] waechter: Verbindung warf: %@", e.reason);
      }
#pragma clang diagnostic pop
    }
  }
#endif
}
- (void)beenden {
  if (!self.aktiv) return;
  self.aktiv = NO;
  @try { [self.geraet removeObserver:self forKeyPath:@"activeFormat"]; } @catch (NSException* e) {}
}
- (void)dealloc { [self beenden]; }
@end

@implementation FlutterWebRTCPlugin (RTCMediaStream)

/**
 * {@link https://www.w3.org/TR/mediacapture-streams/#navigatorusermediaerrorcallback}
 */
typedef void (^NavigatorUserMediaErrorCallback)(NSString* errorType, NSString* errorMessage);

/**
 * {@link https://www.w3.org/TR/mediacapture-streams/#navigatorusermediasuccesscallback}
 */
typedef void (^NavigatorUserMediaSuccessCallback)(RTCMediaStream* mediaStream);

- (NSDictionary*)defaultVideoConstraints {
    return @{@"minWidth" : @"1280", @"minHeight" : @"720", @"minFrameRate" : @"30"};
}

- (NSDictionary*)defaultAudioConstraints {
    return @{};
}


- (RTCMediaConstraints*)defaultMediaStreamConstraints {
  RTCMediaConstraints* constraints =
      [[RTCMediaConstraints alloc] initWithMandatoryConstraints:[self defaultVideoConstraints]
                                            optionalConstraints:nil];
  return constraints;
}


- (NSArray<AVCaptureDevice*> *) captureDevices {
    if (@available(iOS 13.0, macOS 10.15, macCatalyst 14.0, tvOS 17.0, *)) {
        NSArray<AVCaptureDeviceType> *deviceTypes = @[
#if TARGET_OS_IPHONE
            AVCaptureDeviceTypeBuiltInTripleCamera,
            AVCaptureDeviceTypeBuiltInDualCamera,
            AVCaptureDeviceTypeBuiltInDualWideCamera,
            AVCaptureDeviceTypeBuiltInWideAngleCamera,
            AVCaptureDeviceTypeBuiltInTelephotoCamera,
            AVCaptureDeviceTypeBuiltInUltraWideCamera,
#else
            AVCaptureDeviceTypeBuiltInWideAngleCamera,
#endif
        ];
        
#if !defined(TARGET_OS_IPHONE)
        if (@available(macOS 13.0, *)) {
            deviceTypes = [deviceTypes arrayByAddingObject:AVCaptureDeviceTypeDeskViewCamera];
        }
#endif

        if (@available(iOS 17.0, macOS 14.0, tvOS 17.0, *)) {
            deviceTypes = [deviceTypes arrayByAddingObjectsFromArray: @[
                AVCaptureDeviceTypeContinuityCamera,
                AVCaptureDeviceTypeExternal,
            ]];
        }

        return [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:deviceTypes
                                                                      mediaType:AVMediaTypeVideo
                                                                       position:AVCaptureDevicePositionUnspecified].devices;
    }
    return @[];
}

/**
 * Initializes a new {@link RTCAudioTrack} which satisfies specific constraints,
 * adds it to a specific {@link RTCMediaStream}, and reports success to a
 * specific callback. Implements the audio-specific counterpart of the
 * {@code getUserMedia()} algorithm.
 *
 * @param constraints The {@code MediaStreamConstraints} which the new
 * {@code RTCAudioTrack} instance is to satisfy.
 * @param successCallback The {@link NavigatorUserMediaSuccessCallback} to which
 * success is to be reported.
 * @param errorCallback The {@link NavigatorUserMediaErrorCallback} to which
 * failure is to be reported.
 * @param mediaStream The {@link RTCMediaStream} which is being initialized as
 * part of the execution of the {@code getUserMedia()} algorithm, to which a
 * new {@code RTCAudioTrack} is to be added, and which is to be reported to
 * {@code successCallback} upon success.
 */
- (void)getUserAudio:(NSDictionary*)constraints
     successCallback:(NavigatorUserMediaSuccessCallback)successCallback
       errorCallback:(NavigatorUserMediaErrorCallback)errorCallback
         mediaStream:(RTCMediaStream*)mediaStream {
  id audioConstraints = constraints[@"audio"];
  NSString* audioDeviceId = @"";
  RTCMediaConstraints *rtcConstraints;
  if ([audioConstraints isKindOfClass:[NSDictionary class]]) {
    // constraints.audio.deviceId
    NSString* deviceId = audioConstraints[@"deviceId"];

    if (deviceId) {
      audioDeviceId = deviceId;
    }

    rtcConstraints = [self parseMediaConstraints:audioConstraints];
    // constraints.audio.optional.sourceId
    id optionalConstraints = audioConstraints[@"optional"];
    if (optionalConstraints && [optionalConstraints isKindOfClass:[NSArray class]] &&
        !deviceId) {
      NSArray* options = optionalConstraints;
      for (id item in options) {
        if ([item isKindOfClass:[NSDictionary class]]) {
          NSString* sourceId = ((NSDictionary*)item)[@"sourceId"];
          if (sourceId) {
            audioDeviceId = sourceId;
          }
        }
      }
    }
  } else {
      rtcConstraints = [self parseMediaConstraints:[self defaultAudioConstraints]];
  }

// ★ GEMESSEN 2026-07-28 (HoneyCord #70): Hier stand `#if !defined(TARGET_OS_IPHONE)`
// — und damit wurde dieser Block auf macOS NIE uebersetzt.
//
// `TARGET_OS_IPHONE` ist auf jeder Apple-Plattform DEFINIERT; auf dem Mac
// schlicht mit dem Wert 0. `defined(...)` fragt aber nur nach der Existenz, nicht
// nach dem Wert — also war `!defined(...)` dort immer falsch. Nachgemessen mit
// einem eigenen clang-Lauf:
//     TARGET_OS_IPHONE definiert : JA (Wert 0)
//     #if !defined(...)          : WIRD WEGGELASSEN
//     #if TARGET_OS_OSX          : wird kompiliert
//
// Folge: Das in der App gewaehlte Mikrofon wurde nie angewandt. Die Geraete-ID
// reiste durch bis in `audioTrack.settings`, aber niemand rief
// `setInputDevice:` — das Audio-Geraetemodul oeffnete weiter den
// System-Standard-Eingang. Bei Bluetooth-Kopfhoerern hiess das obendrein
// Telefonqualitaet, weil der Standard-Eingang das Headset war.
//
// Der Rest dieser Datei prueft ueberall `#if TARGET_OS_IPHONE` (den WERT). Nur
// diese eine Stelle fragte nach der Existenz. `TARGET_OS_OSX` passt genau zu
// dem Guard, den `selectAudioInput:` in seinem eigenen Rumpf schon benutzt.
#if TARGET_OS_OSX
  if (audioDeviceId != nil) {
    [self selectAudioInput:audioDeviceId result:nil];
  }
#endif

  NSString* trackId = [[NSUUID UUID] UUIDString];
  RTCAudioSource *audioSource = [self.peerConnectionFactory audioSourceWithConstraints:rtcConstraints];
  RTCAudioTrack* audioTrack = [self.peerConnectionFactory audioTrackWithSource:audioSource trackId:trackId];
  LocalAudioTrack *localAudioTrack = [[LocalAudioTrack alloc] initWithTrack:audioTrack];

  audioTrack.settings = @{
    @"deviceId" : audioDeviceId,
    @"kind" : @"audioinput",
    @"autoGainControl" : @YES,
    @"echoCancellation" : @YES,
    @"noiseSuppression" : @YES,
    @"channelCount" : @1,
    @"latency" : @0,
  };

  [mediaStream addAudioTrack:audioTrack];

  [self.localTracks setObject:localAudioTrack forKey:trackId];

  [self ensureAudioSession];

  successCallback(mediaStream);
}

// TODO: Use RCTConvert for constraints ...
- (void)getUserMedia:(NSDictionary*)constraints result:(FlutterResult)result {
  // Initialize RTCMediaStream with a unique label in order to allow multiple
  // RTCMediaStream instances initialized by multiple getUserMedia calls to be
  // added to 1 RTCPeerConnection instance. As suggested by
  // https://www.w3.org/TR/mediacapture-streams/#mediastream to be a good
  // practice, use a UUID (conforming to RFC4122).
  NSString* mediaStreamId = [[NSUUID UUID] UUIDString];
  RTCMediaStream* mediaStream = [self.peerConnectionFactory mediaStreamWithStreamId:mediaStreamId];

  [self getUserMedia:constraints
      successCallback:^(RTCMediaStream* mediaStream) {
        NSString* mediaStreamId = mediaStream.streamId;

        NSMutableArray* audioTracks = [NSMutableArray array];
        NSMutableArray* videoTracks = [NSMutableArray array];

        for (RTCAudioTrack* track in mediaStream.audioTracks) {
          [audioTracks addObject:@{
            @"id" : track.trackId,
            @"kind" : track.kind,
            @"label" : track.trackId,
            @"enabled" : @(track.isEnabled),
            @"remote" : @(YES),
            @"readyState" : @"live",
            @"settings" : track.settings
          }];
        }

        for (RTCVideoTrack* track in mediaStream.videoTracks) {
          [videoTracks addObject:@{
            @"id" : track.trackId,
            @"kind" : track.kind,
            @"label" : track.trackId,
            @"enabled" : @(track.isEnabled),
            @"remote" : @(YES),
            @"readyState" : @"live",
            @"settings" : track.settings
          }];
        }

        self.localStreams[mediaStreamId] = mediaStream;
        result(@{
          @"streamId" : mediaStreamId,
          @"audioTracks" : audioTracks,
          @"videoTracks" : videoTracks
        });
      }
      errorCallback:^(NSString* errorType, NSString* errorMessage) {
        result([FlutterError errorWithCode:[NSString stringWithFormat:@"Error %@", errorType]
                                   message:errorMessage
                                   details:nil]);
      }
      mediaStream:mediaStream];
}

/**
 * Initializes a new {@link RTCAudioTrack} or a new {@link RTCVideoTrack} which
 * satisfies specific constraints and adds it to a specific
 * {@link RTCMediaStream} if the specified {@code mediaStream} contains no track
 * of the respective media type and the specified {@code constraints} specify
 * that a track of the respective media type is required; otherwise, reports
 * success for the specified {@code mediaStream} to a specific
 * {@link NavigatorUserMediaSuccessCallback}. In other words, implements a media
 * type-specific iteration of or successfully concludes the
 * {@code getUserMedia()} algorithm. The method will be recursively invoked to
 * conclude the whole {@code getUserMedia()} algorithm either with (successful)
 * satisfaction of the specified {@code constraints} or with failure.
 *
 * @param constraints The {@code MediaStreamConstraints} which specifies the
 * requested media types and which the new {@code RTCAudioTrack} or
 * {@code RTCVideoTrack} instance is to satisfy.
 * @param successCallback The {@link NavigatorUserMediaSuccessCallback} to which
 * success is to be reported.
 * @param errorCallback The {@link NavigatorUserMediaErrorCallback} to which
 * failure is to be reported.
 * @param mediaStream The {@link RTCMediaStream} which is being initialized as
 * part of the execution of the {@code getUserMedia()} algorithm.
 */
- (void)getUserMedia:(NSDictionary*)constraints
     successCallback:(NavigatorUserMediaSuccessCallback)successCallback
       errorCallback:(NavigatorUserMediaErrorCallback)errorCallback
         mediaStream:(RTCMediaStream*)mediaStream {
  // If mediaStream contains no audioTracks and the constraints request such a
  // track, then run an iteration of the getUserMedia() algorithm to obtain
  // local audio content.
  if (mediaStream.audioTracks.count == 0) {
    // constraints.audio
    id audioConstraints = constraints[@"audio"];
    BOOL constraintsIsDictionary = [audioConstraints isKindOfClass:[NSDictionary class]];
    if (audioConstraints && (constraintsIsDictionary || [audioConstraints boolValue])) {
      [self requestAccessForMediaType:AVMediaTypeAudio
                          constraints:constraints
                      successCallback:successCallback
                        errorCallback:errorCallback
                          mediaStream:mediaStream];
      return;
    }
  }

  // If mediaStream contains no videoTracks and the constraints request such a
  // track, then run an iteration of the getUserMedia() algorithm to obtain
  // local video content.
  if (mediaStream.videoTracks.count == 0) {
    // constraints.video
    id videoConstraints = constraints[@"video"];
    if (videoConstraints) {
      BOOL requestAccessForVideo = [videoConstraints isKindOfClass:[NSNumber class]]
                                       ? [videoConstraints boolValue]
                                       : [videoConstraints isKindOfClass:[NSDictionary class]];
#if !TARGET_IPHONE_SIMULATOR
      if (requestAccessForVideo) {
        [self requestAccessForMediaType:AVMediaTypeVideo
                            constraints:constraints
                        successCallback:successCallback
                          errorCallback:errorCallback
                            mediaStream:mediaStream];
        return;
      }
#endif
    }
  }

  // There are audioTracks and/or videoTracks in mediaStream as requested by
  // constraints so the getUserMedia() is to conclude with success.
  successCallback(mediaStream);
}

- (int)getConstrainInt:(NSDictionary*)constraints forKey:(NSString*)key {
  if (![constraints isKindOfClass:[NSDictionary class]]) {
    return 0;
  }

  id constraint = constraints[key];
  if ([constraint isKindOfClass:[NSNumber class]]) {
    return [constraint intValue];
  } else if ([constraint isKindOfClass:[NSString class]]) {
    int possibleValue = [constraint intValue];
    if (possibleValue != 0) {
      return possibleValue;
    }
  } else if ([constraint isKindOfClass:[NSDictionary class]]) {
    // Constraint-Objekt {ideal|exact|max|min: <Zahl|String>}. livekit_client
    // schickt die Kamera-fps als {'max': 60.0} (NSNumber-Double); frueher las
    // dieser Zweig NUR {'ideal': <String>} -> ein {max:...} fiel auf 0 durch ->
    // targetFps=0 -> die Kamera blieb beim Default ihres 720p-Formats (z.B. Brio
    // 720p30 statt 60; Home-Cams mit 720p60-Default liefen zufaellig richtig).
    // Jetzt Prioritaet ideal > exact > max > min, Zahl (int/double, gerundet) UND
    // String — analog zum Windows-C++-Parser (common/cpp/src/flutter_media_stream.cc).
    for (NSString* subKey in @[ @"ideal", @"exact", @"max", @"min" ]) {
      id sub = constraint[subKey];
      if ([sub isKindOfClass:[NSNumber class]]) {
        int possibleValue = (int) ([sub doubleValue] + 0.5);
        if (possibleValue != 0) {
          return possibleValue;
        }
      } else if ([sub isKindOfClass:[NSString class]]) {
        int possibleValue = [sub intValue];
        if (possibleValue != 0) {
          return possibleValue;
        }
      }
    }
  }

  return 0;
}

/**
 * Initializes a new {@link RTCVideoTrack} which satisfies specific constraints,
 * adds it to a specific {@link RTCMediaStream}, and reports success to a
 * specific callback. Implements the video-specific counterpart of the
 * {@code getUserMedia()} algorithm.
 *
 * @param constraints The {@code MediaStreamConstraints} which the new
 * {@code RTCVideoTrack} instance is to satisfy.
 * @param successCallback The {@link NavigatorUserMediaSuccessCallback} to which
 * success is to be reported.
 * @param errorCallback The {@link NavigatorUserMediaErrorCallback} to which
 * failure is to be reported.
 * @param mediaStream The {@link RTCMediaStream} which is being initialized as
 * part of the execution of the {@code getUserMedia()} algorithm, to which a
 * new {@code RTCVideoTrack} is to be added, and which is to be reported to
 * {@code successCallback} upon success.
 */
- (void)getUserVideo:(NSDictionary*)constraints
     successCallback:(NavigatorUserMediaSuccessCallback)successCallback
       errorCallback:(NavigatorUserMediaErrorCallback)errorCallback
         mediaStream:(RTCMediaStream*)mediaStream {
  id videoConstraints = constraints[@"video"];
  AVCaptureDevice* videoDevice;
  NSString* videoDeviceId = nil;
  NSString* facingMode = nil;
  NSArray<AVCaptureDevice*>* captureDevices = [self captureDevices];

  if ([videoConstraints isKindOfClass:[NSDictionary class]]) {
    // constraints.video.deviceId
    NSString* deviceId = videoConstraints[@"deviceId"];

    if (deviceId) {
        for (AVCaptureDevice *device in captureDevices) {
            if( [deviceId isEqualToString:device.uniqueID]) {
                videoDevice = device;
                videoDeviceId = deviceId;
            }
        }
    }

    // constraints.video.optional
    id optionalVideoConstraints = videoConstraints[@"optional"];
    if (optionalVideoConstraints && [optionalVideoConstraints isKindOfClass:[NSArray class]] &&
        !videoDevice) {
      NSArray* options = optionalVideoConstraints;
      for (id item in options) {
        if ([item isKindOfClass:[NSDictionary class]]) {
          NSString* sourceId = ((NSDictionary*)item)[@"sourceId"];
          if (sourceId) {
              for (AVCaptureDevice *device in captureDevices) {
                  if( [sourceId isEqualToString:device.uniqueID]) {
                      videoDevice = device;
                      videoDeviceId = sourceId;
                  }
              }
            if (videoDevice) {
              break;
            }
          }
        }
      }
    }

    if (!videoDevice) {
      // constraints.video.facingMode
      // https://www.w3.org/TR/mediacapture-streams/#def-constraint-facingMode
      facingMode = videoConstraints[@"facingMode"];
      if (facingMode && [facingMode isKindOfClass:[NSString class]]) {
        AVCaptureDevicePosition position;
        if ([facingMode isEqualToString:@"environment"]) {
          self._usingFrontCamera = NO;
          position = AVCaptureDevicePositionBack;
        } else if ([facingMode isEqualToString:@"user"]) {
          self._usingFrontCamera = YES;
          position = AVCaptureDevicePositionFront;
        } else {
          // If the specified facingMode value is not supported, fall back to
          // the default video device.
          self._usingFrontCamera = NO;
          position = AVCaptureDevicePositionUnspecified;
        }
        videoDevice = [self findDeviceForPosition:position];
      }
    }
  }

  if ([videoConstraints isKindOfClass:[NSNumber class]]) {
    videoConstraints = @{@"mandatory": [self defaultVideoConstraints]};
  }

  NSInteger targetWidth = 0;
  NSInteger targetHeight = 0;
  NSInteger targetFps = 0;

  if (!videoDevice) {
    videoDevice = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
  }

  int possibleWidth = [self getConstrainInt:videoConstraints forKey:@"width"];
  if (possibleWidth != 0) {
    targetWidth = possibleWidth;
  }

  int possibleHeight = [self getConstrainInt:videoConstraints forKey:@"height"];
  if (possibleHeight != 0) {
    targetHeight = possibleHeight;
  }

  int possibleFps = [self getConstrainInt:videoConstraints forKey:@"frameRate"];
  if (possibleFps != 0) {
    targetFps = possibleFps;
  }

  id mandatory =
      [videoConstraints isKindOfClass:[NSDictionary class]] ? videoConstraints[@"mandatory"] : nil;

  // constraints.video.mandatory
  if (mandatory && [mandatory isKindOfClass:[NSDictionary class]]) {
    id widthConstraint = mandatory[@"minWidth"];
    if ([widthConstraint isKindOfClass:[NSString class]] ||
        [widthConstraint isKindOfClass:[NSNumber class]]) {
      int possibleWidth = [widthConstraint intValue];
      if (possibleWidth != 0) {
        targetWidth = possibleWidth;
      }
    }
    id heightConstraint = mandatory[@"minHeight"];
    if ([heightConstraint isKindOfClass:[NSString class]] ||
        [heightConstraint isKindOfClass:[NSNumber class]]) {
      int possibleHeight = [heightConstraint intValue];
      if (possibleHeight != 0) {
        targetHeight = possibleHeight;
      }
    }
    id fpsConstraint = mandatory[@"minFrameRate"];
    if ([fpsConstraint isKindOfClass:[NSString class]] ||
        [fpsConstraint isKindOfClass:[NSNumber class]]) {
      int possibleFps = [fpsConstraint intValue];
      if (possibleFps != 0) {
        targetFps = possibleFps;
      }
    }
  }

  if (videoDevice) {
    RTCVideoSource* videoSource = [self.peerConnectionFactory videoSource];
    // ★ HoneyCord (Block 4): der alte Aufnehmer wird NICHT mehr gestoppt —
    // jede Spur behaelt ihren eigenen (siehe `videoCapturers`). Kamera und
    // Capture-Karte laufen so gleichzeitig, wie auf Windows und im nativen
    // Mac-Client. Gestoppt wird ausschliesslich ueber den Stop-Handler der
    // eigenen Spur.
      
    VideoProcessingAdapter *videoProcessingAdapter = [[VideoProcessingAdapter alloc] initWithRTCVideoSource:videoSource];
    self.videoCapturer = [[RTCCameraVideoCapturer alloc] initWithDelegate:videoProcessingAdapter];
      
    AVCaptureDeviceFormat* selectedFormat = [self selectFormatForDevice:videoDevice
                                                            targetWidth:targetWidth
                                                           targetHeight:targetHeight
                                                              targetFps:targetFps];

    CMVideoDimensions selectedDimension = CMVideoFormatDescriptionGetDimensions(selectedFormat.formatDescription);
    NSInteger selectedWidth = (NSInteger) selectedDimension.width;
    NSInteger selectedHeight = (NSInteger) selectedDimension.height;
    NSInteger selectedFps = [self selectFpsForFormat:selectedFormat targetFps:targetFps];

    self._lastTargetFps = selectedFps;
    self._lastTargetWidth = targetWidth;
    self._lastTargetHeight = targetHeight;
    
    NSLog(@"target format %ldx%ld, targetFps: %ld, selected format: %ldx%ld, selected fps %ld", targetWidth, targetHeight, targetFps, selectedWidth, selectedHeight, selectedFps);

    // HoneyCord: Kamera-fps NACH startCaptureWithDevice pinnen (im completionHandler).
    // Warum nicht davor (webrtc-sdk RTCCameraVideoCapturer::updateDeviceCaptureFormat):
    //  (a) startCapture setzt activeFormat NEU -> das RESETTET jede vorher gesetzte
    //      FrameDuration auf den Format-Default; ein Setzen DAVOR ist wirkungslos
    //      (darum brachte der activeFormat-vor-Duration-Versuch ea4f5dd nichts).
    //  (b) fuer AVCaptureDALDevice (viele USB/UVC-Cams, u.a. Logitech Brio)
    //      UEBERSPRINGT der Capturer das fps-Setzen KOMPLETT -> Cam bleibt auf 30,
    //      waehrend Nicht-DAL-Cams (Home-Cam) korrekt auf 60 gehen.
    // Hier ist activeFormat bereits == selectedFormat und wird nicht mehr ueberschrieben,
    // also 1/fps gueltig und dauerhaft. Geraeteklasse wird geloggt (Beweis DAL?).
    AVCaptureDevice* fpsDevice = videoDevice;
    NSInteger pinFps = selectedFps;
    // ★ VORAB: Format und Bildrate setzen, BEVOR der Capturer die Session startet.
    // Der Capturer setzt `activeFormat = format` (dasselbe Objekt) — ob das die
    // Dauer zuruecksetzt, zeigt die Kontrollzeile im completionHandler. Ziel:
    // EIN UVC-Start mit der Zielrate, wie OBS (s. HoneycordBereichFuer).
    if (pinFps > 0 && [videoDevice lockForConfiguration:NULL]) {
      @try {
        videoDevice.activeFormat = selectedFormat;
      } @catch (NSException* eF) {
        NSLog(@"[hc-fps] vorab: activeFormat abgelehnt: %@", eF.reason);
      }
      double vorab = HoneycordPinnen(videoDevice, selectedFormat, pinFps, @"vorab");
      NSLog(@"[hc-fps] vorab aktiv %.3f fps", vorab);
      [videoDevice unlockForConfiguration];
    }
    // ★ OBS-Weg (05.09. nachts): Ton der Capture-Karte in DIESELBE Session wie das
    // Bild, BEVOR der Capturer startet. Die Video-Track-Id entsteht deshalb schon
    // hier (sie war vorher erst nach dem Start vergeben).
    NSString* trackUUID = [[NSUUID UUID] UUIDString];
    NSString* tonGeraet = videoConstraints[@"honeycordTonGeraet"];
    BOOL tonInSession = NO;
#if TARGET_OS_OSX
    if ([tonGeraet isKindOfClass:[NSString class]] && tonGeraet.length > 0) {
      tonInSession = [self honeycordTonInSession:self.videoCapturer.captureSession geraetId:tonGeraet fuerVideoTrack:trackUUID];
      NSLog(@"[geraete-ton] OBS-Weg vor dem Start: %@", tonInSession ? @"eingehaengt" : @"NICHT eingehaengt");
    }
#endif
    HoneycordFormatWaechter* waechter = (pinFps > 0)
        ? [[HoneycordFormatWaechter alloc] initMitGeraet:videoDevice session:self.videoCapturer.captureSession zielFps:pinFps] : nil;
    [self.videoCapturer startCaptureWithDevice:videoDevice
                                        format:selectedFormat
                                           fps:selectedFps
                             completionHandler:^(NSError* error) {
                               // Der Start ist durch (Session laeuft) — Beobachter abmelden.
                               [waechter beenden];
                               if (error) {
                                 NSLog(@"Start capture error: %@", [error localizedDescription]);
                                 return;
                               }
                               // Ohne Ziel (keine frameRate in den Constraints liefert selectFpsForFormat 0)
                               // bleibt die Format-Vorgabe — sonst pinnte die Bereichssuche auf die
                               // LANGSAMSTE Stufe einer UVC-Karte (Pruefbefund 2).
                               if (pinFps <= 0) { NSLog(@"[hc-fps] kein Ziel (pinFps %ld), Format-Vorgabe bleibt", (long)pinFps); return; }
                               if (![fpsDevice lockForConfiguration:NULL]) return;
                               NSString* cls = NSStringFromClass([fpsDevice class]);
                               // Stimmt die Rate schon (Vorab-Pin hat den Start ueberlebt)?
                               // Dann NICHTS anfassen — jedes Setzen startet den UVC-Strom
                               // neu, und genau der Neustart hielt die Karte neben laufendem
                               // Ton an. Sonst wie bisher pinnen.
                               AVFrameRateRange* zielBereich = HoneycordBereichFuer(fpsDevice.activeFormat, pinFps);
                               const double zielFps = zielBereich ? zielBereich.maxFrameRate : (double)pinFps;
                               const double aktiv = HoneycordAktiveFps(fpsDevice);
                               if (aktiv > 0 && fabs(aktiv - zielFps) < 0.05) {
                                 NSLog(@"[hc-fps] nach Start: schon %.3f fps (Ziel %.3f) — kein Neustart (device %@)", aktiv, zielFps, cls);
                               } else if (self.honeycordTonAufnehmer.count > 0) {
                                 // ★ Laeuft der Ton der Capture-Karte, ist der Neustart des
                                 // Video-Stroms genau das, was die Karte anhaelt (gemessen
                                 // 21:58/22:14/22:26/22:37). Lieber die Vorgabe-Rate behalten
                                 // (LiveKit deckelt die Encoder-Rate ohnehin) als eine tote Karte.
                                 NSLog(@"[hc-fps] nach Start: %.3f statt %.3f fps — KEIN Neustart, weil Karten-Ton laeuft (device %@)", aktiv, zielFps, cls);
                               } else {
                                 const double jetzt = HoneycordPinnen(fpsDevice, fpsDevice.activeFormat, pinFps, @"nach Start");
                                 NSLog(@"[hc-fps] nach Start: war %.3f, jetzt %.3f fps (Ziel %.3f, device %@)", aktiv, jetzt, zielFps, cls);
                               }
                               {
                                 CMTime mn = fpsDevice.activeVideoMinFrameDuration, mx = fpsDevice.activeVideoMaxFrameDuration;
                                 NSLog(@"[hc-fps] aktiv jetzt %.3f..%.3f fps (device %@)",
                                       mx.value > 0 ? (double)mx.timescale / (double)mx.value : 0.0,
                                       mn.value > 0 ? (double)mn.timescale / (double)mn.value : 0.0, cls);
                               }
                               [fpsDevice unlockForConfiguration];
                             }];

    RTCVideoTrack* videoTrack = [self.peerConnectionFactory videoTrackWithSource:videoSource
                                                                        trackId:trackUUID];
    LocalVideoTrack *localVideoTrack = [[LocalVideoTrack alloc] initWithTrack:videoTrack videoProcessing:videoProcessingAdapter];
      
    __weak RTCCameraVideoCapturer* capturer = self.videoCapturer;
    __weak FlutterWebRTCPlugin* weakSelfStop = self;
    self.videoCapturerStopHandlers[videoTrack.trackId] = ^(CompletionHandler handler) {
      NSLog(@"Stop video capturer, trackID %@", videoTrack.trackId);
      // OBS-Weg: der Karten-Ton haengt in dieser Session — Quelle mit schliessen
      // (Stream-Id aus der Warteschlange oder ueber die Aufnehmer-Liste).
      FlutterWebRTCPlugin* me = weakSelfStop;
      NSDictionary* wartend = me ? me.honeycordTonWartend[videoTrack.trackId] : nil;
      if (wartend) { [me honeycordCaptureAudioStopFuer:wartend[@"streamId"]]; [me.honeycordTonWartend removeObjectForKey:videoTrack.trackId]; }
      [capturer stopCaptureWithCompletionHandler:handler];
    };
    // STARK halten, bis die Spur endet — die schwache Referenz im Handler
    // allein wuerde den Aufnehmer freigeben, sobald `videoCapturer` auf die
    // naechste Kamera zeigt.
    self.videoCapturers[videoTrack.trackId] = self.videoCapturer;

    if (!videoDeviceId) {
      videoDeviceId = videoDevice.uniqueID;
    }

    if (!facingMode) {
      facingMode = videoDevice.position == AVCaptureDevicePositionBack    ? @"environment"
                   : videoDevice.position == AVCaptureDevicePositionFront ? @"user"
                                                                          : @"unspecified";
    }

    videoTrack.settings = @{
      @"deviceId" : videoDeviceId,
      @"kind" : @"videoinput",
      @"width" : [NSNumber numberWithInteger:selectedWidth],
      @"height" : [NSNumber numberWithInteger:selectedHeight],
      @"frameRate" : [NSNumber numberWithInteger:selectedFps],
      @"facingMode" : facingMode,
    };

    [mediaStream addVideoTrack:videoTrack];

    [self.localTracks setObject:localVideoTrack forKey:trackUUID];

    successCallback(mediaStream);
  } else {
    // According to step 6.2.3 of the getUserMedia() algorithm, if there is no
    // source, fail with a new OverconstrainedError.
    errorCallback(@"OverconstrainedError", /* errorMessage */ nil);
  }
}

- (void)mediaStreamRelease:(RTCMediaStream*)stream {
  if (stream) {
    for (RTCVideoTrack* track in stream.videoTracks) {
      [self.localTracks removeObjectForKey:track.trackId];
    }
    for (RTCAudioTrack* track in stream.audioTracks) {
      [self.localTracks removeObjectForKey:track.trackId];
    }
    [self.localStreams removeObjectForKey:stream.streamId];
  }
}

/**
 * Obtains local media content of a specific type. Requests access for the
 * specified {@code mediaType} if necessary. In other words, implements a media
 * type-specific iteration of the {@code getUserMedia()} algorithm.
 *
 * @param mediaType Either {@link AVMediaTypAudio} or {@link AVMediaTypeVideo}
 * which specifies the type of the local media content to obtain.
 * @param constraints The {@code MediaStreamConstraints} which are to be
 * satisfied by the obtained local media content.
 * @param successCallback The {@link NavigatorUserMediaSuccessCallback} to which
 * success is to be reported.
 * @param errorCallback The {@link NavigatorUserMediaErrorCallback} to which
 * failure is to be reported.
 * @param mediaStream The {@link RTCMediaStream} which is to collect the
 * obtained local media content of the specified {@code mediaType}.
 */
- (void)requestAccessForMediaType:(NSString*)mediaType
                      constraints:(NSDictionary*)constraints
                  successCallback:(NavigatorUserMediaSuccessCallback)successCallback
                    errorCallback:(NavigatorUserMediaErrorCallback)errorCallback
                      mediaStream:(RTCMediaStream*)mediaStream {
  // According to step 6.2.1 of the getUserMedia() algorithm, if there is no
  // source, fail "with a new DOMException object whose name attribute has the
  // value NotFoundError."
  // XXX The following approach does not work for audio in Simulator. That is
  // because audio capture is done using AVAudioSession which does not use
  // AVCaptureDevice there. Anyway, Simulator will not (visually) request access
  // for audio.
  if (mediaType == AVMediaTypeVideo && [self captureDevices].count == 0) {
    // Since successCallback and errorCallback are asynchronously invoked
    // elsewhere, make sure that the invocation here is consistent.
    dispatch_async(dispatch_get_main_queue(), ^{
      errorCallback(@"DOMException", @"NotFoundError");
    });
    return;
  }

#if TARGET_OS_OSX
  if (@available(macOS 10.14, *)) {
#endif
    [AVCaptureDevice requestAccessForMediaType:mediaType
                             completionHandler:^(BOOL granted) {
                               dispatch_async(dispatch_get_main_queue(), ^{
                                 if (granted) {
                                   NavigatorUserMediaSuccessCallback scb =
                                       ^(RTCMediaStream* mediaStream) {
                                         [self getUserMedia:constraints
                                             successCallback:successCallback
                                               errorCallback:errorCallback
                                                 mediaStream:mediaStream];
                                       };

                                   if (mediaType == AVMediaTypeAudio) {
                                     [self getUserAudio:constraints
                                         successCallback:scb
                                           errorCallback:errorCallback
                                             mediaStream:mediaStream];
                                   } else if (mediaType == AVMediaTypeVideo) {
                                     [self getUserVideo:constraints
                                         successCallback:scb
                                           errorCallback:errorCallback
                                             mediaStream:mediaStream];
                                   }
                                 } else {
                                   // According to step 10 Permission Failure of the getUserMedia()
                                   // algorithm, if the user has denied permission, fail "with a new
                                   // DOMException object whose name attribute has the value
                                   // NotAllowedError."
                                   errorCallback(@"DOMException", @"NotAllowedError");
                                 }
                               });
                             }];
#if TARGET_OS_OSX
  } else {
    // Fallback on earlier versions
    NavigatorUserMediaSuccessCallback scb = ^(RTCMediaStream* mediaStream) {
      [self getUserMedia:constraints
          successCallback:successCallback
            errorCallback:errorCallback
              mediaStream:mediaStream];
    };
    if (mediaType == AVMediaTypeAudio) {
      [self getUserAudio:constraints
          successCallback:scb
            errorCallback:errorCallback
              mediaStream:mediaStream];
    } else if (mediaType == AVMediaTypeVideo) {
      [self getUserVideo:constraints
          successCallback:scb
            errorCallback:errorCallback
              mediaStream:mediaStream];
    }
  }
#endif
}

- (void)createLocalMediaStream:(FlutterResult)result {
  NSString* mediaStreamId = [[NSUUID UUID] UUIDString];
  RTCMediaStream* mediaStream = [self.peerConnectionFactory mediaStreamWithStreamId:mediaStreamId];

  self.localStreams[mediaStreamId] = mediaStream;
  result(@{@"streamId" : [mediaStream streamId]});
}

- (void)getSources:(FlutterResult)result {
  NSMutableArray* sources = [NSMutableArray array];
  NSArray* videoDevices =  [self captureDevices];
  for (AVCaptureDevice* device in videoDevices) {
    [sources addObject:@{
      @"facing" : device.positionString,
      @"deviceId" : device.uniqueID,
      @"label" : device.localizedName,
      @"kind" : @"videoinput",
    }];
  }
#if TARGET_OS_IPHONE

  RTCAudioSession* session = [RTCAudioSession sharedInstance];
  for (AVAudioSessionPortDescription* port in session.session.availableInputs) {
    // NSLog(@"input portName: %@, type %@", port.portName,port.portType);
    [sources addObject:@{
      @"deviceId" : port.UID,
      @"label" : port.portName,
      @"groupId" : port.portType,
      @"kind" : @"audioinput",
    }];
  }

  for (AVAudioSessionPortDescription* port in session.currentRoute.outputs) {
    // NSLog(@"output portName: %@, type %@", port.portName,port.portType);
    if (session.currentRoute.outputs.count == 1 && ![port.UID isEqualToString:@"Speaker"]) {
      [sources addObject:@{
        @"deviceId" : @"Speaker",
        @"label" : @"Speaker",
        @"groupId" : @"Speaker",
        @"kind" : @"audiooutput",
      }];
    }
    [sources addObject:@{
      @"deviceId" : port.UID,
      @"label" : port.portName,
      @"groupId" : port.portType,
      @"kind" : @"audiooutput",
    }];
  }
#endif
#if TARGET_OS_OSX
  RTCAudioDeviceModule* audioDeviceModule = [self.peerConnectionFactory audioDeviceModule];

  NSArray* inputDevices = [audioDeviceModule inputDevices];
  for (RTCIODevice* device in inputDevices) {
    [sources addObject:@{
      @"deviceId" : device.deviceId,
      @"label" : device.name,
      @"kind" : @"audioinput",
    }];
  }

  NSArray* outputDevices = [audioDeviceModule outputDevices];
  for (RTCIODevice* device in outputDevices) {
    [sources addObject:@{
      @"deviceId" : device.deviceId,
      @"label" : device.name,
      @"kind" : @"audiooutput",
    }];
  }
#endif
  result(@{@"sources" : sources});
}

- (void)selectAudioInput:(NSString*)deviceId result:(FlutterResult)result {
#if TARGET_OS_OSX
  RTCAudioDeviceModule* audioDeviceModule = [self.peerConnectionFactory audioDeviceModule];
  NSArray* inputDevices = [audioDeviceModule inputDevices];
  for (RTCIODevice* device in inputDevices) {
    if ([deviceId isEqualToString:device.deviceId]) {
      [audioDeviceModule setInputDevice:device];
      if (result)
        result(nil);
      return;
    }
  }
#endif
#if TARGET_OS_IPHONE
  RTCAudioSession* session = [RTCAudioSession sharedInstance];
  for (AVAudioSessionPortDescription* port in session.session.availableInputs) {
    if ([port.UID isEqualToString:deviceId]) {
      if (self.preferredInput != port.portType) {
        self.preferredInput = port.portType;
        [AudioUtils selectAudioInput:self.preferredInput];
      }
      break;
    }
  }
  if (result)
    result(nil);
#endif
  if (result)
    result([FlutterError errorWithCode:@"selectAudioInputFailed"
                               message:[NSString stringWithFormat:@"Error: deviceId not found!"]
                               details:nil]);
}

- (void)selectAudioOutput:(NSString*)deviceId result:(FlutterResult)result {
#if TARGET_OS_OSX
  RTCAudioDeviceModule* audioDeviceModule = [self.peerConnectionFactory audioDeviceModule];
  NSArray* outputDevices = [audioDeviceModule outputDevices];
  for (RTCIODevice* device in outputDevices) {
    if ([deviceId isEqualToString:device.deviceId]) {
      [audioDeviceModule setOutputDevice:device];
      result(nil);
      return;
    }
  }
#endif
#if TARGET_OS_IPHONE
  RTCAudioSession* session = [RTCAudioSession sharedInstance];
  NSError* setCategoryError = nil;

  if ([deviceId isEqualToString:@"Speaker"]) {
    [session.session overrideOutputAudioPort:kAudioSessionOverrideAudioRoute_Speaker
                                       error:&setCategoryError];
  } else {
    [session.session overrideOutputAudioPort:kAudioSessionOverrideAudioRoute_None
                                       error:&setCategoryError];
  }

  if (setCategoryError == nil) {
    result(nil);
    return;
  }

  result([FlutterError
      errorWithCode:@"selectAudioOutputFailed"
            message:[NSString
                        stringWithFormat:@"Error: %@", [setCategoryError localizedFailureReason]]
            details:nil]);

#endif
  result([FlutterError errorWithCode:@"selectAudioOutputFailed"
                             message:[NSString stringWithFormat:@"Error: deviceId not found!"]
                             details:nil]);
}

- (void)mediaStreamTrackRelease:(RTCMediaStream*)mediaStream track:(RTCMediaStreamTrack*)track {
  // what's different to mediaStreamTrackStop? only call mediaStream explicitly?
  if (mediaStream && track) {
    track.isEnabled = NO;
    // FIXME this is called when track is removed from the MediaStream,
    // but it doesn't mean it can not be added back using MediaStream.addTrack
    // TODO: [self.localTracks removeObjectForKey:trackID];
    if ([track.kind isEqualToString:@"audio"]) {
      [mediaStream removeAudioTrack:(RTCAudioTrack*)track];
    } else if ([track.kind isEqualToString:@"video"]) {
      [mediaStream removeVideoTrack:(RTCVideoTrack*)track];
    }
  }
}

- (void)mediaStreamTrackHasTorch:(RTCMediaStreamTrack*)track result:(FlutterResult)result {
  if (!self.videoCapturer) {
    result(@NO);
    return;
  }
  if (self.videoCapturer.captureSession.inputs.count == 0) {
    result(@NO);
    return;
  }

  AVCaptureDeviceInput* deviceInput = [self.videoCapturer.captureSession.inputs objectAtIndex:0];
  AVCaptureDevice* device = deviceInput.device;

  result(@([device isTorchModeSupported:AVCaptureTorchModeOn]));
}

- (void)mediaStreamTrackSetTorch:(RTCMediaStreamTrack*)track
                           torch:(BOOL)torch
                          result:(FlutterResult)result {
  if (!self.videoCapturer) {
    NSLog(@"Video capturer is null. Can't set torch");
    return;
  }
  if (self.videoCapturer.captureSession.inputs.count == 0) {
    NSLog(@"Video capturer is missing an input. Can't set torch");
    return;
  }

  AVCaptureDeviceInput* deviceInput = [self.videoCapturer.captureSession.inputs objectAtIndex:0];
  AVCaptureDevice* device = deviceInput.device;

  if (![device isTorchModeSupported:AVCaptureTorchModeOn]) {
    NSLog(@"Current capture device does not support torch. Can't set torch");
    return;
  }

  NSError* error;
  if ([device lockForConfiguration:&error] == NO) {
    NSLog(@"Failed to aquire configuration lock. %@", error.localizedDescription);
    return;
  }

  device.torchMode = torch ? AVCaptureTorchModeOn : AVCaptureTorchModeOff;
  [device unlockForConfiguration];

  result(nil);
}

- (void)mediaStreamTrackSetZoom:(RTCMediaStreamTrack*)track
                           zoomLevel:(double)zoomLevel
                          result:(FlutterResult)result {
#if TARGET_OS_OSX
  NSLog(@"Not supported on macOS. Can't set zoom");
  return;
#endif
#if TARGET_OS_IPHONE
  if (!self.videoCapturer) {
    NSLog(@"Video capturer is null. Can't set zoom");
    return;
  }
  if (self.videoCapturer.captureSession.inputs.count == 0) {
    NSLog(@"Video capturer is missing an input. Can't set zoom");
    return;
  }

  AVCaptureDeviceInput* deviceInput = [self.videoCapturer.captureSession.inputs objectAtIndex:0];
  AVCaptureDevice* device = deviceInput.device;

  NSError* error;
  if ([device lockForConfiguration:&error] == NO) {
    NSLog(@"Failed to acquire configuration lock. %@", error.localizedDescription);
    return;
  }
  
  CGFloat desiredZoomFactor = (CGFloat)zoomLevel;
  device.videoZoomFactor = MAX(1.0, MIN(desiredZoomFactor, device.activeFormat.videoMaxZoomFactor));
  [device unlockForConfiguration];

  result(nil);
#endif
}

- (void)mediaStreamTrackCaptureFrame:(RTCVideoTrack*)track
                              toPath:(NSString*)path
                              result:(FlutterResult)result {
  self.frameCapturer = [[FlutterRTCFrameCapturer alloc] initWithTrack:track
                                                               toPath:path
                                                               result:result];
}

- (void)mediaStreamTrackStop:(RTCMediaStreamTrack*)track {
  if (track) {
    track.isEnabled = NO;
    [self.localTracks removeObjectForKey:track.trackId];
  }
}

- (AVCaptureDevice*)findDeviceForPosition:(AVCaptureDevicePosition)position {
  if (position == AVCaptureDevicePositionUnspecified) {
    return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
  }
  NSArray<AVCaptureDevice*>* captureDevices = [RTCCameraVideoCapturer captureDevices];
  for (AVCaptureDevice* device in captureDevices) {
    if (device.position == position) {
      return device;
    }
  }
  if(captureDevices.count > 0) {
    return captureDevices[0];
  }
  return nil;
}

- (AVCaptureDeviceFormat*)selectFormatForDevice:(AVCaptureDevice*)device
                                    targetWidth:(NSInteger)targetWidth
                                   targetHeight:(NSInteger)targetHeight
                                      targetFps:(NSInteger)targetFps {
  NSArray<AVCaptureDeviceFormat*>* formats =
      [RTCCameraVideoCapturer supportedFormatsForDevice:device];
  AVCaptureDeviceFormat* selectedFormat = nil;
  long currentDiff = INT_MAX;
  BOOL selectedMeetsFps = NO;
  for (AVCaptureDeviceFormat* format in formats) {
    CMVideoDimensions dimension = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    FourCharCode pixelFormat = CMFormatDescriptionGetMediaSubType(format.formatDescription);
#if TARGET_OS_IPHONE
    if (@available(iOS 13.0, *)) {
      if(format.isMultiCamSupported != AVCaptureMultiCamSession.multiCamSupported) {
        continue;
      }
    }
#endif
    //NSLog(@"AVCaptureDeviceFormats,fps %d, dimension: %dx%d", format.videoSupportedFrameRateRanges, dimension.width, dimension.height);
    long diff = labs(targetWidth - dimension.width) + labs(targetHeight - dimension.height);
    // Erfuellt dieses Format die gewuenschte fps? (nur relevant, wenn targetFps>0)
    // Cameras wie die Brio bieten 720p in MEHREREN Formaten (z.B. 720p30 UND
    // 720p60) mit identischer Aufloesung an — die Auswahl nach reiner Aufloesung
    // (currentDiff) traf sonst zufaellig das 30er-Format und selectFpsForFormat
    // kappte via fmin(30, Ziel) auf 30. Bei gleicher Aufloesung deshalb fps-faehige
    // Formate bevorzugen, DANN das bevorzugte Pixelformat (altes Kriterium).
    BOOL meetsFps = NO;
    if (targetFps > 0) {
      for (AVFrameRateRange* fpsRange in format.videoSupportedFrameRateRanges) {
        if (fpsRange.maxFrameRate + 0.5 >= (Float64)targetFps) {
          meetsFps = YES;
          break;
        }
      }
    }
    if (diff < currentDiff) {
      selectedFormat = format;
      currentDiff = diff;
      selectedMeetsFps = meetsFps;
    } else if (diff == currentDiff) {
      if (targetFps > 0 && meetsFps && !selectedMeetsFps) {
        // fps-faehiges Format schlaegt ein bisher gewaehltes ohne die Ziel-fps.
        selectedFormat = format;
        selectedMeetsFps = meetsFps;
      } else if (meetsFps == selectedMeetsFps &&
                 pixelFormat == [self.videoCapturer preferredOutputPixelFormat]) {
        // gleiche fps-Klasse -> unveraendertes Pixelformat-Kriterium.
        selectedFormat = format;
      }
    }
  }
  return selectedFormat;
}

- (NSInteger)selectFpsForFormat:(AVCaptureDeviceFormat*)format targetFps:(NSInteger)targetFps {
  Float64 maxSupportedFramerate = 0;
  for (AVFrameRateRange* fpsRange in format.videoSupportedFrameRateRanges) {
    maxSupportedFramerate = fmax(maxSupportedFramerate, fpsRange.maxFrameRate);
  }
  return fmin(maxSupportedFramerate, targetFps);
}

@end
