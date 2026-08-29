#if TARGET_OS_IPHONE
#import <Flutter/Flutter.h>
#elif TARGET_OS_OSX
#import <FlutterMacOS/FlutterMacOS.h>
#endif

#import "FlutterRTCFrameCapturer.h"

@import CoreImage;
@import CoreVideo;

@implementation FlutterRTCFrameCapturer {
  RTCVideoTrack* _track;
  NSString* _path;
  FlutterResult _result;
  bool _gotFrame;
}

- (instancetype)initWithTrack:(RTCVideoTrack*)track
                       toPath:(NSString*)path
                       result:(FlutterResult)result {
  self = [super init];
  if (self) {
    _gotFrame = false;
    _track = track;
    _path = path;
    _result = result;
    [track addRenderer:self];
  }
  return self;
}

- (void)setSize:(CGSize)size {
}

#if TARGET_OS_OSX
// HoneyCord (#107, 2026-08-29): Auf dem Mac ohne CoreImage — I420 -> BGRA ->
// CGImage direkt aus dem Speicher. Der CIImage/CIContext-Weg unten lieferte fuer
// die Kamera (AVCaptureDALDevice, 1280x720) "Failed to write image data to
// file", ohne zu sagen, welcher Schritt leer blieb. Dieser Weg braucht keinen
// GPU-Kontext auf dem Capture-Thread und meldet jeden Schritt einzeln.
// Rotation gibt es bei Desktop-Kameras nicht; sie wird hier nicht angewandt.
- (NSData*)hcImageDataFromFrame:(RTCVideoFrame*)frame {
  id<RTCI420Buffer> i420 = [frame.buffer toI420];
  if (!i420) {
    NSLog(@"[hc-capture] toI420 lieferte nil");
    return nil;
  }
  int w = i420.width, h = i420.height;
  if (w <= 0 || h <= 0) {
    NSLog(@"[hc-capture] Frame ohne Masse (%dx%d)", w, h);
    return nil;
  }
  size_t stride = (size_t)w * 4;
  NSMutableData* bgra = [NSMutableData dataWithLength:stride * (size_t)h];
  // libyuv-"ARGB" = Speicherfolge B,G,R,A (kleines Endian).
  [RTCYUVHelper I420ToARGB:i420.dataY
                srcStrideY:i420.strideY
                      srcU:i420.dataU
                srcStrideU:i420.strideU
                      srcV:i420.dataV
                srcStrideV:i420.strideV
                   dstARGB:bgra.mutableBytes
             dstStrideARGB:(int)stride
                     width:w
                    height:h];
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
  CGContextRef ctx = CGBitmapContextCreate(bgra.mutableBytes, (size_t)w, (size_t)h, 8, stride, cs,
                                           kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
  CGColorSpaceRelease(cs);
  if (!ctx) {
    NSLog(@"[hc-capture] CGBitmapContextCreate schlug fehl (%dx%d)", w, h);
    return nil;
  }
  CGImageRef cg = CGBitmapContextCreateImage(ctx);
  CGContextRelease(ctx);
  if (!cg) {
    NSLog(@"[hc-capture] CGBitmapContextCreateImage lieferte NULL");
    return nil;
  }
  NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
  CGImageRelease(cg);
  if (!rep) {
    NSLog(@"[hc-capture] NSBitmapImageRep lieferte nil");
    return nil;
  }
  NSDictionary<NSBitmapImageRepPropertyKey, id>* props = @{NSImageCompressionFactor : @1.0f};
  NSData* data = [[_path pathExtension] isEqualToString:@"jpg"]
                     ? [rep representationUsingType:NSBitmapImageFileTypeJPEG properties:props]
                     : [rep representationUsingType:NSBitmapImageFileTypePNG properties:props];
  if (!data)
    NSLog(@"[hc-capture] representationUsingType lieferte nil (%dx%d)", w, h);
  return data;
}
#endif

- (void)renderFrame:(nullable RTCVideoFrame*)frame {
  if (_gotFrame || frame == nil)
    return;
  _gotFrame = true;
#if TARGET_OS_OSX
  {
    NSData* hcData = [self hcImageDataFromFrame:frame];
    NSError* hcErr = nil;
    // Gemessen 29.08.: path_provider nennt auf dem Mac ~/Library/Caches/<bundle>/
    // als Temp-Ordner -- und der existiert nicht, bis ihn jemand anlegt
    // ("The folder captureFrame.png doesn't exist", errno 2). Also anlegen.
    [[NSFileManager defaultManager] createDirectoryAtPath:[_path stringByDeletingLastPathComponent]
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    BOOL hcOk = hcData != nil && [hcData writeToFile:_path options:NSDataWritingAtomic error:&hcErr];
    if (!hcOk)
      NSLog(@"[hc-capture] Schreiben nach %@ fehlgeschlagen: %@", _path, hcErr);
    // Ergebnis und Abmelden auf dem Hauptthread — wir stehen hier auf dem
    // Capture-Thread, und Flutter erwartet Antworten auf dem Plattform-Thread.
    dispatch_async(dispatch_get_main_queue(), ^{
      if (hcOk) {
        self->_result(nil);
      } else {
        self->_result([FlutterError errorWithCode:@"CaptureFrameFailed"
                                          message:@"Failed to write image data to file"
                                          details:hcErr ? hcErr.localizedDescription : nil]);
      }
      [self->_track removeRenderer:self];
      self->_track = nil;
    });
    return;
  }
#endif
  id<RTCVideoFrameBuffer> buffer = frame.buffer;
  CVPixelBufferRef pixelBufferRef;
  bool shouldRelease;
  if (![buffer isKindOfClass:[RTCCVPixelBuffer class]]) {
    pixelBufferRef = [FlutterRTCFrameCapturer convertToCVPixelBuffer:frame];
    shouldRelease = true;
  } else {
    pixelBufferRef = ((RTCCVPixelBuffer*)buffer).pixelBuffer;
    shouldRelease = false;
  }
  CIImage* ciImage = [CIImage imageWithCVPixelBuffer:pixelBufferRef];
  CGRect outputSize;
  if (@available(iOS 11, macOS 10.13, *)) {
    switch (frame.rotation) {
      case RTCVideoRotation_90:
        ciImage = [ciImage imageByApplyingCGOrientation:kCGImagePropertyOrientationRight];
        outputSize = CGRectMake(0, 0, frame.height, frame.width);
        break;
      case RTCVideoRotation_180:
        ciImage = [ciImage imageByApplyingCGOrientation:kCGImagePropertyOrientationDown];
        outputSize = CGRectMake(0, 0, frame.width, frame.height);
        break;
      case RTCVideoRotation_270:
        ciImage = [ciImage imageByApplyingCGOrientation:kCGImagePropertyOrientationLeft];
        outputSize = CGRectMake(0, 0, frame.height, frame.width);
        break;
      default:
        outputSize = CGRectMake(0, 0, frame.width, frame.height);
        break;
    }
  } else {
    outputSize = CGRectMake(0, 0, frame.width, frame.height);
  }
  CIContext* tempContext = [CIContext contextWithOptions:nil];
  CGImageRef cgImage = [tempContext createCGImage:ciImage fromRect:outputSize];
  NSData* imageData;
#if TARGET_OS_IPHONE
  UIImage* uiImage = [UIImage imageWithCGImage:cgImage];
  if ([[_path pathExtension] isEqualToString:@"jpg"]) {
    imageData = UIImageJPEGRepresentation(uiImage, 1.0f);
  } else {
    imageData = UIImagePNGRepresentation(uiImage);
  }
#else
  NSBitmapImageRep* newRep = [[NSBitmapImageRep alloc] initWithCGImage:cgImage];
  [newRep setSize:NSSizeToCGSize(outputSize.size)];
  NSDictionary<NSBitmapImageRepPropertyKey, id>* quality = @{NSImageCompressionFactor : @1.0f};
  if ([[_path pathExtension] isEqualToString:@"jpg"]) {
    imageData = [newRep representationUsingType:NSBitmapImageFileTypeJPEG properties:quality];
  } else {
    imageData = [newRep representationUsingType:NSBitmapImageFileTypePNG properties:quality];
  }
#endif
  CGImageRelease(cgImage);
  if (shouldRelease)
    CVPixelBufferRelease(pixelBufferRef);
  if (imageData && [imageData writeToFile:_path atomically:NO]) {
    NSLog(@"File writed successfully to %@", _path);
    _result(nil);
  } else {
    NSLog(@"Failed to write to file");
    _result([FlutterError errorWithCode:@"CaptureFrameFailed"
                                message:@"Failed to write image data to file"
                                details:nil]);
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    [self->_track removeRenderer:self];
    self->_track = nil;
  });
}

+ (CVPixelBufferRef)convertToCVPixelBuffer:(RTCVideoFrame*)frame {
  id<RTCI420Buffer> i420Buffer = [frame.buffer toI420];
  CVPixelBufferRef outputPixelBuffer;
  size_t w = (size_t)roundf(i420Buffer.width);
  size_t h = (size_t)roundf(i420Buffer.height);
  NSDictionary* pixelAttributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
  CVPixelBufferCreate(kCFAllocatorDefault, w, h, kCVPixelFormatType_32BGRA,
                      (__bridge CFDictionaryRef)(pixelAttributes), &outputPixelBuffer);
  CVPixelBufferLockBaseAddress(outputPixelBuffer, 0);
  const OSType pixelFormat = CVPixelBufferGetPixelFormatType(outputPixelBuffer);
  if (pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
      pixelFormat == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
    // NV12
    uint8_t* dstY = CVPixelBufferGetBaseAddressOfPlane(outputPixelBuffer, 0);
    const size_t dstYStride = CVPixelBufferGetBytesPerRowOfPlane(outputPixelBuffer, 0);
    uint8_t* dstUV = CVPixelBufferGetBaseAddressOfPlane(outputPixelBuffer, 1);
    const size_t dstUVStride = CVPixelBufferGetBytesPerRowOfPlane(outputPixelBuffer, 1);

    [RTCYUVHelper I420ToNV12:i420Buffer.dataY
                  srcStrideY:i420Buffer.strideY
                        srcU:i420Buffer.dataU
                  srcStrideU:i420Buffer.strideU
                        srcV:i420Buffer.dataV
                  srcStrideV:i420Buffer.strideV
                        dstY:dstY
                  dstStrideY:(int)dstYStride
                       dstUV:dstUV
                 dstStrideUV:(int)dstUVStride
                       width:i420Buffer.width
                      height:i420Buffer.height];
  } else {
    uint8_t* dst = CVPixelBufferGetBaseAddress(outputPixelBuffer);
    const size_t bytesPerRow = CVPixelBufferGetBytesPerRow(outputPixelBuffer);

    if (pixelFormat == kCVPixelFormatType_32BGRA) {
      // Corresponds to libyuv::FOURCC_ARGB
      [RTCYUVHelper I420ToARGB:i420Buffer.dataY
                    srcStrideY:i420Buffer.strideY
                          srcU:i420Buffer.dataU
                    srcStrideU:i420Buffer.strideU
                          srcV:i420Buffer.dataV
                    srcStrideV:i420Buffer.strideV
                       dstARGB:dst
                 dstStrideARGB:(int)bytesPerRow
                         width:i420Buffer.width
                        height:i420Buffer.height];
    } else if (pixelFormat == kCVPixelFormatType_32ARGB) {
      // Corresponds to libyuv::FOURCC_BGRA
      [RTCYUVHelper I420ToBGRA:i420Buffer.dataY
                    srcStrideY:i420Buffer.strideY
                          srcU:i420Buffer.dataU
                    srcStrideU:i420Buffer.strideU
                          srcV:i420Buffer.dataV
                    srcStrideV:i420Buffer.strideV
                       dstBGRA:dst
                 dstStrideBGRA:(int)bytesPerRow
                         width:i420Buffer.width
                        height:i420Buffer.height];
    }
  }
  CVPixelBufferUnlockBaseAddress(outputPixelBuffer, 0);
  return outputPixelBuffer;
}

@end
