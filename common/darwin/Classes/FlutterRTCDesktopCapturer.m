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
  size_t _chunkCap;     // capacity in Frames
  size_t _chunkLen;     // momentan belegte Frames
  size_t _chunkChannels;
}
@property(nonatomic, strong) RTCCustomAudioSource *source;
@end

@implementation HoneycordScreenAudioRelay

- (void)screenCapturerDidOutputAudioBuffer:(CMSampleBufferRef)sampleBuffer {
  static uint64_t entries = 0;
  entries++;
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
#endif

@implementation FlutterWebRTCPlugin (DesktopCapturer)

- (void)getDisplayMedia:(NSDictionary*)constraints result:(FlutterResult)result {
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

  if (useDefaultScreen) {
    useScreenCaptureKit = YES;
  } else {
    source = [self getSourceById:sourceId];
    if (source == nil) {
      result(@{@"error" : [NSString stringWithFormat:@"No source found for id: %@", sourceId]});
      return;
    }
    if (source.sourceType == RTCDesktopSourceTypeScreen) {
      useScreenCaptureKit = YES;
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
  NSEnumerator* enumerator = [_captureSources objectEnumerator];
  RTCDesktopSource* object;
  while ((object = enumerator.nextObject) != nil) {
    /*NSData *data = nil;
    if([object thumbnail]) {
        data = [[NSData alloc] init];
        NSImage *resizedImg = [self resizeImage:[object thumbnail] forSize:NSMakeSize(320, 180)];
        data = [resizedImg TIFFRepresentation];
    }*/
    [sources addObject:@{
      @"id" : object.sourceId,
      @"name" : object.name,
      @"thumbnailSize" : @{@"width" : @0, @"height" : @0},
      @"type" : object.sourceType == RTCDesktopSourceTypeScreen ? @"screen" : @"window",
      //@"thumbnail": data,
    }];
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
    if (!_window)
      _window = [[RTCDesktopMediaList alloc] initWithType:RTCDesktopSourceTypeWindow delegate:self];
    [_window UpdateSourceList:forceReload updateAllThumbnails:YES];
    NSArray<RTCDesktopSource*>* sources = [_window getSources];
    _captureSources = [_captureSources arrayByAddingObjectsFromArray:sources];
  }
  if (captureScreen) {
    if (!_screen)
      _screen = [[RTCDesktopMediaList alloc] initWithType:RTCDesktopSourceTypeScreen delegate:self];
    [_screen UpdateSourceList:forceReload updateAllThumbnails:YES];
    NSArray<RTCDesktopSource*>* sources = [_screen getSources];
    _captureSources = [_captureSources arrayByAddingObjectsFromArray:sources];
  }
  NSLog(@"captureSources: %lu", [_captureSources count]);
  return YES;
}

#pragma mark - RTCDesktopMediaListDelegate delegate

#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
- (void)didDesktopSourceAdded:(RTC_OBJC_TYPE(RTCDesktopSource) *)source {
  // NSLog(@"didDesktopSourceAdded: %@, id %@", source.name, source.sourceId);
  if (self.eventSink) {
    NSImage* image = [source UpdateThumbnail];
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

@end
