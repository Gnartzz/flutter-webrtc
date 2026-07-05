#import "FlutterScreenCaptureKitCapturer.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>

#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#endif

@interface FlutterScreenCaptureKitCapturer ()
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
<SCStreamOutput>
#endif
@property(nonatomic, strong) RTCVideoCapturer *capturer;
@property(nonatomic, weak) id<RTCVideoCapturerDelegate> delegate;
@property(nonatomic, strong) dispatch_queue_t captureQueue;
@property(nonatomic, strong) dispatch_queue_t audioQueue;
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
@property(nonatomic, strong) SCStream *stream;
#endif
@end

// ── Prozessweiter SCShareableContent-Cache ─────────────────────────────────────
// Jede SCShareableContent-Abfrage kann auf macOS 26 (Berechtigungs-Daemon/XPC)
// ~20 s dauern. Der Picker fragt frisch ab und fuettert den Cache; Capture-Start
// nutzt ihn (TTL) -> der Share startet direkt nach der Auswahl statt erneut zu
// warten. Zugriff via Lock (Abfragen kommen von Main- und Hintergrund-Queues).
static id _hcSckContent = nil;             // SCShareableContent (id: pre-12.3-Build)
static NSDate* _hcSckContentDate = nil;
static NSLock* _hcSckContentLock = nil;

@implementation FlutterScreenCaptureKitCapturer

+ (void)initialize {
  if (self == [FlutterScreenCaptureKitCapturer class]) {
    _hcSckContentLock = [[NSLock alloc] init];
  }
}

+ (void)cacheShareableContent:(id)content {
  if (content == nil) return;
  [_hcSckContentLock lock];
  _hcSckContent = content;
  _hcSckContentDate = [NSDate date];
  [_hcSckContentLock unlock];
}

+ (id)cachedShareableContentMaxAge:(NSTimeInterval)maxAge {
  [_hcSckContentLock lock];
  id content = nil;
  if (_hcSckContent != nil && _hcSckContentDate != nil &&
      -[_hcSckContentDate timeIntervalSinceNow] <= maxAge) {
    content = _hcSckContent;
  }
  [_hcSckContentLock unlock];
  return content;
}

- (instancetype)initWithDelegate:(id<RTCVideoCapturerDelegate>)delegate {
  self = [super init];
  if (self) {
    _delegate = delegate;
    _capturer = [[RTCVideoCapturer alloc] initWithDelegate:delegate];
    _captureQueue = dispatch_queue_create("com.iperius.sck.capture", DISPATCH_QUEUE_SERIAL);
    _audioQueue = dispatch_queue_create("com.honeycord.sck.audio", DISPATCH_QUEUE_SERIAL);
  }
  return self;
}

- (void)startCaptureWithFPS:(NSInteger)fps
                   sourceId:(NSString* _Nullable)sourceId
               captureAudio:(BOOL)captureAudio
                   isWindow:(BOOL)isWindow
                   maxWidth:(NSInteger)maxWidth
                  maxHeight:(NSInteger)maxHeight
                  onStarted:(void (^)(NSError * _Nullable error))onStarted {
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
  if (@available(macOS 12.3, *)) {
    NSLog(@"[hc-cap] T0 startCaptureWithFPS entry isWindow=%d src=%@", (int)isWindow, sourceId);
    // Cache zuerst: SCShareableContent kann auf macOS 26 (Berechtigungs-XPC)
    // ~20 s dauern. Der Picker hat den Content gerade geholt -> der Share startet
    // direkt nach der Auswahl. Cache-Miss (z.B. neues Fenster) -> frisch holen.
    SCShareableContent *cached = (SCShareableContent *)
        [FlutterScreenCaptureKitCapturer cachedShareableContentMaxAge:60.0];
    NSLog(@"[hc-cap] T1 cache %@", cached ? @"HIT" : @"MISS");
    if (cached != nil) {
      [self hcStartWithContent:cached fromCache:YES fps:fps sourceId:sourceId
                  captureAudio:captureAudio isWindow:isWindow
                      maxWidth:maxWidth maxHeight:maxHeight onStarted:onStarted];
    } else {
      [self hcFetchContentAndStartWithFPS:fps sourceId:sourceId
                             captureAudio:captureAudio isWindow:isWindow
                                 maxWidth:maxWidth maxHeight:maxHeight onStarted:onStarted];
    }
    return;
  }
#endif

  NSError *unavailable = [NSError errorWithDomain:@"FlutterScreenCaptureKit"
                                             code:-2
                                         userInfo:@{NSLocalizedDescriptionKey: @"ScreenCaptureKit not available"}];
  onStarted(unavailable);
}

#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
- (void)hcFetchContentAndStartWithFPS:(NSInteger)fps
                             sourceId:(NSString* _Nullable)sourceId
                         captureAudio:(BOOL)captureAudio
                             isWindow:(BOOL)isWindow
                             maxWidth:(NSInteger)maxWidth
                            maxHeight:(NSInteger)maxHeight
                            onStarted:(void (^)(NSError * _Nullable error))onStarted
    API_AVAILABLE(macos(12.3)) {
  NSLog(@"[hc-cap] T2 getShareableContent REQUEST (fresh fetch)");
  [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *error) {
    NSLog(@"[hc-cap] T3 getShareableContent RESPONSE err=%@", error);
    if (error != nil) {
      onStarted(error);
      return;
    }
    [FlutterScreenCaptureKitCapturer cacheShareableContent:content];
    [self hcStartWithContent:content fromCache:NO fps:fps sourceId:sourceId
                captureAudio:captureAudio isWindow:isWindow
                    maxWidth:maxWidth maxHeight:maxHeight onStarted:onStarted];
  }];
}

- (void)hcStartWithContent:(SCShareableContent *)content
                 fromCache:(BOOL)fromCache
                       fps:(NSInteger)fps
                  sourceId:(NSString* _Nullable)sourceId
              captureAudio:(BOOL)captureAudio
                  isWindow:(BOOL)isWindow
                  maxWidth:(NSInteger)maxWidth
                 maxHeight:(NSInteger)maxHeight
                 onStarted:(void (^)(NSError * _Nullable error))onStarted
    API_AVAILABLE(macos(12.3)) {
      // Quelle waehlen: FENSTER (SCWindow, zero-copy wie Bildschirm) oder DISPLAY.
      SCContentFilter *filter = nil;
      NSInteger srcW = 0, srcH = 0;
      if (isWindow) {
        SCWindow *win = [self selectWindowFromContent:content sourceId:sourceId];
        if (win == nil) {
          if (fromCache) {
            // Cache zu alt (Fenster nach dem Cachen geoeffnet) -> frisch holen.
            [self hcFetchContentAndStartWithFPS:fps sourceId:sourceId
                                   captureAudio:captureAudio isWindow:isWindow
                                       maxWidth:maxWidth maxHeight:maxHeight onStarted:onStarted];
            return;
          }
          onStarted([NSError errorWithDomain:@"FlutterScreenCaptureKit" code:-3
                      userInfo:@{NSLocalizedDescriptionKey: @"No matching window"}]);
          return;
        }
        filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:win];
        // SCWindow.frame ist in Punkten -> in Pixel umrechnen (Retina), sonst
        // capturet SCK das Fenster weich. Box-Fit (unten) deckelt es danach.
        CGFloat sc = [NSScreen mainScreen].backingScaleFactor;
        if (sc < 1.0) sc = 1.0;
        srcW = (NSInteger)lround(win.frame.size.width * sc);
        srcH = (NSInteger)lround(win.frame.size.height * sc);
      } else {
        SCDisplay *display = [self selectDisplayFromContent:content sourceId:sourceId];
        if (display == nil) {
          if (fromCache) {
            [self hcFetchContentAndStartWithFPS:fps sourceId:sourceId
                                   captureAudio:captureAudio isWindow:isWindow
                                       maxWidth:maxWidth maxHeight:maxHeight onStarted:onStarted];
            return;
          }
          onStarted([NSError errorWithDomain:@"FlutterScreenCaptureKit" code:-1
                      userInfo:@{NSLocalizedDescriptionKey: @"No matching display"}]);
          return;
        }
        filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
        // SCDisplay meldet PUNKTE. Fuer Retina-/Scaled-Modes mit dem
        // backingScaleFactor des zugehoerigen NSScreen in PIXEL umrechnen —
        // sonst captured SCK unscharf unterhalb nativ (User-Report 2026-07-05:
        // 32:9 im Scaled-Mode 3360x945 Punkte -> Share lief mit „944p").
        CGFloat dsc = 1.0;
        for (NSScreen *scr in [NSScreen screens]) {
          NSNumber *num = scr.deviceDescription[@"NSScreenNumber"];
          if (num != nil && (CGDirectDisplayID)num.unsignedIntValue == display.displayID) {
            dsc = scr.backingScaleFactor;
            break;
          }
        }
        if (dsc < 1.0) dsc = 1.0;
        srcW = (NSInteger)lround((double)display.width * dsc);
        srcH = (NSInteger)lround((double)display.height * dsc);
      }

      SCStreamConfiguration *config = [SCStreamConfiguration new];
      // GPU-seitiges Downscale: SCK skaliert beim Capturen (gratis, zero-copy) in
      // die Deckel-Box statt die volle Quelle zu liefern und sie spaeter per
      // webrtc-CPU-I420-Scaling zu verkleinern. Aspekt-korrekt, nie hochskaliert,
      // gerade Kanten (H.264 4:2:0).
      NSInteger outW = srcW;
      NSInteger outH = srcH;
      if (maxWidth > 0 && maxHeight > 0 && srcW > 0 && srcH > 0) {
        double scale = fmin(fmin((double)maxWidth / (double)srcW,
                                 (double)maxHeight / (double)srcH),
                            1.0);
        outW = ((NSInteger)lround((double)srcW * scale)) & ~(NSInteger)1;
        outH = ((NSInteger)lround((double)srcH * scale)) & ~(NSInteger)1;
        if (outW < 2) outW = 2;
        if (outH < 2) outH = 2;
      }
      config.width = outW;
      config.height = outH;
      config.minimumFrameInterval = CMTimeMake(1, (int32_t)MAX(1, fps));
      config.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
      if (@available(macOS 13.0, *)) {
        config.showsCursor = YES;
        // System-Audio des freigegebenen Bildschirms aufnehmen (macOS 13+).
        // Eigene Mikrofon-Audio wird NICHT mit erfasst — das bleibt der
        // separaten AudioDeviceModule-Pipeline überlassen.
        if (captureAudio) {
          config.capturesAudio = YES;
          // 48 kHz / 2 Kanäle passt zur WebRTC-Standardpipeline; ScreenCaptureKit
          // liefert Float32 nicht-interleaved, wir konvertieren später in s16.
          config.sampleRate = 48000;
          config.channelCount = 2;
          config.excludesCurrentProcessAudio = YES;
        }
      }

      NSLog(@"[hc-cap] T4 filter+config ready (srcW=%ld srcH=%ld out=%ldx%ld) -> SCStream alloc",
            (long)srcW, (long)srcH, (long)config.width, (long)config.height);
      self.stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:nil];
      NSError *addOutputError = nil;
      [self.stream addStreamOutput:self
                              type:SCStreamOutputTypeScreen
               sampleHandlerQueue:self.captureQueue
                            error:&addOutputError];
      if (addOutputError != nil) {
        onStarted(addOutputError);
        return;
      }
      // Audio-Output separat hinzufügen, eigener Queue zur Vermeidung von
      // Backpressure zwischen Video- und Audio-Pfad.
      if (captureAudio && @available(macOS 13.0, *)) {
        NSError *audioOutputError = nil;
        BOOL ok = [self.stream addStreamOutput:self
                                          type:SCStreamOutputTypeAudio
                            sampleHandlerQueue:self.audioQueue
                                         error:&audioOutputError];
        NSLog(@"[scr-audio sck] addStreamOutput audio: ok=%d err=%@ "
              @"capturesAudio=%d sr=%lu ch=%lu excludeOwn=%d",
              ok, audioOutputError,
              config.capturesAudio,
              (unsigned long)config.sampleRate,
              (unsigned long)config.channelCount,
              config.excludesCurrentProcessAudio);
      }

      NSLog(@"[hc-cap] T5 SCStream startCapture REQUEST");
      [self.stream startCaptureWithCompletionHandler:^(NSError * _Nullable startError) {
        NSLog(@"[hc-cap] T6 SCStream startCapture RESPONSE err=%@", startError);
        onStarted(startError);
      }];
}
#endif

- (void)stopCaptureWithCompletion:(void (^)(void))completion {
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
  if (@available(macOS 12.3, *)) {
    if (self.stream == nil) {
      completion();
      return;
    }
    SCStream *stream = self.stream;
    self.stream = nil;
    [stream stopCaptureWithCompletionHandler:^(__unused NSError * _Nullable error) {
      completion();
    }];
    return;
  }
#endif
  completion();
}

#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
- (SCDisplay *)selectDisplayFromContent:(SCShareableContent *)content
                               sourceId:(NSString *)sourceId API_AVAILABLE(macos(12.3)) {
  if (content.displays.count == 0) {
    return nil;
  }

  if (sourceId != nil && sourceId.length > 0) {
    for (SCDisplay *display in content.displays) {
      if ([[NSString stringWithFormat:@"%u", display.displayID] isEqualToString:sourceId]) {
        return display;
      }
    }
  }

  CGDirectDisplayID mainDisplay = CGMainDisplayID();
  for (SCDisplay *display in content.displays) {
    if (display.displayID == mainDisplay) {
      return display;
    }
  }

  return content.displays.firstObject;
}

// Fenster per CGWindowID (= sourceId aus dem Picker) finden. nil -> Aufrufer
// liefert "No matching window" (z.B. Fenster inzwischen geschlossen).
- (SCWindow *)selectWindowFromContent:(SCShareableContent *)content
                             sourceId:(NSString *)sourceId API_AVAILABLE(macos(12.3)) {
  if (sourceId == nil || sourceId.length == 0 || content.windows.count == 0) {
    return nil;
  }
  CGWindowID wid = (CGWindowID)[sourceId longLongValue];
  for (SCWindow *w in content.windows) {
    if (w.windowID == wid) {
      return w;
    }
  }
  return nil;
}

- (void)stream:(SCStream *)stream
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        ofType:(SCStreamOutputType)type API_AVAILABLE(macos(12.3)) {
  if (type == SCStreamOutputTypeScreen) {
    CVPixelBufferRef pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (pixelBuffer == nil) {
      return;
    }
    CMTime timestamp = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    int64_t timeStampNs = (int64_t)(CMTimeGetSeconds(timestamp) * 1000000000.0);
    id<RTCVideoFrameBuffer> rtcBuffer = [[RTCCVPixelBuffer alloc] initWithPixelBuffer:pixelBuffer];
    RTCVideoFrame *frame = [[RTCVideoFrame alloc] initWithBuffer:rtcBuffer
                                                        rotation:RTCVideoRotation_0
                                                     timeStampNs:timeStampNs];
    [self.delegate capturer:self.capturer didCaptureVideoFrame:frame];
    return;
  }
  if (@available(macOS 13.0, *)) {
    if (type == SCStreamOutputTypeAudio) {
      static uint64_t audioCalls = 0;
      audioCalls++;
      id<FlutterScreenCaptureKitAudioDelegate> delegate = self.audioDelegate;
      if (audioCalls == 1 || (audioCalls % 200) == 0) {
        NSLog(@"[scr-audio sck] audio buffer #%llu delegate=%@",
              (unsigned long long)audioCalls, delegate);
      }
      if (delegate != nil) {
        [delegate screenCapturerDidOutputAudioBuffer:sampleBuffer];
      }
    }
  }
}
#endif

@end
