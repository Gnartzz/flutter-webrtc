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

/// Prozessweiter SCShareableContent-Cache. Jede SCShareableContent-Abfrage kann
/// auf macOS 26 durch den Berechtigungs-Daemon ~20 s dauern — der Picker holt den
/// Content ohnehin frisch; Capture-Start & Co. nutzen den Cache (TTL) statt eine
/// zweite teure Abfrage zu starten. Thread-safe (interner Lock).
+ (void)cacheShareableContent:(id _Nullable)content;
+ (id _Nullable)cachedShareableContentMaxAge:(NSTimeInterval)maxAge;

@property(nonatomic, strong) id<FlutterScreenCaptureKitAudioDelegate> _Nullable audioDelegate;

/// `isWindow`=YES nimmt EIN FENSTER zero-copy auf (SCContentFilter
/// initWithDesktopIndependentWindow:, `sourceId` = CGWindowID) — das Mac-Gegenstueck
/// zur Windows-WGC-Fenster-Capture; sonst den ganzen Bildschirm (Display per
/// displayID). `maxWidth`/`maxHeight` = Deckel-Box fuer GPU-seitiges Downscale
/// direkt in der SCStreamConfiguration (aspekt-korrekt, nie hochskaliert; 0 = nativ).
- (void)startCaptureWithFPS:(NSInteger)fps
                   sourceId:(NSString* _Nullable)sourceId
               captureAudio:(BOOL)captureAudio
                   isWindow:(BOOL)isWindow
                   maxWidth:(NSInteger)maxWidth
                  maxHeight:(NSInteger)maxHeight
                  onStarted:(void (^)(NSError * _Nullable error))onStarted;

- (void)stopCaptureWithCompletion:(void (^)(void))completion;

@end
