#if TARGET_OS_IPHONE
#import <Flutter/Flutter.h>
#elif TARGET_OS_OSX
#import <FlutterMacOS/FlutterMacOS.h>
#endif
#import <Foundation/Foundation.h>
#import <WebRTC/WebRTC.h>

#import "FlutterWebRTCPlugin.h"

@interface FlutterWebRTCPlugin (DesktopCapturer)

- (void)getDisplayMedia:(nonnull NSDictionary*)constraints result:(nonnull FlutterResult)result;

- (void)getDesktopSources:(nonnull NSDictionary*)argsMap result:(nonnull FlutterResult)result;

- (void)updateDesktopSources:(nonnull NSDictionary*)argsMap result:(nonnull FlutterResult)result;

- (void)getDesktopSourceThumbnail:(nonnull NSDictionary*)argsMap
                           result:(nonnull FlutterResult)result;

- (void)prewarmDesktopCapture:(nonnull FlutterResult)result;
/// HoneyCord (Block 2, 05.09.2026): Ton eines AUFNAHMEGERAETS (USB-Tonseite
/// einer Capture-Karte) als eigener Stream mit einer Audiospur — macOS.
- (void)honeycordCaptureAudioStart:(nonnull NSString*)deviceId result:(nonnull FlutterResult)result;
/// Aufnehmer eines solchen Streams stoppen (Stream- oder Track-Id). Idempotent.
- (void)honeycordCaptureAudioStopFuer:(nonnull NSString*)streamOderTrackId;
/// OBS-Weg: Ton der Karte in die VIDEO-Session des Capturers legen (vor dem Start).
- (BOOL)honeycordTonInSession:(nonnull AVCaptureSession*)session geraetId:(nonnull NSString*)deviceId fuerVideoTrack:(nonnull NSString*)videoTrackId;

@end