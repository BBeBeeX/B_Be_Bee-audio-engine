#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/**
 * Statistics snapshot from the internal lock-free ring buffer.
 */
typedef struct {
    uint64_t totalFramesWritten;
    uint64_t totalFramesRead;
    uint64_t droppedFrames;
    uint64_t overflowCount;
    uint64_t underrunCount;
} BBeBeeRingBufferStats;

/**
 * BBeBeeAudioBridge: Objective-C wrapper around the C++ audio engine,
 * libmpv CoreAudio backend, and realtime FFT visualizer for iOS / macOS.
 */
@interface BBeBeeAudioBridge : NSObject

@property (nonatomic, readonly) BOOL isMpvAvailable;
@property (nonatomic, readonly) BOOL isPcmTapAvailable;
@property (nonatomic, readonly) BOOL isPlaying;

/**
 * Initialize audio engine with optional configuration directory path.
 *
 * Init never creates the mpv core: playback touches (loadURI, the
 * persistent setters, play/pause) create it lazily on first use, and
 * read-only queries plus the transcode surface never create it at
 * all. A transcode-only instance therefore costs no playback core.
 * isMpvAvailable and currentState's mpvAvailable report whether the
 * libmpv symbols are linked, not whether a core exists.
 */
- (instancetype)initWithConfigDirectory:(nullable NSString *)configDir error:(NSError * _Nullable * _Nullable)error;

/**
 * Teardown audio engine, unregister PCM callbacks, and release resources.
 */
- (void)destroy;

/**
 * E2: parity additions that used to exist only on Android/desktop.
 * currentState answers status/positionMs/durationMs/uri/mpvAvailable/
 * pcmTapAvailable from the engine instead of forcing the caller to
 * infer state. setOutputDevice/setMuted/setDspConfig mirror the
 * desktop actions of the same names (deviceId "default" maps to
 * mpv's "auto"; a nil/empty af string clears the filter chain).
 *
 * Transcode used to be absent here because the iOS FFmpeg build had
 * no encoders; it now has an iOS FFmpeg encoder SDK (see
 * scripts/mpv-build/build-ios.sh) and the transcode methods below,
 * which share the Android/desktop transcoder and contract.
 */
- (NSDictionary *)currentState;
- (void)setOutputDevice:(nullable NSString *)deviceId;
- (void)setMuted:(BOOL)muted;
- (void)setDspConfig:(nullable NSString *)afString;

/**
 * Load audio track or remote stream URI.
 */
- (BOOL)loadURI:(NSString *)uri error:(NSError * _Nullable * _Nullable)error;

/**
 * Playback transport controls.
 */
- (void)play;
- (void)pause;
- (void)stop;
- (void)seekToSeconds:(double)seconds;
- (void)setVolume:(double)volume;

/**
 * Retrieve realtime frequency spectrum and time-domain waveform data.
 * @param outFrequency Pointer to pre-allocated byte buffer for FFT magnitudes [0..255]
 * @param outTimeDomain Pointer to pre-allocated byte buffer for waveform amplitudes [0..255]
 * @param count Number of elements to write (e.g., 64 frequency bins, 128 time samples)
 * @return YES if spectrum was successfully calculated, NO on error.
 */
- (BOOL)getFrequencyData:(uint8_t *)outFrequency
          timeDomainData:(nullable uint8_t *)outTimeDomain
                   count:(NSUInteger)count;

/**
 * Retrieve ring buffer performance and overflow metrics.
 */
- (BBeBeeRingBufferStats)ringBufferStats;

/**
 * iOS transcode, mirroring the Android JNI contract (not a new one):
 * a live jobId or a live output path is refused, progress is throttled
 * by the transcoder (2% steps or 0.5 s), cancellation is cooperative
 * (the worker exits at the next cancel checkpoint and the temp file
 * is deleted, so a cancelled job never leaves a partial output).
 * Runs on a background queue; progress and completion blocks are
 * called on the main queue. Returns the jobId, or nil if the request
 * was refused (completion is still called with ok=NO in that case).
 * Input/output paths must be plain filesystem paths inside the app
 * container; security-scoped URLs must be held open by the caller for
 * the whole job. Transcode does not use mpv — it runs the shared
 * libav transcoder, so it works even when mpv is unavailable.
 */
- (NSDictionary *)transcodeCapabilities;
- (nullable NSString *)transcodeWithInput:(NSString *)inputPath
                                  output:(NSString *)outputPath
                                  format:(NSString *)format
                                 bitrate:(nullable NSString *)bitrate
                              sampleRate:(int)sampleRate
                                channels:(int)channels
                               startTime:(double)startTime
                                 endTime:(double)endTime
                                progress:(nullable void (^)(double progress, double currentTime, double duration))progressBlock
                              completion:(nullable void (^)(BOOL ok, BOOL cancelled, NSString * _Nullable error))completionBlock;
- (void)cancelTranscode:(NSString *)jobId;

@end

NS_ASSUME_NONNULL_END
