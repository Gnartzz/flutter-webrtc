#import <objc/runtime.h>

#import "FlutterRTCDesktopCapturer.h"

#if TARGET_OS_IPHONE
#import <ReplayKit/ReplayKit.h>
#import "FlutterBroadcastScreenCapturer.h"
#import "FlutterRPScreenRecorder.h"
#endif

#import "VideoProcessingAdapter.h"
#import "LocalVideoTrack.h"
#import "LocalAudioTrack.h"
#if TARGET_OS_OSX
#import <AVFoundation/AVFoundation.h>
#import <WebRTC/RTCCustomAudioSource.h>
#import "FlutterScreenCaptureKitCapturer.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreGraphics/CoreGraphics.h>

// Pipe für System-Audio aus ScreenCaptureKit in eine RTCCustomAudioSource.
// SCK liefert Float32 PCM, in der Praxis INTERLEAVED (mBuffers[0] enthält alle
// Kanäle verflochten). Wir unterstützen beide Layouts (interleaved + non-
// interleaved), wandeln in Int16-interleaved und pushen in die CustomAudioSource.
// Hilfs-Ringbuffer für 10-ms-Chunking. libwebrtc' AudioSendStream::SendAudioData
// hat ein RTC_CHECK_EQ(samples_per_channel, sample_rate/100) — wir MÜSSEN in
// 10-ms-Schritten (480 Frames @ 48 kHz) pushen, sonst SIGABRT.
@interface HoneycordScreenAudioRelay : NSObject <FlutterScreenCaptureKitAudioDelegate> {
  BOOL _logged;
  uint64_t _bufferCount;
  int16_t *_chunkBuf;   // interleaved Int16, Größe = capacity * outChannels
  uint64_t _entries;    // Aufrufzaehler fuer die Log-Kadenz (je Instanz)
  size_t _chunkCap;     // capacity in Frames
  size_t _chunkLen;     // momentan belegte Frames
  size_t _chunkChannels;
}
@property(nonatomic, strong) RTCCustomAudioSource *source;
@end

@implementation HoneycordScreenAudioRelay

- (void)screenCapturerDidOutputAudioBuffer:(CMSampleBufferRef)sampleBuffer {
  // Ivar statt Funktions-Statik: seit Block 2 gibt es ZWEI Relais (SCK-Systemton
  // und Kartenton) auf zwei Queues — eine gemeinsame Statik waere ein Datenwettlauf.
  _entries++;
  const uint64_t entries = _entries;
  BOOL diag = (entries == 1 || (entries % 200) == 0);
  if (diag) {
    NSLog(@"[scr-audio relay] entry #%llu source=%@ sampleBuffer=%p",
          (unsigned long long)entries, self.source, sampleBuffer);
  }
  if (sampleBuffer == NULL || self.source == nil) {
    if (diag) NSLog(@"[scr-audio relay] EARLY: sb/source nil");
    return;
  }
  CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
  if (fmt == NULL) {
    if (diag) NSLog(@"[scr-audio relay] EARLY: no format description");
    return;
  }
  const AudioStreamBasicDescription *asbd =
      CMAudioFormatDescriptionGetStreamBasicDescription(fmt);
  if (asbd == NULL) {
    if (diag) NSLog(@"[scr-audio relay] EARLY: no asbd");
    return;
  }
  if (diag) {
    NSLog(@"[scr-audio relay] asbd: formatID=0x%x flags=0x%x sr=%.0f ch=%u "
          @"framesPerPacket=%u bytesPerPacket=%u bytesPerFrame=%u bitsPerChannel=%u",
          (unsigned)asbd->mFormatID, (unsigned)asbd->mFormatFlags,
          asbd->mSampleRate, (unsigned)asbd->mChannelsPerFrame,
          (unsigned)asbd->mFramesPerPacket, (unsigned)asbd->mBytesPerPacket,
          (unsigned)asbd->mBytesPerFrame, (unsigned)asbd->mBitsPerChannel);
  }
  if (asbd->mFormatID != kAudioFormatLinearPCM ||
      (asbd->mFormatFlags & kAudioFormatFlagIsFloat) == 0) {
    if (diag) NSLog(@"[scr-audio relay] EARLY: format not LinearPCM/Float "
                    @"id=0x%x flags=0x%x",
                    (unsigned)asbd->mFormatID, (unsigned)asbd->mFormatFlags);
    return;
  }
  const int sampleRate = (int)asbd->mSampleRate;
  const size_t channels = asbd->mChannelsPerFrame;
  if (sampleRate <= 0 || channels == 0 || channels > 8) {
    if (diag) NSLog(@"[scr-audio relay] EARLY: bad sr/ch sr=%d ch=%zu",
                    sampleRate, channels);
    return;
  }
  CMItemCount numFrames = CMSampleBufferGetNumSamples(sampleBuffer);
  if (numFrames <= 0) {
    if (diag) NSLog(@"[scr-audio relay] EARLY: numFrames=%lld", (long long)numFrames);
    return;
  }
  // Probe: NULL bufferList -> Größe in totalListSize. Apple gibt hier noErr
  // zurück, wenn nur die Größe abgefragt wird; das vorherige Pattern mit
  // einer Stack-AudioBufferList lieferte kCMSampleBufferError_ArrayTooSmall
  // und wurde fälschlich als Fehler behandelt.
  size_t totalListSize = 0;
  OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
      sampleBuffer, &totalListSize, NULL, 0, NULL, NULL,
      kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, NULL);
  if ((status != noErr && status != -12737 /* ArrayTooSmall */) || totalListSize == 0) {
    if (diag) NSLog(@"[scr-audio relay] EARLY: probe status=%d size=%zu",
                    (int)status, totalListSize);
    return;
  }
  AudioBufferList *list = (AudioBufferList *)malloc(totalListSize);
  CMBlockBufferRef block = NULL;
  status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
      sampleBuffer, NULL, list, totalListSize, NULL, NULL,
      kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
  if (status != noErr) {
    if (diag) NSLog(@"[scr-audio relay] EARLY: getABL status=%d", (int)status);
    free(list);
    if (block != NULL) CFRelease(block);
    return;
  }

  const BOOL isNonInterleaved =
      (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
  const size_t outChannels = MIN(channels, (size_t)2);

  // Einmalig Format-Diagnose loggen, damit wir sehen, was SCK wirklich liefert.
  if (!_logged) {
    _logged = YES;
    NSLog(@"[scr-audio relay] format: sr=%d ch=%zu interleaved=%@ "
          @"mBuffers=%u buf0.ch=%u buf0.bytes=%u flags=0x%x numFrames=%lld",
          sampleRate, channels, isNonInterleaved ? @"NO" : @"YES",
          list->mNumberBuffers,
          list->mBuffers[0].mNumberChannels,
          list->mBuffers[0].mDataByteSize,
          asbd->mFormatFlags,
          (long long)numFrames);
  }

  size_t frames = numFrames;
  int16_t *interleaved = NULL;

  if (isNonInterleaved) {
    // ein AudioBuffer pro Kanal
    for (size_t c = 0; c < outChannels && c < list->mNumberBuffers; c++) {
      size_t framesAvail = list->mBuffers[c].mDataByteSize / sizeof(float);
      frames = MIN(frames, framesAvail);
    }
    if (frames == 0) goto cleanup;
    interleaved = (int16_t *)malloc(frames * outChannels * sizeof(int16_t));
    float peak = 0.0f;
    for (size_t f = 0; f < frames; f++) {
      for (size_t c = 0; c < outChannels; c++) {
        const float *src = (const float *)list->mBuffers[c].mData;
        float sample = src[f];
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        float ab = sample < 0 ? -sample : sample;
        if (ab > peak) peak = ab;
        interleaved[f * outChannels + c] = (int16_t)(sample * 32767.0f);
      }
    }
    if ((_bufferCount++ % 200) == 0) {
      NSLog(@"[scr-audio relay] non-interleaved peak=%.4f frames=%zu", peak, frames);
    }
  } else {
    // mBuffers[0] enthält alle Kanäle interleaved
    const float *src = (const float *)list->mBuffers[0].mData;
    size_t framesAvail =
        list->mBuffers[0].mDataByteSize / sizeof(float) / channels;
    frames = MIN(frames, framesAvail);
    if (frames == 0) goto cleanup;
    interleaved = (int16_t *)malloc(frames * outChannels * sizeof(int16_t));
    float peak = 0.0f;
    for (size_t f = 0; f < frames; f++) {
      for (size_t c = 0; c < outChannels; c++) {
        float sample = src[f * channels + c];
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        float ab = sample < 0 ? -sample : sample;
        if (ab > peak) peak = ab;
        interleaved[f * outChannels + c] = (int16_t)(sample * 32767.0f);
      }
    }
    if ((_bufferCount++ % 200) == 0) {
      NSLog(@"[scr-audio relay] interleaved peak=%.4f frames=%zu", peak, frames);
    }
  }

  // 10-ms-Chunking. libwebrtc verlangt EXAKT 10-ms-AudioFrames im
  // Sender-Pfad (AudioSendStream::SendAudioData hat ein RTC_CHECK_EQ
  // samples_per_channel == sample_rate/100). SCK liefert aber 1024-/2048-
  // Frame-Buffer → wir puffern und drainen in chunks von 480 Frames @ 48 kHz.
  const size_t chunkFrames = (size_t)sampleRate / 100;  // 10 ms
  if (chunkFrames > 0) {
    // Buffer (neu) reservieren wenn Kanal-/Größenänderung.
    const size_t needCap = chunkFrames * 4;  // genug für Backlog
    if (_chunkBuf == NULL || _chunkCap < needCap || _chunkChannels != outChannels) {
      free(_chunkBuf);
      _chunkBuf = (int16_t *)malloc(needCap * outChannels * sizeof(int16_t));
      _chunkCap = needCap;
      _chunkLen = 0;
      _chunkChannels = outChannels;
    }
    size_t srcOffset = 0;
    while (srcOffset < frames) {
      size_t copyFrames = MIN(frames - srcOffset, _chunkCap - _chunkLen);
      memcpy(_chunkBuf + _chunkLen * outChannels,
             interleaved + srcOffset * outChannels,
             copyFrames * outChannels * sizeof(int16_t));
      _chunkLen += copyFrames;
      srcOffset += copyFrames;
      while (_chunkLen >= chunkFrames) {
        [self.source pushData:_chunkBuf
                bitsPerSample:16
                   sampleRate:sampleRate
                     channels:outChannels
                       frames:chunkFrames];
        const size_t remaining = _chunkLen - chunkFrames;
        if (remaining > 0) {
          memmove(_chunkBuf,
                  _chunkBuf + chunkFrames * outChannels,
                  remaining * outChannels * sizeof(int16_t));
        }
        _chunkLen = remaining;
      }
    }
  }

cleanup:
  if (interleaved != NULL) free(interleaved);
  free(list);
  if (block != NULL) CFRelease(block);
}

- (void)dealloc {
  free(_chunkBuf);
  _chunkBuf = NULL;
}

@end
#endif

#if TARGET_OS_OSX
RTCDesktopMediaList* _screen = nil;
RTCDesktopMediaList* _window = nil;
NSArray<RTCDesktopSource*>* _captureSources;
// Per SCK ergaenzte Fenster-IDs, die CGWindowList/RTCDesktopMediaList NICHT
// liefert (v.a. Fullscreen-Apps auf eigenem Space). Damit der Capture-Pfad
// (getDisplayMedia) so eine sourceId trotzdem als Fenster annimmt.
NSSet<NSString*>* _sckExtraWindowIds = nil;

// Ist die sourceId eine AKTIVE DisplayID? Screens kommen (>=12.3) nicht mehr aus
// der RTCDesktopMediaList (deren blockierendes GetSourceList stand hinter dem
// ~20s-Picker-Beachball auf macOS 26) — der Picker baut sie via
// CGGetActiveDisplayList, der Capture-Pfad erkennt sie hiermit.
static BOOL HCIsActiveDisplayId(NSString* sourceId) {
  if (sourceId == nil || sourceId.length == 0) return NO;
  CGDirectDisplayID want = (CGDirectDisplayID)[sourceId longLongValue];
  if (want == 0) return NO; // DisplayIDs sind nie 0; "abc" parst zu 0
  CGDirectDisplayID ids[16];
  uint32_t n = 0;
  if (CGGetActiveDisplayList(16, ids, &n) != kCGErrorSuccess) return NO;
  for (uint32_t i = 0; i < n; i++) {
    if (ids[i] == want) return YES;
  }
  return NO;
}
#else
static inline BOOL HCIsActiveDisplayId(NSString* sourceId) { return NO; }
#endif


#if TARGET_OS_OSX
// ★ HoneyCord Block 2 („Vier Stroeme", 05.09.2026): Ton eines AUFNAHMEGERAETS —
// die USB-Tonseite einer HDMI-Capture-Karte — als eigener Stream, den die App
// als `screenShareAudio` veroeffentlicht (Kartenton an der Karten-Kachel, mit
// eigenem Regler beim Zuhoerer). Windows macht dasselbe ueber WASAPI
// (`FlutterScreenCapture::CaptureAudioStart`), hier AVCaptureSession +
// AVCaptureAudioDataOutput. Der Weg in libwebrtc ist derselbe wie beim
// SCK-Systemton: `HoneycordScreenAudioRelay` wandelt Float32 in Int16 und
// pusht 10-ms-Bloecke in eine RTCCustomAudioSource. Deshalb wird die Ausgabe
// auf Float32/48 kHz/2 Kanaele festgelegt — GEMESSEN liefert die UGREEN 35871
// nativ 48 kHz, 2 Kanaele, 16-bit Int; der Wandler der AudioDataOutput macht
// daraus Float, und das Relais braucht nichts Neues.
//
// Kennungen: enumerateDevices liefert auf macOS `RTCIODevice.deviceId` = die
// CoreAudio-UID, und die ist GEMESSEN wortgleich mit `AVCaptureDevice.uniqueID`
// (z. B. „AppleUSBAudioEngine:UGREEN 35871:UGREEN 35871:PRODUCT:3").
@interface HoneycordGeraeteTonAufnehmer : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate>
@property(nonatomic, strong) AVCaptureSession *session;
@property(nonatomic, strong) AVCaptureAudioDataOutput *ausgang;
@property(nonatomic, strong) HoneycordScreenAudioRelay *relay;
@property(nonatomic, strong) RTCCustomAudioSource *source;
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, copy) NSString *streamId;
@property(nonatomic, copy) NSString *trackId;
/// Token der Block-Beobachter — `removeObserver:self` entfernt Block-Beobachter
/// NICHT (Apple: das zurueckgegebene Token muss entfernt werden; Pruefbefund 2. Runde).
@property(nonatomic, strong) id beobachterFehler;
@property(nonatomic, strong) id beobachterAbzug;
@property(atomic) BOOL gestoppt;
@property(nonatomic) uint64_t puffer;
@end

@implementation HoneycordGeraeteTonAufnehmer

- (BOOL)vorbereitenMitGeraet:(AVCaptureDevice *)geraet fehler:(NSError **)fehler {
  NSError *err = nil;
  AVCaptureDeviceInput *eingang = [AVCaptureDeviceInput deviceInputWithDevice:geraet error:&err];
  if (eingang == nil) {
    if (fehler) *fehler = err ?: [NSError errorWithDomain:@"honeycord" code:1
                                                userInfo:@{NSLocalizedDescriptionKey: @"Eingang liess sich nicht anlegen"}];
    return NO;
  }
  AVCaptureSession *session = [[AVCaptureSession alloc] init];
  [session beginConfiguration];
  if (![session canAddInput:eingang]) {
    [session commitConfiguration];
    if (fehler) *fehler = [NSError errorWithDomain:@"honeycord" code:2
                                          userInfo:@{NSLocalizedDescriptionKey: @"Session nimmt den Eingang nicht"}];
    return NO;
  }
  [session addInput:eingang];
  AVCaptureAudioDataOutput *ausgang = [[AVCaptureAudioDataOutput alloc] init];
  // Zielformat fuer das Relais: Float32, 48 kHz, interleaved. Die Kanalzahl
  // bleibt dem Geraet ueberlassen (Pruefbefund: Mono wuerde der Wandler nicht
  // sicher auf beide Kanaele legen; das Relais klemmt selbst auf <= 2, und
  // libwebrtc nimmt Mono). Ein 16-bit-Geraet (die UGREEN) wird nach Float gewandelt.
  ausgang.audioSettings = @{
    AVFormatIDKey : @(kAudioFormatLinearPCM),
    AVSampleRateKey : @48000.0,
    AVLinearPCMBitDepthKey : @32,
    AVLinearPCMIsFloatKey : @YES,
    AVLinearPCMIsBigEndianKey : @NO,
    AVLinearPCMIsNonInterleaved : @NO,
  };
  self.queue = dispatch_queue_create("honeycord.geraeteton", DISPATCH_QUEUE_SERIAL);
  [ausgang setSampleBufferDelegate:self queue:self.queue];
  if (![session canAddOutput:ausgang]) {
    [session commitConfiguration];
    if (fehler) *fehler = [NSError errorWithDomain:@"honeycord" code:3
                                          userInfo:@{NSLocalizedDescriptionKey: @"Session nimmt den Ausgang nicht"}];
    return NO;
  }
  [session addOutput:ausgang];
  [session commitConfiguration];
  self.session = session;
  self.ausgang = ausgang;
  // Asynchrone Fehler und Geraete-Abzug SEHEN (Pruefbefund 7): sonst liefert die
  // Session einfach nichts mehr, und die Spur bleibt stumm veroeffentlicht.
  __weak HoneycordGeraeteTonAufnehmer *weakSelf = self;
  NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
  // Zustellung auf Main — `stop` laeuft sonst parallel zu einem `stop` von Main.
  self.beobachterFehler = [nc addObserverForName:AVCaptureSessionRuntimeErrorNotification
                                          object:session queue:[NSOperationQueue mainQueue]
                                      usingBlock:^(NSNotification *n) {
    NSLog(@"[geraete-ton] Session-Fehler: %@", n.userInfo[AVCaptureSessionErrorKey]);
  }];
  self.beobachterAbzug = [nc addObserverForName:AVCaptureDeviceWasDisconnectedNotification
                                         object:geraet queue:[NSOperationQueue mainQueue]
                                     usingBlock:^(NSNotification *n) {
    NSLog(@"[geraete-ton] Geraet abgezogen: %@ — Aufnehmer wird gestoppt", geraet.localizedName);
    [weakSelf stop];
  }];
  return YES;
}

- (void)beobachterEntfernen {
  NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
  if (self.beobachterFehler) { [nc removeObserver:self.beobachterFehler]; self.beobachterFehler = nil; }
  if (self.beobachterAbzug)  { [nc removeObserver:self.beobachterAbzug];  self.beobachterAbzug  = nil; }
}

- (void)dealloc {
  [self beobachterEntfernen];
}

/// `startRunning` blockiert (50-300 ms Geraeteoeffnung) — deshalb NICHT auf dem
/// Platform-Thread (Pruefbefund 1), sondern im Hintergrund; `fertig` kommt auf Main.
- (void)startenMitGeraet:(AVCaptureDevice *)geraet fertig:(void (^)(BOOL laeuft))fertig {
  AVCaptureSession *session = self.session;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    [session startRunning];
    const BOOL laeuft = session.isRunning;
    NSLog(@"[geraete-ton] gestartet: %@ (%@) running=%d", geraet.localizedName, geraet.uniqueID, laeuft ? 1 : 0);
    dispatch_async(dispatch_get_main_queue(), ^{ fertig(laeuft); });
  });
}

- (void)captureOutput:(AVCaptureOutput *)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection *)connection {
  if (self.gestoppt) return;
  self.puffer++;
  if (self.puffer == 1 || (self.puffer % 500) == 0) {
    CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
    const AudioStreamBasicDescription *asbd = fmt ? CMAudioFormatDescriptionGetStreamBasicDescription(fmt) : NULL;
    NSLog(@"[geraete-ton] Puffer #%llu frames=%lld sr=%.0f ch=%u flags=0x%x",
          (unsigned long long)self.puffer, (long long)CMSampleBufferGetNumSamples(sampleBuffer),
          asbd ? asbd->mSampleRate : 0.0, asbd ? (unsigned)asbd->mChannelsPerFrame : 0u,
          asbd ? (unsigned)asbd->mFormatFlags : 0u);
  }
  // Dasselbe Relais wie der SCK-Systemton: Float -> Int16, 10-ms-Bloecke, push.
  [self.relay screenCapturerDidOutputAudioBuffer:sampleBuffer];
}

- (void)stop {
  if (self.gestoppt) return;
  self.gestoppt = YES;
  AVCaptureSession *session = self.session;
  AVCaptureAudioDataOutput *ausgang = self.ausgang;
  RTCCustomAudioSource *source = self.source;
  dispatch_queue_t q = self.queue;
  self.session = nil;
  self.ausgang = nil;
  [self beobachterEntfernen];
  // Apples dokumentierter Weg, Rueckrufe zu beenden — danach kommt kein Puffer mehr.
  [ausgang setSampleBufferDelegate:nil queue:nil];
  const uint64_t puffer = self.puffer;
  // `stopRunning` blockiert -> Hintergrund; `[source stop]` erst AUF der Delegate-
  // Queue, damit ein bereits laufender Rueckruf sein `pushData` beendet hat
  // (Pruefbefund 2: deterministisch statt „geht in der Praxis gut").
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    [session stopRunning];
    if (q) {
      dispatch_async(q, ^{ [source stop]; NSLog(@"[geraete-ton] gestoppt (%llu Puffer)", (unsigned long long)puffer); });
    } else {
      [source stop];
    }
  });
}

@end
#endif

@implementation FlutterWebRTCPlugin (DesktopCapturer)

- (void)getDisplayMedia:(NSDictionary*)constraints result:(FlutterResult)result {
  NSLog(@"[hc-cap] G0 getDisplayMedia ENTRY (User hat Quelle gewaehlt)");
  NSString* mediaStreamId = [[NSUUID UUID] UUIDString];
  RTCMediaStream* mediaStream = [self.peerConnectionFactory mediaStreamWithStreamId:mediaStreamId];
  RTCVideoSource* videoSource = [self.peerConnectionFactory videoSourceForScreenCast:YES];
  NSString* trackUUID = [[NSUUID UUID] UUIDString];
  VideoProcessingAdapter *videoProcessingAdapter = [[VideoProcessingAdapter alloc] initWithRTCVideoSource:videoSource];
  
#if TARGET_OS_IPHONE
  BOOL useBroadcastExtension = false;
  BOOL presentBroadcastPicker = false;

  id videoConstraints = constraints[@"video"];
  if ([videoConstraints isKindOfClass:[NSDictionary class]]) {
    // constraints.video.deviceId
    useBroadcastExtension =
        [((NSDictionary*)videoConstraints)[@"deviceId"] hasPrefix:@"broadcast"];
    presentBroadcastPicker =
        useBroadcastExtension &&
        ![((NSDictionary*)videoConstraints)[@"deviceId"] hasSuffix:@"-manual"];
  }

  id screenCapturer;

  if (useBroadcastExtension) {
    screenCapturer = [[FlutterBroadcastScreenCapturer alloc] initWithDelegate:videoProcessingAdapter];
  } else {
    screenCapturer = [[FlutterRPScreenRecorder alloc] initWithDelegate:[videoProcessingAdapter source]];
  }

  [screenCapturer startCapture];
  NSLog(@"start %@ capture", useBroadcastExtension ? @"broadcast" : @"replykit");

  self.videoCapturerStopHandlers[trackUUID] = ^(CompletionHandler handler) {
    NSLog(@"stop %@ capture, trackID %@", useBroadcastExtension ? @"broadcast" : @"replykit",
          trackUUID);
    [screenCapturer stopCaptureWithCompletionHandler:handler];
  };

  if (presentBroadcastPicker) {
    NSString* extension =
        [[[NSBundle mainBundle] infoDictionary] valueForKey:kRTCScreenSharingExtension];

    RPSystemBroadcastPickerView* picker = [[RPSystemBroadcastPickerView alloc] init];
    picker.showsMicrophoneButton = false;
    if (extension) {
      picker.preferredExtension = extension;
    } else {
      NSLog(@"Not able to find the %@ key", kRTCScreenSharingExtension);
    }
    SEL selector = NSSelectorFromString(@"buttonPressed:");
    if ([picker respondsToSelector:selector]) {
      [picker performSelector:selector withObject:nil];
    }
  }
#endif

#if TARGET_OS_OSX
  /* example for constraints:
      {
          'audio': false,
          'video": {
              'deviceId':  {'exact': sourceId},
              'mandatory': {
                  'frameRate': 30.0
              },
          }
      }
  */
  NSString* sourceId = nil;
  BOOL useDefaultScreen = NO;
  NSInteger fps = 30;
  // Deckel-Box fuer SCK-GPU-Downscale (0 = volle Display-Aufloesung).
  NSInteger maxWidth = 0;
  NSInteger maxHeight = 0;
  id videoConstraints = constraints[@"video"];
  if ([videoConstraints isKindOfClass:[NSNumber class]] && [videoConstraints boolValue] == YES) {
    useDefaultScreen = YES;
  } else if ([videoConstraints isKindOfClass:[NSDictionary class]]) {
    NSDictionary* deviceId = videoConstraints[@"deviceId"];
    if (deviceId != nil && [deviceId isKindOfClass:[NSDictionary class]]) {
      if (deviceId[@"exact"] != nil) {
        sourceId = deviceId[@"exact"];
        if (sourceId == nil) {
          result(@{@"error" : @"No deviceId.exact found"});
          return;
        }
      }
    } else {
      // fall back to default screen if no deviceId is specified
      useDefaultScreen = YES;
    }
    id mandatory = videoConstraints[@"mandatory"];
    if (mandatory != nil && [mandatory isKindOfClass:[NSDictionary class]]) {
      id frameRate = mandatory[@"frameRate"];
      if (frameRate != nil && [frameRate isKindOfClass:[NSNumber class]]) {
        fps = [frameRate integerValue];
      }
      id mw = mandatory[@"maxWidth"];
      if (mw != nil && [mw isKindOfClass:[NSNumber class]]) {
        maxWidth = [mw integerValue];
      }
      id mh = mandatory[@"maxHeight"];
      if (mh != nil && [mh isKindOfClass:[NSNumber class]]) {
        maxHeight = [mh integerValue];
      }
    }
  }
  // System-Audio des freigegebenen Bildschirms mit übertragen, wenn die
  // Constraint `audio: true` gesetzt wurde. Erfordert SCK (macOS 13+) — beim
  // legacy RTCDesktopCapturer-Pfad still ignoriert.
  BOOL captureAudio = NO;
  id audioConstraint = constraints[@"audio"];
  if ([audioConstraint isKindOfClass:[NSNumber class]]) {
    captureAudio = [audioConstraint boolValue];
  } else if ([audioConstraint isKindOfClass:[NSDictionary class]]) {
    captureAudio = YES;
  }

  RTCDesktopCapturer* desktopCapturer;
  FlutterScreenCaptureKitCapturer* screenCaptureKitCapturer = nil;
  RTCDesktopSource* source = nil;
  BOOL useScreenCaptureKit = NO;
  BOOL isWindow = NO;

  if (useDefaultScreen) {
    useScreenCaptureKit = YES;
  } else if (HCIsActiveDisplayId(sourceId)) {
    // Screens kommen (>=12.3) NICHT mehr aus der RTCDesktopMediaList — der Picker
    // baut sie via CGGetActiveDisplayList. sourceId = DisplayID -> direkt SCK.
    useScreenCaptureKit = YES;
  } else {
    source = [self getSourceById:sourceId];
    // sourceId nicht in der RTCDesktopMediaList? Dann evtl. ein per SCK
    // nachgereichtes (Fullscreen-)Fenster -> direkt als Fenster via SCK aufnehmen.
    BOOL isSckExtra = NO;
    if (@available(macOS 12.3, *)) {
      isSckExtra = (source == nil) && [_sckExtraWindowIds containsObject:sourceId];
    }
    if (source == nil && !isSckExtra) {
      result(@{@"error" : [NSString stringWithFormat:@"No source found for id: %@", sourceId]});
      return;
    }
    if (isSckExtra) {
      useScreenCaptureKit = YES;
      isWindow = YES;
    } else if (source.sourceType == RTCDesktopSourceTypeScreen) {
      useScreenCaptureKit = YES;
    } else if (@available(macOS 12.3, *)) {
      // FENSTER zero-copy via SCK (Mac-Gegenstueck zur Windows-WGC-Fenster-Capture);
      // sourceId = CGWindowID. Fallback (kein SCK <12.3): legacy CPU-Capturer unten.
      useScreenCaptureKit = YES;
      isWindow = YES;
    } else {
      desktopCapturer = [[RTCDesktopCapturer alloc] initWithSource:source
                                                          delegate:self
                                                   captureDelegate:videoProcessingAdapter];
    }
  }
  // Pro Capture-Session ein RTCCustomAudioSource + RTCAudioTrack. Wird nur
  // angelegt, wenn captureAudio=YES UND wir SCK nutzen (legacy DesktopCapturer
  // hat keinen Audio-Pfad). Strong Ref via Closure, damit der Relay den
  // Capture-Lifetime überlebt.
  RTCCustomAudioSource *screenAudioSource = nil;
  HoneycordScreenAudioRelay *audioRelay = nil;
  NSString *audioTrackUUID = nil;
  if (captureAudio && useScreenCaptureKit && @available(macOS 13.0, *)) {
    screenAudioSource =
        [[RTCCustomAudioSource alloc] initWithFactory:self.peerConnectionFactory];
    audioRelay = [[HoneycordScreenAudioRelay alloc] init];
    audioRelay.source = screenAudioSource;
    audioTrackUUID = [[NSUUID UUID] UUIDString];
  }
  if (useScreenCaptureKit) {
    if (@available(macOS 12.3, *)) {
      screenCaptureKitCapturer =
          [[FlutterScreenCaptureKitCapturer alloc] initWithDelegate:videoProcessingAdapter];
      if (audioRelay != nil) {
        screenCaptureKitCapturer.audioDelegate = audioRelay;
      }
      [screenCaptureKitCapturer startCaptureWithFPS:fps
                                           sourceId:sourceId
                                       captureAudio:(audioRelay != nil)
                                           isWindow:isWindow
                                           maxWidth:maxWidth
                                          maxHeight:maxHeight
                                          onStarted:^(NSError * _Nullable error) {
                                            if (error != nil) {
                                              NSLog(@"ScreenCaptureKit start failed: %@", error);
                                            } else {
                                              NSLog(@"start screencapturekit capture: for  sourceId: %@, fps: %lu, audio=%@",
                                                    sourceId, fps, (audioRelay != nil) ? @"YES" : @"NO");
                                            }
                                          }];
    } else {
      NSLog(@"ScreenCaptureKit not available, falling back to RTCDesktopCapturer");
      desktopCapturer = [[RTCDesktopCapturer alloc] initWithDefaultScreen:self
                                                          captureDelegate:videoProcessingAdapter];
    }
  }

  if (screenCaptureKitCapturer == nil) {
    [desktopCapturer startCaptureWithFPS:fps];
    NSLog(@"start desktop capture: sourceId: %@, type: %@, fps: %lu", sourceId,
          source.sourceType == RTCDesktopSourceTypeScreen ? @"screen" : @"window", fps);

    self.videoCapturerStopHandlers[trackUUID] = ^(CompletionHandler handler) {
      NSLog(@"stop desktop capture: sourceId: %@, type: %@, trackID %@", sourceId,
            source.sourceType == RTCDesktopSourceTypeScreen ? @"screen" : @"window", trackUUID);
      [desktopCapturer stopCapture];
      handler();
    };
  } else {
    self.videoCapturerStopHandlers[trackUUID] = ^(CompletionHandler handler) {
      NSLog(@"stop screencapturekit capture: trackID %@", trackUUID);
      // Audio-Source mit beenden — Relay-Strong-Ref durch das Closure freigegeben.
      [screenAudioSource stop];
      [screenCaptureKitCapturer stopCaptureWithCompletion:handler];
    };
  }
#endif

  RTCVideoTrack* videoTrack = [self.peerConnectionFactory videoTrackWithSource:videoSource
                                                                       trackId:trackUUID];
  [mediaStream addVideoTrack:videoTrack];

  LocalVideoTrack *localVideoTrack = [[LocalVideoTrack alloc] initWithTrack:videoTrack videoProcessing:videoProcessingAdapter];

  [self.localTracks setObject:localVideoTrack forKey:trackUUID];

  NSMutableArray* audioTracks = [NSMutableArray array];
  NSMutableArray* videoTracks = [NSMutableArray array];

#if TARGET_OS_OSX
  if (screenAudioSource != nil && audioTrackUUID != nil) {
    RTCAudioTrack *screenAudioTrack =
        [self.peerConnectionFactory audioTrackWithSource:screenAudioSource
                                                 trackId:audioTrackUUID];
    [mediaStream addAudioTrack:screenAudioTrack];
    LocalAudioTrack *localAudioTrack =
        [[LocalAudioTrack alloc] initWithTrack:screenAudioTrack];
    [self.localTracks setObject:localAudioTrack forKey:audioTrackUUID];
  }
#endif

  for (RTCAudioTrack* track in mediaStream.audioTracks) {
    [audioTracks addObject:@{
      @"id" : track.trackId,
      @"kind" : track.kind,
      @"label" : track.trackId,
      @"enabled" : @(track.isEnabled),
      @"remote" : @(YES),
      @"readyState" : @"live"
    }];
  }

  for (RTCVideoTrack* track in mediaStream.videoTracks) {
    [videoTracks addObject:@{
      @"id" : track.trackId,
      @"kind" : track.kind,
      @"label" : track.trackId,
      @"enabled" : @(track.isEnabled),
      @"remote" : @(YES),
      @"readyState" : @"live"
    }];
  }

  self.localStreams[mediaStreamId] = mediaStream;
  result(
      @{@"streamId" : mediaStreamId, @"audioTracks" : audioTracks, @"videoTracks" : videoTracks});
}

#if TARGET_OS_OSX
// CGImage -> JPEG-NSData (Flutter Image.memory dekodiert JPEG, NICHT TIFF).
static NSData* HCJpegFromCGImage(CGImageRef img) {
  if (img == NULL) return nil;
  NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:img];
  return [rep representationUsingType:NSBitmapImageFileTypeJPEG
                           properties:@{NSImageCompressionFactor : @0.6}];
}

// CGImage aspekt-korrekt auf maxW Breite herunterrechnen + JPEG. Fuer die
// Picker-Vorschauen: die CG-Vollbilder (v.a. Displays) waeren sonst riesig.
static NSData* HCThumbJpegFromCGImage(CGImageRef img, CGFloat maxW) {
  if (img == NULL) return nil;
  size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
  if (w == 0 || h == 0) return nil;
  CGFloat scale = w > maxW ? maxW / (CGFloat)w : 1.0;
  size_t tw = (size_t)MAX((CGFloat)2, (CGFloat)w * scale);
  size_t th = (size_t)MAX((CGFloat)2, (CGFloat)h * scale);
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx = CGBitmapContextCreate(NULL, tw, th, 8, 0, cs,
      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
  CGColorSpaceRelease(cs);
  if (ctx == NULL) return HCJpegFromCGImage(img);
  CGContextSetInterpolationQuality(ctx, kCGInterpolationLow);
  CGContextDrawImage(ctx, CGRectMake(0, 0, tw, th), img);
  CGImageRef scaled = CGBitmapContextCreateImage(ctx);
  CGContextRelease(ctx);
  NSData* jpeg = HCJpegFromCGImage(scaled);
  if (scaled != NULL) CGImageRelease(scaled);
  return jpeg;
}

// Bundle-IDs aller "regulaeren" (Dock-)Apps. Filtert Service-/Helper-Prozesse
// (CursorUIViewService, Open-and-Save-Panel-Service, Menulets) zuverlaessig raus.
- (NSSet<NSString*>*)hcRegularAppBundleIds {
  NSMutableSet<NSString*>* ids = [NSMutableSet set];
  for (NSRunningApplication* a in [NSWorkspace sharedWorkspace].runningApplications) {
    if (a.activationPolicy == NSApplicationActivationPolicyRegular &&
        a.bundleIdentifier != nil) {
      [ids addObject:a.bundleIdentifier];
    }
  }
  return ids;
}

// Fenster-Quellen KOMPLETT via ScreenCaptureKit aufbauen (statt RTCDesktopMediaList):
// eine Quelle -> keine Duplikate, kein Race mit der async-Liste, und Fullscreen-Apps
// auf eigenem Space sind dabei (onScreenWindowsOnly:NO). Stark gefiltert: nur normale
// Fenster (windowLayer==0) regulaerer Dock-Apps, Mindestgroesse, nicht wir selbst.
// sourceId = CGWindowID (wie der SCK-Capture-Pfad erwartet). JPEG-Thumbnails inline
// per SCScreenshotManager (14+); aeltere macOS zeigen Platzhalter.
- (void)buildSckSourcesWithScreens:(NSArray<NSMutableDictionary*>*)screenDicts
                       withWindows:(BOOL)withWindows
                        completion:(void (^)(NSArray<NSDictionary*>* windowDicts))completion
    API_AVAILABLE(macos(12.3)) {
  NSString* ownBundleId = [[NSBundle mainBundle] bundleIdentifier];
  NSSet<NSString*>* regular = [self hcRegularAppBundleIds];
  NSLog(@"[hc-cap] P0 buildSckSources: SCShareableContent REQUEST (Picker)");
  [SCShareableContent
      getShareableContentExcludingDesktopWindows:YES
                             onScreenWindowsOnly:NO
                               completionHandler:^(SCShareableContent* content,
                                                   NSError* error) {
        NSLog(@"[hc-cap] P1 buildSckSources: SCShareableContent RESPONSE err=%@ -> Cache gefuellt", error);
        NSMutableArray<SCWindow*>* wins = [NSMutableArray array];
        NSMutableArray<NSMutableDictionary*>* dicts = [NSMutableArray array];
        NSMutableSet<NSString*>* ids = [NSMutableSet set];
        if (content != nil && withWindows) {
          for (SCWindow* w in content.windows) {
            if (w.windowLayer != 0) continue;
            if (w.frame.size.width < 80 || w.frame.size.height < 60) continue;
            // Nur SICHTBARE Fenster — ODER bildschirmfuellende (eine Fullscreen-App
            // auf eigenem Space meldet isOnScreen=NO, soll aber dabei sein). Das
            // wirft inaktive Hintergrund-/Dialog-Fenster raus (z.B. die vielen
            // versteckten Bambu-Studio-Helferfenster).
            BOOL keep = w.isOnScreen;
            if (!keep) {
              for (SCDisplay* disp in content.displays) {
                if (w.frame.size.width >= disp.frame.size.width * 0.9 &&
                    w.frame.size.height >= disp.frame.size.height * 0.9) {
                  keep = YES;
                  break;
                }
              }
            }
            if (!keep) continue;
            NSString* bid = w.owningApplication.bundleIdentifier;
            if (bid == nil || ![regular containsObject:bid]) continue;
            // ★ HoneyCord 31.08.2026: Das EIGENE Fenster bleibt in der Liste.
            //
            // Hier stand `if (bid == ownBundleId) continue;` — gut gemeint gegen
            // das Spiegelkabinett (wer sein eigenes Fenster teilt, sieht darin
            // die Vorschau davon). Der Nutzer wollte aber genau das: das
            // HoneyCord-Fenster freigeben, etwa um jemandem etwas darin zu
            // zeigen. Discord und Teams erlauben es ebenfalls; die Entscheidung
            // gehoert dem Nutzer, nicht dem Filter.
            //
            // Die uebrigen Filter bleiben unangetastet (Nutzer-Entscheid:
            // „Nur HoneyCord, Rest so lassen") — minimierte Fenster und Apps
            // ohne Dock-Symbol fehlen weiterhin, dafuer bleibt die Liste kurz.
            (void)ownBundleId;
            NSString* appName = w.owningApplication.applicationName ?: @"";
            if (appName.length == 0) continue;
            NSString* sid = [NSString stringWithFormat:@"%u", (unsigned)w.windowID];
            NSString* title = w.title ?: @"";
            NSString* name = title.length > 0
                                 ? [NSString stringWithFormat:@"%@ — %@", appName, title]
                                 : appName;
            [wins addObject:w];
            [dicts addObject:[@{
              @"id" : sid,
              @"name" : name,
              @"thumbnailSize" : @{@"width" : @0, @"height" : @0},
              @"type" : @"window",
            } mutableCopy]];
            [ids addObject:sid];
          }
        } else if (error != nil) {
          NSLog(@"buildSckWindowSources: SCShareableContent error: %@", error);
        }
        _sckExtraWindowIds = ids;

        // Content fuer den Capture-Start cachen: SCShareableContent kann auf
        // macOS 26 (Berechtigungs-XPC) ~20 s dauern — dank Cache startet der
        // Share nach der Picker-Wahl SOFORT statt erneut zu warten.
        [FlutterScreenCaptureKitCapturer cacheShareableContent:content];

        // Liste SOFORT liefern (Picker oeffnet, Namen sichtbar) — die JPEG-
        // Vorschauen kommen danach EINZELN als desktopSourceThumbnailChanged-
        // Events (Dart fuellt live nach). Frueher wartete ein dispatch_group auf
        // ALLE Screenshots; EIN haengender Screenshot (Berechtigungs-XPC)
        // verzoegerte damit die komplette Picker-Liste um ~20 s.
        dispatch_async(dispatch_get_main_queue(), ^{
          completion(dicts);
        });

        // Vorschauen via CoreGraphics auf einem HINTERGRUND-Thread — NICHT via
        // SCScreenshotManager: der teilt sich den SCK-Daemon mit dem echten
        // SCStream.startCapture, und EINE haengende Thumbnail-Aufnahme blockierte
        // den Share-Start ~30 s (macOS 26, GEMESSEN 2026-07-05: T5->T6 = 29 s,
        // T6 fiel exakt mit dem Ende des haengenden Thumbnails zusammen). CG nutzt
        // ein eigenes Subsystem -> selbst eine langsame Vorschau haengt den Share
        // NICHT. Hintergrund-QoS -> kein Main-Thread-Block. (SCK-Fenster-Liste
        // brauchen wir weiter, nur die THUMBNAILS gehen ueber CG.)
        NSMutableArray<NSDictionary*>* tasks = [NSMutableArray array];
        for (NSMutableDictionary* sd in screenDicts) {
          [tasks addObject:@{@"sid" : sd[@"id"], @"win" : @NO,
                             @"wid" : @((uint32_t)[sd[@"id"] longLongValue])}];
        }
        for (NSUInteger i = 0; i < wins.count; i++) {
          [tasks addObject:@{@"sid" : dicts[i][@"id"], @"win" : @YES,
                             @"wid" : @((uint32_t)wins[i].windowID)}];
        }
        NSLog(@"[hc-cap] P2 %lu Thumbnails (CG, Hintergrund)", (unsigned long)tasks.count);
        __weak typeof(self) weakSelf = self;
        for (NSDictionary* t in tasks) {
          NSString* tid = t[@"sid"];
          BOOL isWin = [t[@"win"] boolValue];
          uint32_t idv = (uint32_t)[t[@"wid"] unsignedIntValue];
          dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            CGImageRef img = NULL;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            if (isWin) {
              img = CGWindowListCreateImage(CGRectNull, kCGWindowListOptionIncludingWindow,
                                            (CGWindowID)idv,
                                            kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);
            } else {
              img = CGDisplayCreateImage((CGDirectDisplayID)idv);
            }
#pragma clang diagnostic pop
            NSData* jpeg = HCThumbJpegFromCGImage(img, 480.0);
            if (img != NULL) CGImageRelease(img);
            if (jpeg == nil) return;
            dispatch_async(dispatch_get_main_queue(), ^{
              typeof(self) s2 = weakSelf;
              if (s2 != nil && s2.eventSink != nil) {
                postEvent(s2.eventSink, @{@"event" : @"desktopSourceThumbnailChanged",
                                          @"id" : tid, @"thumbnail" : jpeg});
              }
            });
          });
        }
      }];
}
#endif

- (void)getDesktopSources:(NSDictionary*)argsMap result:(FlutterResult)result {
#if TARGET_OS_OSX
  NSLog(@"getDesktopSources");

  NSArray* types = [argsMap objectForKey:@"types"];
  if (types == nil) {
    result([FlutterError errorWithCode:@"ERROR" message:@"types is required" details:nil]);
    return;
  }

  if (![self buildDesktopSourcesListWithTypes:types forceReload:YES result:result]) {
    NSLog(@"getDesktopSources failed.");
    return;
  }

  NSMutableArray* sources = [NSMutableArray array];
  // Screen-Dicts separat halten — der SCK-Helfer schiebt ihre Vorschau nach.
  NSMutableArray<NSMutableDictionary*>* screenDicts = [NSMutableArray array];
  if ([types containsObject:@"screen"] && [self hcModernSck]) {
    // Screens DIREKT via CoreGraphics (instant, ohne webrtc/SCK-Blocking):
    // id = CGDirectDisplayID — dieselbe ID, auf die SCK-Vorschau (buildSckSources)
    // und Capture-Start (HCIsActiveDisplayId -> SCK initWithDisplay) matchen.
    CGDirectDisplayID ids[16];
    uint32_t n = 0;
    CGGetActiveDisplayList(16, ids, &n);
    for (uint32_t i = 0; i < n; i++) {
      NSMutableDictionary* d = [@{
        @"id" : [NSString stringWithFormat:@"%u", ids[i]],
        @"name" : [NSString stringWithFormat:@"Screen %u", i + 1],
        @"thumbnailSize" : @{@"width" : @0, @"height" : @0},
        @"type" : @"screen",
      } mutableCopy];
      [screenDicts addObject:d];
      [sources addObject:d];
    }
  } else {
    // Legacy (<12.3): Screens aus der RTCDesktopMediaList.
    for (RTCDesktopSource* object in _captureSources) {
      NSMutableDictionary* d = [@{
        @"id" : object.sourceId,
        @"name" : object.name,
        @"thumbnailSize" : @{@"width" : @0, @"height" : @0},
        @"type" : object.sourceType == RTCDesktopSourceTypeScreen ? @"screen" : @"window",
      } mutableCopy];
      if (object.sourceType == RTCDesktopSourceTypeScreen) [screenDicts addObject:d];
      [sources addObject:d];
    }
  }

  // Fenster komplett via ScreenCaptureKit (eine Quelle -> keine Dupes/kein Race,
  // inkl. Fullscreen-Apps) + STATISCHE SCK-Vorschau fuer die Screens (statt der bei
  // Sidecar korrupten Live-Thumbnail). Async -> result() im Completion (Main-Thread).
  BOOL wantWindows = [types containsObject:@"window"];
  if (@available(macOS 12.3, *)) {
    if (wantWindows || screenDicts.count > 0) {
      [self buildSckSourcesWithScreens:screenDicts
                           withWindows:wantWindows
                            completion:^(NSArray<NSDictionary*>* windowDicts) {
        [sources addObjectsFromArray:windowDicts];
        result(@{@"sources" : sources});
      }];
      return;
    }
  }
  result(@{@"sources" : sources});
#else
  result([FlutterError errorWithCode:@"ERROR" message:@"Not supported on iOS" details:nil]);
#endif
}

- (void)getDesktopSourceThumbnail:(NSDictionary*)argsMap result:(FlutterResult)result {
#if TARGET_OS_OSX
  NSLog(@"getDesktopSourceThumbnail");
  NSString* sourceId = argsMap[@"sourceId"];
  // SCK-nachgereichtes Fenster: Vorschau best-effort per CGWindowListCreateImage
  // (greift auch ueber Space-Grenzen fuer eine konkrete windowID).
  if (@available(macOS 12.3, *)) {
    if ([_sckExtraWindowIds containsObject:sourceId]) {
      CGWindowID wid = (CGWindowID)[sourceId longLongValue];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      CGImageRef cg = CGWindowListCreateImage(
          CGRectNull, kCGWindowListOptionIncludingWindow, wid,
          kCGWindowImageBoundsIgnoreFraming | kCGWindowImageNominalResolution);
#pragma clang diagnostic pop
      if (cg != NULL) {
        NSImage* img = [[NSImage alloc] initWithCGImage:cg size:NSZeroSize];
        CGImageRelease(cg);
        NSImage* resized = [self resizeImage:img forSize:NSMakeSize(320, 180)];
        result([resized TIFFRepresentation]);
      } else {
        result(@{@"error" : @"No thumbnail found"});
      }
      return;
    }
  }
  RTCDesktopSource* object = [self getSourceById:sourceId];
  if (object == nil) {
    result(@{@"error" : @"No source found"});
    return;
  }
  NSImage* image = [object UpdateThumbnail];
  if (image != nil) {
    NSImage* resizedImg = [self resizeImage:image forSize:NSMakeSize(320, 180)];
    NSData* data = [resizedImg TIFFRepresentation];
    result(data);
  } else {
    result(@{@"error" : @"No thumbnail found"});
  }

#else
  result([FlutterError errorWithCode:@"ERROR" message:@"Not supported on iOS" details:nil]);
#endif
}

// SCShareableContent vorwaermen + im SCK-Cache ablegen. Auf macOS 26 kostet der
// erste Abruf ~20 s (Berechtigungs-XPC) — beim Voice-Beitritt (und periodisch)
// aufgerufen, damit der Capture-Start den Content aus dem Cache nimmt statt zu
// warten. Antwortet SOFORT (fire-and-forget); die Cache-Fuellung passiert async.
- (void)prewarmDesktopCapture:(FlutterResult)result {
#if TARGET_OS_OSX
  if (@available(macOS 12.3, *)) {
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES
                                               onScreenWindowsOnly:NO
                                                 completionHandler:^(SCShareableContent* content,
                                                                     NSError* error) {
      if (content != nil) {
        [FlutterScreenCaptureKitCapturer cacheShareableContent:content];
      }
    }];
  }
  result(@YES);
#else
  result(@NO);
#endif
}

- (void)updateDesktopSources:(NSDictionary*)argsMap result:(FlutterResult)result {
#if TARGET_OS_OSX
  NSLog(@"updateDesktopSources");
  NSArray* types = [argsMap objectForKey:@"types"];
  if (types == nil) {
    result([FlutterError errorWithCode:@"ERROR" message:@"types is required" details:nil]);
    return;
  }
  if (![self buildDesktopSourcesListWithTypes:types forceReload:NO result:result]) {
    NSLog(@"updateDesktopSources failed.");
    return;
  }
  result(@{@"result" : @YES});
#else
  result([FlutterError errorWithCode:@"ERROR" message:@"Not supported on iOS" details:nil]);
#endif
}

#if TARGET_OS_OSX
- (NSImage*)resizeImage:(NSImage*)sourceImage forSize:(CGSize)targetSize {
  CGSize imageSize = sourceImage.size;
  CGFloat width = imageSize.width;
  CGFloat height = imageSize.height;
  CGFloat targetWidth = targetSize.width;
  CGFloat targetHeight = targetSize.height;
  CGFloat scaleFactor = 0.0;
  CGFloat scaledWidth = targetWidth;
  CGFloat scaledHeight = targetHeight;
  CGPoint thumbnailPoint = CGPointMake(0.0, 0.0);

  if (CGSizeEqualToSize(imageSize, targetSize) == NO) {
    CGFloat widthFactor = targetWidth / width;
    CGFloat heightFactor = targetHeight / height;

    // scale to fit the longer
    scaleFactor = (widthFactor > heightFactor) ? widthFactor : heightFactor;
    scaledWidth = ceil(width * scaleFactor);
    scaledHeight = ceil(height * scaleFactor);

    // center the image
    if (widthFactor > heightFactor) {
      thumbnailPoint.y = (targetHeight - scaledHeight) * 0.5;
    } else if (widthFactor < heightFactor) {
      thumbnailPoint.x = (targetWidth - scaledWidth) * 0.5;
    }
  }

  NSImage* newImage = [[NSImage alloc] initWithSize:NSMakeSize(scaledWidth, scaledHeight)];
  CGRect thumbnailRect = {thumbnailPoint, {scaledWidth, scaledHeight}};
  NSRect imageRect = NSMakeRect(0.0, 0.0, width, height);

  [newImage lockFocus];
    [sourceImage drawInRect:thumbnailRect fromRect:imageRect operation:NSCompositingOperationCopy fraction:1.0];
  [newImage unlockFocus];

  return newImage;
}

// @available laesst sich nicht in zusammengesetzten Bedingungen nutzen -> Helper.
- (BOOL)hcModernSck {
  if (@available(macOS 12.3, *)) {
    return YES;
  }
  return NO;
}

- (RTCDesktopSource*)getSourceById:(NSString*)sourceId {
  NSEnumerator* enumerator = [_captureSources objectEnumerator];
  RTCDesktopSource* object;
  while ((object = enumerator.nextObject) != nil) {
    if ([sourceId isEqualToString:object.sourceId]) {
      return object;
    }
  }
  return nil;
}

- (BOOL)buildDesktopSourcesListWithTypes:(NSArray*)types
                             forceReload:(BOOL)forceReload
                                  result:(FlutterResult)result {
  BOOL captureWindow = NO;
  BOOL captureScreen = NO;
  _captureSources = [NSMutableArray array];

  NSEnumerator* typesEnumerator = [types objectEnumerator];
  NSString* type;
  while ((type = typesEnumerator.nextObject) != nil) {
    if ([type isEqualToString:@"screen"]) {
      captureScreen = YES;
    } else if ([type isEqualToString:@"window"]) {
      captureWindow = YES;
    } else {
      result([FlutterError errorWithCode:@"ERROR" message:@"Invalid type" details:nil]);
      return NO;
    }
  }

  if (!captureWindow && !captureScreen) {
    result([FlutterError errorWithCode:@"ERROR"
                               message:@"At least one type is required"
                               details:nil]);
    return NO;
  }

  if (forceReload) {
    _screen = nil;
    _window = nil;
  }

  if (captureWindow) {
    if (@available(macOS 12.3, *)) {
      // Fenster kommen via ScreenCaptureKit (buildSckWindowSources) — hier bewusst
      // NICHT ueber RTCDesktopMediaList (sonst Dupes + Race mit dessen async-Liste).
    } else {
      // Fallback auf altem macOS ohne SCK: RTCDesktopMediaList.
      if (!_window)
        _window = [[RTCDesktopMediaList alloc] initWithType:RTCDesktopSourceTypeWindow delegate:self];
      [_window UpdateSourceList:forceReload updateAllThumbnails:YES];
      NSArray<RTCDesktopSource*>* sources = [_window getSources];
      _captureSources = [_captureSources arrayByAddingObjectsFromArray:sources];
    }
  }
  if (captureScreen) {
    if (@available(macOS 12.3, *)) {
      // KEINE RTCDesktopMediaList fuer Screens mehr: deren GetSourceList blockierte
      // auf macOS 26 den MAIN-THREAD ~20-30 s (SCK-/Berechtigungs-XPC hinter einer
      // Semaphore -> Beachball beim Picker-Oeffnen, User-Report 2026-07-05).
      // getDesktopSources baut Screens direkt via CGGetActiveDisplayList (instant);
      // der Capture-Pfad erkennt DisplayIDs via HCIsActiveDisplayId; die Vorschau
      // kommt weiterhin statisch via SCK (buildSckSources).
    } else {
      if (!_screen)
        _screen = [[RTCDesktopMediaList alloc] initWithType:RTCDesktopSourceTypeScreen delegate:self];
      [_screen UpdateSourceList:forceReload updateAllThumbnails:YES];
      NSArray<RTCDesktopSource*>* sources = [_screen getSources];
      _captureSources = [_captureSources arrayByAddingObjectsFromArray:sources];
    }
  }
  NSLog(@"captureSources: %lu", [_captureSources count]);
  return YES;
}

#pragma mark - RTCDesktopMediaListDelegate delegate

#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
- (void)didDesktopSourceAdded:(RTC_OBJC_TYPE(RTCDesktopSource) *)source {
  // NSLog(@"didDesktopSourceAdded: %@, id %@", source.name, source.sourceId);
  if (self.eventSink) {
    // Screens ab 12.3: KEIN Legacy-UpdateThumbnail (Vollbild-CG-Screenshot,
    // blockiert auf macOS 26 den Main-Thread via XPC-Gate; Vorschau kommt
    // ohnehin statisch via SCK) — gleiche Logik wie didDesktopSourceThumbnailChanged.
    BOOL skipLegacyThumb = NO;
    if (@available(macOS 12.3, *)) {
      skipLegacyThumb = (source.sourceType == RTCDesktopSourceTypeScreen);
    }
    NSImage* image = skipLegacyThumb ? nil : [source UpdateThumbnail];
    NSData* data = [[NSData alloc] init];
    if (image != nil) {
      NSImage* resizedImg = [self resizeImage:image forSize:NSMakeSize(320, 180)];
      data = [resizedImg TIFFRepresentation];
    }
    postEvent(self.eventSink, @{
      @"event" : @"desktopSourceAdded",
      @"id" : source.sourceId,
      @"name" : source.name,
      @"thumbnailSize" : @{@"width" : @0, @"height" : @0},
      @"type" : source.sourceType == RTCDesktopSourceTypeScreen ? @"screen" : @"window",
      @"thumbnail" : data
    });
  }
}

#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
- (void)didDesktopSourceRemoved:(RTC_OBJC_TYPE(RTCDesktopSource) *)source {
  // NSLog(@"didDesktopSourceRemoved: %@, id %@", source.name, source.sourceId);
  if (self.eventSink) {
    postEvent(self.eventSink, @{
      @"event" : @"desktopSourceRemoved",
      @"id" : source.sourceId,
    });
  }
}

#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
- (void)didDesktopSourceNameChanged:(RTC_OBJC_TYPE(RTCDesktopSource) *)source {
  // NSLog(@"didDesktopSourceNameChanged: %@, id %@", source.name, source.sourceId);
  if (self.eventSink) {
    postEvent(self.eventSink, @{
      @"event" : @"desktopSourceNameChanged",
      @"id" : source.sourceId,
      @"name" : source.name,
    });
  }
}

#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
- (void)didDesktopSourceThumbnailChanged:(RTC_OBJC_TYPE(RTCDesktopSource) *)source {
  // NSLog(@"didDesktopSourceThumbnailChanged: %@, id %@", source.name, source.sourceId);
  // Screens bekommen ab 12.3 eine STATISCHE SCK-JPEG-Vorschau inline (getDesktop-
  // Sources). Die Live-TIFF-Thumbnail hier wuerde sie ueberschreiben — und Flutter
  // dekodiert TIFF eh nicht (-> schwarz; bei drahtlosem Sidecar zudem korrupt).
  if (@available(macOS 12.3, *)) {
    if (source.sourceType == RTCDesktopSourceTypeScreen) return;
  }
  if (self.eventSink) {
    NSImage* resizedImg = [self resizeImage:[source thumbnail] forSize:NSMakeSize(320, 180)];
    NSData* data = [resizedImg TIFFRepresentation];
    postEvent(self.eventSink, @{
      @"event" : @"desktopSourceThumbnailChanged",
      @"id" : source.sourceId,
      @"thumbnail" : data
    });
  }
}

#pragma mark - RTCDesktopCapturerDelegate delegate

- (void)didSourceCaptureStart:(RTCDesktopCapturer*)capturer {
  NSLog(@"didSourceCaptureStart");
}

- (void)didSourceCapturePaused:(RTCDesktopCapturer*)capturer {
  NSLog(@"didSourceCapturePaused");
}

- (void)didSourceCaptureStop:(RTCDesktopCapturer*)capturer {
  NSLog(@"didSourceCaptureStop");
}

- (void)didSourceCaptureError:(RTCDesktopCapturer*)capturer {
  NSLog(@"didSourceCaptureError");
}

#endif


#if TARGET_OS_OSX
- (AVCaptureDevice *)honeycordTonGeraetFuer:(NSString *)deviceId {
  if (deviceId.length == 0) return nil;
  AVCaptureDevice *d = [AVCaptureDevice deviceWithUniqueID:deviceId];
  if (d != nil && [d hasMediaType:AVMediaTypeAudio]) return d;
  // Rueckfall: alle Tongeraete durchgehen (uniqueID ODER Name).
  NSArray<AVCaptureDevice *> *alle = nil;
  if (@available(macOS 14.0, *)) {
    AVCaptureDeviceDiscoverySession *ds =
        [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeMicrophone, AVCaptureDeviceTypeExternal ]
                                                               mediaType:AVMediaTypeAudio
                                                                position:AVCaptureDevicePositionUnspecified];
    alle = ds.devices;
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    alle = [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio];
#pragma clang diagnostic pop
  }
  for (AVCaptureDevice *k in alle) {
    if ([k.uniqueID isEqualToString:deviceId] || [k.localizedName isEqualToString:deviceId]) return k;
  }
  return nil;
}
#endif

- (void)honeycordCaptureAudioStart:(NSString *)deviceId result:(FlutterResult)result {
#if TARGET_OS_OSX
  AVCaptureDevice *geraet = [self honeycordTonGeraetFuer:deviceId];
  if (geraet == nil) {
    NSLog(@"[geraete-ton] Geraet nicht gefunden: %@", deviceId);
    result([FlutterError errorWithCode:@"captureAudio" message:@"Aufnahmegeraet nicht gefunden" details:deviceId]);
    return;
  }
  // Mikrofon-Freigabe: die App hat sie in der Regel schon (Mikrofon laeuft);
  // sonst hier anfragen und im Rueckruf starten — Start OHNE Freigabe liefert
  // stumm null Puffer, und niemand saehe warum.
  __weak FlutterWebRTCPlugin *weakSelf = self;
  void (^starten)(void) = ^{
    FlutterWebRTCPlugin *me = weakSelf;
    if (me == nil) { result([FlutterError errorWithCode:@"captureAudio" message:@"Plugin weg" details:nil]); return; }
    RTCCustomAudioSource *quelle = [[RTCCustomAudioSource alloc] initWithFactory:me.peerConnectionFactory];
    HoneycordScreenAudioRelay *relay = [[HoneycordScreenAudioRelay alloc] init];
    relay.source = quelle;
    HoneycordGeraeteTonAufnehmer *auf = [[HoneycordGeraeteTonAufnehmer alloc] init];
    auf.relay = relay;
    auf.source = quelle;
    NSError *fehler = nil;
    if (![auf vorbereitenMitGeraet:geraet fehler:&fehler]) {
      [quelle stop];
      NSLog(@"[geraete-ton] Vorbereitung fehlgeschlagen: %@", fehler.localizedDescription);
      result([FlutterError errorWithCode:@"captureAudio"
                                 message:@"Aufnahmegeraet liess sich nicht oeffnen"
                                 details:fehler.localizedDescription]);
      return;
    }
    // Start im Hintergrund; Registrierung und `result` danach auf Main.
    [auf startenMitGeraet:geraet fertig:^(BOOL laeuft) {
      FlutterWebRTCPlugin *me2 = weakSelf;
      if (!laeuft || me2 == nil) {
        [auf stop];
        result([FlutterError errorWithCode:@"captureAudio"
                                   message:@"Aufnahmegeraet startete nicht"
                                   details:geraet.localizedName]);
        return;
      }
      NSString *streamId = [[NSUUID UUID] UUIDString];
      NSString *trackId = [[NSUUID UUID] UUIDString];
      RTCMediaStream *stream = [me2.peerConnectionFactory mediaStreamWithStreamId:streamId];
      RTCAudioTrack *spur = [me2.peerConnectionFactory audioTrackWithSource:quelle trackId:trackId];
      [stream addAudioTrack:spur];
      LocalAudioTrack *lokal = [[LocalAudioTrack alloc] initWithTrack:spur];
      [me2.localTracks setObject:lokal forKey:trackId];
      me2.localStreams[streamId] = stream;
      auf.streamId = streamId;
      auf.trackId = trackId;
      me2.honeycordTonAufnehmer[streamId] = auf;
      me2.honeycordTonAufnehmer[trackId] = auf;
      NSLog(@"[geraete-ton] Stream %@ Spur %@ fuer %@", streamId, trackId, geraet.localizedName);
      result(@{
        @"streamId" : streamId,
        @"audioTracks" : @[ @{
          @"id" : trackId,
          @"kind" : spur.kind,
          @"label" : @"capture-card-audio",
          @"enabled" : @(spur.isEnabled),
          @"remote" : @(NO),
          @"readyState" : @"live",
        } ],
        @"videoTracks" : @[],
      });
    }];
  };
  AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
  if (st == AVAuthorizationStatusAuthorized) {
    starten();
  } else if (st == AVAuthorizationStatusNotDetermined) {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
      dispatch_async(dispatch_get_main_queue(), ^{
        if (granted) starten();
        else result([FlutterError errorWithCode:@"captureAudio" message:@"Mikrofon-Freigabe verweigert" details:nil]);
      });
    }];
  } else {
    result([FlutterError errorWithCode:@"captureAudio" message:@"Mikrofon-Freigabe fehlt" details:nil]);
  }
#else
  result([FlutterError errorWithCode:@"captureAudio" message:@"nur macOS" details:nil]);
#endif
}

- (void)honeycordCaptureAudioStopFuer:(NSString *)streamOderTrackId {
#if TARGET_OS_OSX
  HoneycordGeraeteTonAufnehmer *auf = self.honeycordTonAufnehmer[streamOderTrackId];
  if (auf == nil) return;
  [auf stop];
  if (auf.streamId) [self.honeycordTonAufnehmer removeObjectForKey:auf.streamId];
  if (auf.trackId) [self.honeycordTonAufnehmer removeObjectForKey:auf.trackId];
#endif
}

@end
