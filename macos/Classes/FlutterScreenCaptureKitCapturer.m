#import "FlutterScreenCaptureKitCapturer.h"

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

@implementation FlutterScreenCaptureKitCapturer

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
                   maxWidth:(NSInteger)maxWidth
                  maxHeight:(NSInteger)maxHeight
                  onStarted:(void (^)(NSError * _Nullable error))onStarted {
#if __has_include(<ScreenCaptureKit/ScreenCaptureKit.h>)
  if (@available(macOS 12.3, *)) {
    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *error) {
      if (error != nil) {
        onStarted(error);
        return;
      }

      SCDisplay *display = [self selectDisplayFromContent:content sourceId:sourceId];
      if (display == nil) {
        NSError *noDisplay = [NSError errorWithDomain:@"FlutterScreenCaptureKit"
                                                 code:-1
                                             userInfo:@{NSLocalizedDescriptionKey: @"No matching display"}];
        onStarted(noDisplay);
        return;
      }

      SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
      SCStreamConfiguration *config = [SCStreamConfiguration new];
      // GPU-seitiges Downscale: SCK skaliert beim Capturen (gratis, zero-copy) in
      // die Deckel-Box statt die volle Display-Aufloesung zu liefern und sie
      // spaeter per webrtc-CPU-I420-Scaling zu verkleinern. Aspekt-korrekt (gleicher
      // Faktor auf W+H), nie hochskaliert, gerade Kanten (H.264 4:2:0).
      NSInteger outW = display.width;
      NSInteger outH = display.height;
      if (maxWidth > 0 && maxHeight > 0 && display.width > 0 && display.height > 0) {
        double scale = fmin(fmin((double)maxWidth / (double)display.width,
                                 (double)maxHeight / (double)display.height),
                            1.0);
        outW = ((NSInteger)lround((double)display.width * scale)) & ~(NSInteger)1;
        outH = ((NSInteger)lround((double)display.height * scale)) & ~(NSInteger)1;
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

      [self.stream startCaptureWithCompletionHandler:^(NSError * _Nullable startError) {
        onStarted(startError);
      }];
    }];
    return;
  }
#endif

  NSError *unavailable = [NSError errorWithDomain:@"FlutterScreenCaptureKit"
                                             code:-2
                                         userInfo:@{NSLocalizedDescriptionKey: @"ScreenCaptureKit not available"}];
  onStarted(unavailable);
}

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
