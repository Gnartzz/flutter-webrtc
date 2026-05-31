/*
 *  HoneyCord — RTCCustomAudioSource (ObjC-Wrapper)
 *  -----------------------------------------------
 *  Subklasse von RTCAudioSource, deren Audio-Daten von außen per `pushData`
 *  hereingegeben werden. Damit lassen sich z. B. System-Audio-Streams (SCK auf
 *  macOS, WASAPI-Loopback auf Windows) als eigene LiveKit-Audio-Tracks
 *  publizieren — getrennt vom Mikrofon, separat mute-/regelbar.
 *
 *  Verwendung:
 *      RTCCustomAudioSource *src = [[RTCCustomAudioSource alloc]
 *          initWithFactory:factory];
 *      RTCAudioTrack *track = [factory audioTrackWithSource:src trackId:@"sc"];
 *      // Pro Capture-Chunk:
 *      [src pushData:int16Buffer
 *      bitsPerSample:16
 *         sampleRate:48000
 *           channels:2
 *             frames:480];
 *      // Beim Beenden:
 *      [src stop];
 *
 *  PCM-Format: signed 16-bit (bitsPerSample = 16), interleaved bei Stereo.
 *  Andere Formate müssen vom Aufrufer vorher in s16 konvertiert werden.
 */
#import <Foundation/Foundation.h>

#import <WebRTC/RTCAudioSource.h>
#import <WebRTC/RTCMacros.h>

@class RTC_OBJC_TYPE(RTCPeerConnectionFactory);

NS_ASSUME_NONNULL_BEGIN

RTC_OBJC_EXPORT
@interface RTC_OBJC_TYPE (RTCCustomAudioSource) : RTC_OBJC_TYPE(RTCAudioSource)

- (instancetype)init NS_UNAVAILABLE;

/// Erzeugt einen neuen Custom-Audio-Source. `factory` wird (analog zu
/// audioSourceWithConstraints) für Owner-Tracking durchgereicht.
- (instancetype)initWithFactory:
    (RTC_OBJC_TYPE(RTCPeerConnectionFactory) *)factory;

/// PCM-Samples in den Source einspeisen. Daten werden synchron an alle
/// abgehängten Sinks (Audio-Tracks) verteilt. Threadsafe.
- (void)pushData:(const void *)audioData
    bitsPerSample:(int)bitsPerSample
       sampleRate:(int)sampleRate
         channels:(NSUInteger)channels
           frames:(NSUInteger)frames;

/// Source beenden — keine weiteren PCM-Pushes werden weitergeleitet.
- (void)stop;

@end

NS_ASSUME_NONNULL_END
