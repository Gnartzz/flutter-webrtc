#import <Foundation/Foundation.h>
#import <WebRTC/WebRTC.h>

/// Delegate für Audio-Samples aus dem SCStream (System-Audio des freigegebenen
/// Bildschirms). Wird nur aufgerufen, wenn der Capturer mit `captureAudio:YES`
/// gestartet wurde — die Samples sind dann nicht-interleaved Float32 PCM, das
/// der Aufrufer (FlutterRTCDesktopCapturer) in s16-PCM konvertiert und an die
/// RTCCustomAudioSource pusht.
@protocol FlutterScreenCaptureKitAudioDelegate <NSObject>
- (void)screenCapturerDidOutputAudioBuffer:(CMSampleBufferRef _Nonnull)sampleBuffer;
@end

@interface FlutterScreenCaptureKitCapturer : NSObject

- (instancetype)initWithDelegate:(id<RTCVideoCapturerDelegate>)delegate;

@property(nonatomic, strong) id<FlutterScreenCaptureKitAudioDelegate> _Nullable audioDelegate;

- (void)startCaptureWithFPS:(NSInteger)fps
                   sourceId:(NSString* _Nullable)sourceId
               captureAudio:(BOOL)captureAudio
                  onStarted:(void (^)(NSError * _Nullable error))onStarted;

- (void)stopCaptureWithCompletion:(void (^)(void))completion;

@end
