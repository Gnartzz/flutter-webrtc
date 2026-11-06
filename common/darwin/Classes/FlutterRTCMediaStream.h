#import <Foundation/Foundation.h>
#import "FlutterWebRTCPlugin.h"

@interface RTCMediaStreamTrack (Flutter)
@property(nonatomic, strong, nonnull) id settings;
@end

@interface FlutterWebRTCPlugin (RTCMediaStream)

- (void)getUserMedia:(nonnull NSDictionary*)constraints result:(nonnull FlutterResult)result;

- (void)createLocalMediaStream:(nonnull FlutterResult)result;

- (void)getSources:(nonnull FlutterResult)result;

- (void)mediaStreamTrackCaptureFrame:(nonnull RTCMediaStreamTrack*)track
                              toPath:(nonnull NSString*)path
                              result:(nonnull FlutterResult)result;

- (void)selectAudioInput:(nonnull NSString*)deviceId result:(nullable FlutterResult)result;

- (void)selectAudioOutput:(nonnull NSString*)deviceId result:(nullable FlutterResult)result;

/// HoneyCord: Kamera starten und die Bildrate als Ober- UND Untergrenze festlegen
/// (Vorab-Pin, KVO-Format-Waechter, Nach-Start-Pin). Gemeinsamer Pfad fuer
/// getUserMedia (ort @"") und den Kamerawechsel (ort @"wechsel"). fps <= 0 pinnt
/// nichts — die Format-Vorgabe bleibt. `handler` wird nach dem Nach-Start-Pin
/// aufgerufen (auch im Fehlerfall, mit dem Fehler).
- (void)honeycordKameraStarten:(nonnull RTCCameraVideoCapturer*)capturer
                        geraet:(nonnull AVCaptureDevice*)geraet
                        format:(nonnull AVCaptureDeviceFormat*)format
                           fps:(NSInteger)fps
                           ort:(nonnull NSString*)ort
             completionHandler:(nullable void (^)(NSError* _Nullable error))handler;
@end
