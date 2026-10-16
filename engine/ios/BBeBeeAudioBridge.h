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
 */
- (instancetype)initWithConfigDirectory:(nullable NSString *)configDir error:(NSError * _Nullable * _Nullable)error;

/**
 * Teardown audio engine, unregister PCM callbacks, and release resources.
 */
- (void)destroy;

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

@end

NS_ASSUME_NONNULL_END
