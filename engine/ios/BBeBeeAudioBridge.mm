#import "BBeBeeAudioBridge.h"

#include <memory>
#include <mutex>
#include <vector>
#include <atomic>
#include <dlfcn.h>
#include <algorithm>
#include <cstring>

#include "../mpv_client.h"
#include "../pcm_ring_buffer.hpp"
#include "../fft.hpp"
#include "../transcode.hpp"
#include "../jni/transcode_caps.hpp"
#include <map>
#include <string>

// Dynamic function pointers for libmpv symbols on iOS / macOS
typedef mpv_handle* (*mpv_create_fn)();
typedef int (*mpv_initialize_fn)(mpv_handle*);
typedef void (*mpv_destroy_fn)(mpv_handle*);
typedef int (*mpv_command_fn)(mpv_handle*, const char**);
typedef int (*mpv_set_option_string_fn)(mpv_handle*, const char*, const char*);
typedef int (*mpv_set_property_string_fn)(mpv_handle*, const char*, const char*);
typedef int (*mpv_get_property_fn)(mpv_handle*, const char*, mpv_format, void*);
typedef void (*mpv_free_fn)(void*);
typedef const char* (*mpv_error_string_fn)(int);
typedef void (*mpv_set_pcm_callback_fn)(mpv_handle*, mpv_pcm_callback_fn, void*);

static void* s_libmpvHandle = nullptr;
static mpv_create_fn s_mpv_create = nullptr;
static mpv_initialize_fn s_mpv_initialize = nullptr;
static mpv_destroy_fn s_mpv_destroy = nullptr;
static mpv_command_fn s_mpv_command = nullptr;
static mpv_set_option_string_fn s_mpv_set_option_string = nullptr;
static mpv_set_property_string_fn s_mpv_set_property_string = nullptr;
static mpv_get_property_fn s_mpv_get_property = nullptr;
static mpv_free_fn s_mpv_free = nullptr;
static mpv_error_string_fn s_mpv_error_string = nullptr;
static mpv_set_pcm_callback_fn s_mpv_set_pcm_callback = nullptr;

static bool loadIosMpvSymbols() {
    if (s_libmpvHandle) return true;

    // Check framework / dylib paths on iOS
    s_libmpvHandle = dlopen("libmpv.2.dylib", RTLD_NOW);
    if (!s_libmpvHandle) {
        s_libmpvHandle = dlopen("@rpath/mpv.framework/mpv", RTLD_NOW);
    }
    if (!s_libmpvHandle) {
        // Attempt fallback to loaded executable symbols
        s_libmpvHandle = dlopen(nullptr, RTLD_NOW);
    }

    if (!s_libmpvHandle) return false;

    s_mpv_create = (mpv_create_fn)dlsym(s_libmpvHandle, "mpv_create");
    s_mpv_initialize = (mpv_initialize_fn)dlsym(s_libmpvHandle, "mpv_initialize");
    s_mpv_destroy = (mpv_destroy_fn)dlsym(s_libmpvHandle, "mpv_destroy");
    s_mpv_command = (mpv_command_fn)dlsym(s_libmpvHandle, "mpv_command");
    s_mpv_set_option_string = (mpv_set_option_string_fn)dlsym(s_libmpvHandle, "mpv_set_option_string");
    s_mpv_set_property_string = (mpv_set_property_string_fn)dlsym(s_libmpvHandle, "mpv_set_property_string");
    s_mpv_get_property = (mpv_get_property_fn)dlsym(s_libmpvHandle, "mpv_get_property");
    s_mpv_free = (mpv_free_fn)dlsym(s_libmpvHandle, "mpv_free");
    s_mpv_error_string = (mpv_error_string_fn)dlsym(s_libmpvHandle, "mpv_error_string");
    s_mpv_set_pcm_callback = (mpv_set_pcm_callback_fn)dlsym(s_libmpvHandle, "mpv_set_pcm_callback");

    return (s_mpv_create && s_mpv_initialize && s_mpv_destroy && s_mpv_command);
}

@interface BBeBeeAudioBridge () {
    mpv_handle* _mpv;
    NSString* _configDir;
    std::unique_ptr<audio_engine::PcmRingBuffer> _ringBuffer;
    std::unique_ptr<audio_engine::FftProcessor> _fftProcessor;
    std::vector<float> _pcmReadBuffer;
    std::mutex _mutex;
    std::atomic<bool> _pcmTapRegistered;
    std::atomic<bool> _isPlaying;
    NSString* _currentURI;
    // Transcode job registry, mirroring the JNI/desktop contract. A
    // separate mutex from _mutex: progress/completion callbacks must
    // never be invoked while holding the lock that cancelTranscode
    // takes, or a callback that cancels its own job would deadlock.
    std::mutex _transcodeMutex;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> _transcodeJobs;
    std::map<std::string, std::string> _transcodeOutputs;
    dispatch_queue_t _transcodeQueue;
}
- (void)onPcmFrames:(const float *)samples frames:(int)frames channels:(int)channels sampleRate:(int)sampleRate;
// Create the mpv core on first playback touch. Must be called with
// _mutex held. Read-only queries (currentState, isMpvAvailable) and
// the transcode surface deliberately do NOT call it, so a bridge
// instance used only for transcode never spins up a playback core.
- (BOOL)ensureMpvWithError:(NSError **)error;
@end

static void iosPcmCallback(const float* data, int frames, int channels, int sample_rate, void* user_data) {
    if (user_data) {
        auto* bridge = (__bridge BBeBeeAudioBridge*)user_data;
        [bridge onPcmFrames:data frames:frames channels:channels sampleRate:sample_rate];
    }
}

@implementation BBeBeeAudioBridge

- (instancetype)initWithConfigDirectory:(nullable NSString *)configDir error:(NSError **)error {
    self = [super init];
    if (!self) return nil;

    // Kept for source compatibility: init never fails anymore — mpv
    // creation moved to ensureMpvWithError, so a transcode-only
    // instance needs no playback core and reports no init error.
    (void)error;
    _mpv = nullptr;
    _configDir = configDir;
    _pcmTapRegistered.store(false);
    _isPlaying.store(false);
    _transcodeQueue = dispatch_queue_create("org.bbebee.audio.transcode", DISPATCH_QUEUE_CONCURRENT);
    _ringBuffer = std::make_unique<audio_engine::PcmRingBuffer>();
    _fftProcessor = std::make_unique<audio_engine::FftProcessor>();
    _ringBuffer->configure(44100, 2, 8192);
    _pcmReadBuffer.resize(2048 * 2);

    // Symbol resolution only — dlsym is cheap and creates nothing.
    // The mpv core itself is created lazily by ensureMpvWithError on
    // the first playback touch, so a transcode-only instance stays
    // free of an idle playback core.
    if (!loadIosMpvSymbols()) {
        NSLog(@"[BBeBeeAudioBridge] Warning: libmpv symbols unavailable. Running in stub mode.");
    }

    return self;
}

- (BOOL)ensureMpvWithError:(NSError **)error {
    if (_mpv) return YES;
    if (!loadIosMpvSymbols()) {
        if (error) {
            *error = [NSError errorWithDomain:@"org.bbebee.audio" code:-1
                                     userInfo:@{NSLocalizedDescriptionKey: @"libmpv symbols unavailable"}];
        }
        return NO;
    }

    mpv_handle* mpv = s_mpv_create();
    if (!mpv) {
        if (error) {
            *error = [NSError errorWithDomain:@"org.bbebee.audio" code:-1
                                     userInfo:@{NSLocalizedDescriptionKey: @"Failed to create mpv instance"}];
        }
        return NO;
    }

    if (_configDir && s_mpv_set_option_string) {
        s_mpv_set_option_string(mpv, "config-dir", [_configDir UTF8String]);
    }

    if (s_mpv_set_option_string) {
        s_mpv_set_option_string(mpv, "video", "no");
        s_mpv_set_option_string(mpv, "ao", "audiounit,coreaudio");
    }

    int initErr = s_mpv_initialize(mpv);
    if (initErr < 0) {
        const char* errStr = s_mpv_error_string ? s_mpv_error_string(initErr) : "unknown";
        if (error) {
            *error = [NSError errorWithDomain:@"org.bbebee.audio" code:initErr
                                     userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:errStr]}];
        }
        s_mpv_destroy(mpv);
        return NO;
    }

    if (s_mpv_set_pcm_callback) {
        s_mpv_set_pcm_callback(mpv, iosPcmCallback, (__bridge void*)self);
        _pcmTapRegistered.store(true);
        NSLog(@"[BBeBeeAudioBridge] libmpv PCM tap registered successfully on iOS.");
    }

    _mpv = mpv;
    return YES;
}

- (void)dealloc {
    [self destroy];
}

- (void)destroy {
    // Cancel any running transcode jobs first, mirroring the desktop
    // shutdown path: workers observe the flag at the next checkpoint,
    // delete their temp file, and report cancelled. The worker blocks
    // hold self via ivar access until they finish, so destroy cannot
    // free the registry out from under a running job.
    {
        std::lock_guard<std::mutex> tLock(_transcodeMutex);
        for (auto& kv : _transcodeJobs) kv.second->store(true);
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mpv) {
        if (s_mpv_set_pcm_callback && _pcmTapRegistered.load()) {
            s_mpv_set_pcm_callback(_mpv, nullptr, nullptr);
            _pcmTapRegistered.store(false);
        }
        if (s_mpv_destroy) {
            s_mpv_destroy(_mpv);
        }
        _mpv = nullptr;
    }
    _isPlaying.store(false);
    _currentURI = nil;
    if (_ringBuffer) {
        _ringBuffer->clear();
    }
}

// E2: gapless re-bind, mirroring desktop/JNI. When the player re-loads
// the file that is already sounding (playlist auto-advance followed by
// a load+play of the same file), reloading would seek it back to zero
// mid-playback. A same-URI load while playing is therefore a no-op
// that reports success. Comparison strips a file:// prefix and
// percent-decodes, so `file:///a%20b` matches `/a b`. mpv itself
// handles file:// percent-decoding on load, so the URI passed to
// loadfile is left untouched.
static NSString* iosNormalizedURI(NSString* uri) {
    NSString* s = uri ?: @"";
    if ([s hasPrefix:@"file://"]) {
        s = [s substringFromIndex:7];
        if ([s hasPrefix:@"localhost/"]) s = [@"/" stringByAppendingString:[s substringFromIndex:10]];
    }
    NSString* decoded = [s stringByRemovingPercentEncoding];
    return decoded ?: s;
}

- (BOOL)loadURI:(NSString *)uri error:(NSError **)error {
    std::lock_guard<std::mutex> lock(_mutex);
    if (![self ensureMpvWithError:error]) return NO;

    if (_isPlaying.load() && _currentURI &&
        [iosNormalizedURI(_currentURI) isEqualToString:iosNormalizedURI(uri)]) {
        return YES;
    }

    const char* cmd[] = { "loadfile", [uri UTF8String], "replace", nullptr };
    int err = s_mpv_command(_mpv, cmd);
    if (err < 0) {
        const char* errStr = s_mpv_error_string ? s_mpv_error_string(err) : "unknown";
        if (error) {
            *error = [NSError errorWithDomain:@"org.bbebee.audio" code:err
                                     userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:errStr]}];
        }
        return NO;
    }
    _currentURI = uri;
    _isPlaying.store(true);
    return YES;
}

- (NSDictionary *)currentState {
    std::lock_guard<std::mutex> lock(_mutex);
    double pos = 0.0, dur = 0.0;
    int paused = 1;
    if (_mpv && s_mpv_get_property) {
        s_mpv_get_property(_mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
        s_mpv_get_property(_mpv, "duration", MPV_FORMAT_DOUBLE, &dur);
        s_mpv_get_property(_mpv, "pause", MPV_FORMAT_FLAG, &paused);
    }
    NSString* status = (!_mpv || !_currentURI) ? @"idle" : (paused ? @"paused" : @"playing");
    return @{
        @"status": status,
        @"positionMs": @(pos * 1000.0),
        @"durationMs": @(dur * 1000.0),
        @"uri": _currentURI ?: @"",
        // mpvAvailable means the symbols are linked, not that a core was
        // created: with lazy init, a fresh instance that never touched
        // playback still has mpv available, and this query must not be
        // the thing that creates the core.
        @"mpvAvailable": @((s_mpv_create && s_mpv_initialize && s_mpv_destroy && s_mpv_command) ? YES : NO),
        @"pcmTapAvailable": @(s_mpv_set_pcm_callback != nullptr),
    };
}

- (void)setOutputDevice:(NSString *)deviceId {
    std::lock_guard<std::mutex> lock(_mutex);
    // A setting applied before the first load must survive to it, so
    // setters are playback touches too: they create the core rather
    // than silently dropping the value the way a nil-mpv guard would.
    [self ensureMpvWithError:nil];
    if (_mpv && s_mpv_set_property_string) {
        NSString* dev = deviceId;
        if (!dev || dev.length == 0 || [dev isEqualToString:@"default"]) dev = @"auto";
        s_mpv_set_property_string(_mpv, "audio-device", [dev UTF8String]);
    }
}

- (void)setMuted:(BOOL)muted {
    std::lock_guard<std::mutex> lock(_mutex);
    [self ensureMpvWithError:nil];
    if (_mpv && s_mpv_set_property_string) {
        s_mpv_set_property_string(_mpv, "mute", muted ? "yes" : "no");
    }
}

- (void)setDspConfig:(NSString *)afString {
    std::lock_guard<std::mutex> lock(_mutex);
    [self ensureMpvWithError:nil];
    if (_mpv && s_mpv_set_property_string) {
        s_mpv_set_property_string(_mpv, "af", afString ? [afString UTF8String] : "");
    }
}

- (NSDictionary *)transcodeCapabilities {
    bbebee::jni::TranscodeCaps caps = bbebee::jni::probeTranscodeCaps();
    NSMutableDictionary* encoders = [NSMutableDictionary dictionary];
    for (const auto& kv : caps.encoders) {
        encoders[[NSString stringWithUTF8String:kv.first.c_str()]] = @(kv.second);
    }
    return @{
        @"supported": @(caps.supported),
        @"encoders": encoders,
    };
}

- (NSString *)transcodeWithInput:(NSString *)inputPath
                          output:(NSString *)outputPath
                          format:(NSString *)format
                         bitrate:(NSString *)bitrate
                      sampleRate:(int)sampleRate
                        channels:(int)channels
                       startTime:(double)startTime
                         endTime:(double)endTime
                        progress:(void (^)(double, double, double))progressBlock
                      completion:(void (^ _Nullable)(BOOL, BOOL, NSString * _Nullable))completionBlock {
    if (!inputPath.length || !outputPath.length || !format.length) {
        if (completionBlock) {
            dispatch_async(dispatch_get_main_queue(), ^{
                completionBlock(NO, NO, @"input, output and format are required");
            });
        }
        return nil;
    }

    audio_engine::TranscodeOptions opts;
    opts.jobId = "tc-" + std::string([[[NSUUID UUID] UUIDString] UTF8String]);
    opts.inputPath = [inputPath UTF8String];
    opts.outputPath = [outputPath UTF8String];
    opts.format = [format UTF8String];
    opts.bitrate = bitrate ? [bitrate UTF8String] : "";
    opts.sampleRate = sampleRate;
    opts.channels = channels;
    opts.startTimeSec = startTime;
    opts.endTimeSec = endTime;

    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    const std::string outputKey = audio_engine::normalizePathForComparison(opts.outputPath);
    {
        std::lock_guard<std::mutex> lock(_transcodeMutex);
        auto outIt = _transcodeOutputs.find(outputKey);
        if (outIt != _transcodeOutputs.end()) {
            if (completionBlock) {
                NSString* msg = [NSString stringWithFormat:@"output '%@' is already being written by job '%s'",
                                 outputPath, outIt->second.c_str()];
                dispatch_async(dispatch_get_main_queue(), ^{
                    completionBlock(NO, NO, msg);
                });
            }
            return nil;
        }
        _transcodeJobs[opts.jobId] = cancelFlag;
        _transcodeOutputs[outputKey] = opts.jobId;
    }

    NSString* jobIdObj = [NSString stringWithUTF8String:opts.jobId.c_str()];
    void (^progressCopy)(double, double, double) = [progressBlock copy];
    void (^completionCopy)(BOOL, BOOL, NSString *) = [completionBlock copy];
    dispatch_async(_transcodeQueue, ^{
        audio_engine::Transcoder transcoder;
        audio_engine::TranscodeResult r = transcoder.transcode(
            opts,
            [progressCopy](double p, double currentSec, double durationSec) {
                if (!progressCopy) return;
                dispatch_async(dispatch_get_main_queue(), ^{
                    progressCopy(p, currentSec, durationSec);
                });
            },
            cancelFlag.get());

        {
            std::lock_guard<std::mutex> lock(_transcodeMutex);
            auto it = _transcodeJobs.find(opts.jobId);
            if (it != _transcodeJobs.end() && it->second == cancelFlag)
                _transcodeJobs.erase(it);
            auto outIt = _transcodeOutputs.find(outputKey);
            if (outIt != _transcodeOutputs.end() && outIt->second == opts.jobId)
                _transcodeOutputs.erase(outIt);
        }

        if (completionCopy) {
            NSString* err = r.ok ? nil : [NSString stringWithUTF8String:r.error.c_str()];
            dispatch_async(dispatch_get_main_queue(), ^{
                completionCopy(r.ok, r.cancelled, err);
            });
        }
    });
    return jobIdObj;
}

- (void)cancelTranscode:(NSString *)jobId {
    if (!jobId.length) return;
    std::lock_guard<std::mutex> lock(_transcodeMutex);
    auto it = _transcodeJobs.find([jobId UTF8String]);
    if (it != _transcodeJobs.end()) it->second->store(true);
}

- (void)play {
    std::lock_guard<std::mutex> lock(_mutex);
    [self ensureMpvWithError:nil];
    if (_mpv && s_mpv_set_property_string) {
        s_mpv_set_property_string(_mpv, "pause", "no");
        _isPlaying.store(true);
    }
}

- (void)pause {
    std::lock_guard<std::mutex> lock(_mutex);
    [self ensureMpvWithError:nil];
    if (_mpv && s_mpv_set_property_string) {
        s_mpv_set_property_string(_mpv, "pause", "yes");
        _isPlaying.store(false);
    }
}

- (void)stop {
    std::lock_guard<std::mutex> lock(_mutex);
    // Stop on a never-played instance must stay a no-op, not create a
    // core just to stop it — unlike the setters, it has no value to
    // preserve, so it deliberately does not ensure.
    if (_mpv && s_mpv_command) {
        const char* cmd[] = { "stop", nullptr };
        s_mpv_command(_mpv, cmd);
        _isPlaying.store(false);
        _currentURI = nil;
        if (_ringBuffer) _ringBuffer->clear();
    }
}

- (void)seekToSeconds:(double)seconds {
    std::lock_guard<std::mutex> lock(_mutex);
    // Seek has no persistent value (unlike volume/mute/DSP), so like
    // stop it stays a no-op on a never-played instance.
    if (_mpv && s_mpv_command) {
        NSString *secStr = [NSString stringWithFormat:@"%.3f", seconds];
        const char* cmd[] = { "seek", [secStr UTF8String], "absolute", nullptr };
        s_mpv_command(_mpv, cmd);
    }
}

- (void)setVolume:(double)volume {
    std::lock_guard<std::mutex> lock(_mutex);
    // Volume is a persistent setting like mute/DSP: applied before the
    // first load, it must survive to it, so this ensures the core.
    [self ensureMpvWithError:nil];
    if (_mpv && s_mpv_set_property_string) {
        NSString *volStr = [NSString stringWithFormat:@"%.1f", volume];
        s_mpv_set_property_string(_mpv, "volume", [volStr UTF8String]);
    }
}

- (void)onPcmFrames:(const float *)samples frames:(int)frames channels:(int)channels sampleRate:(int)sampleRate {
    if (_ringBuffer && frames > 0) {
        _ringBuffer->write(samples, static_cast<size_t>(frames), channels, sampleRate);
    }
}

- (BOOL)getFrequencyData:(uint8_t *)outFrequency
          timeDomainData:(nullable uint8_t *)outTimeDomain
                   count:(NSUInteger)count {
    // E1: every other method that touches _ringBuffer/_fftProcessor holds
    // _mutex; this one did not, so a call racing destroy() (or another
    // getFrequencyData) could resize _pcmReadBuffer or drive the
    // non-thread-safe FftProcessor while it was being torn down. The
    // JNI getSpectrum already locks for exactly this reason.
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_ringBuffer || !_fftProcessor || !outFrequency) return NO;

    size_t reqFrames = _fftProcessor->getFftSize();
    int channels = _ringBuffer->getChannels();
    if (channels <= 0) channels = 2;

    if (_pcmReadBuffer.size() < reqFrames * channels) {
        _pcmReadBuffer.resize(reqFrames * channels);
    }

    size_t readFrames = _ringBuffer->read(_pcmReadBuffer.data(), reqFrames);
    _fftProcessor->processInterleaved(_pcmReadBuffer.data(), readFrames, channels);

    const auto& freq = _fftProcessor->getFrequencyData();
    const auto& time = _fftProcessor->getTimeDomainData();

    size_t copyBins = std::min((size_t)count, freq.size());
    std::memcpy(outFrequency, freq.data(), copyBins);

    if (outTimeDomain) {
        size_t copyTime = std::min((size_t)count, time.size());
        std::memcpy(outTimeDomain, time.data(), copyTime);
    }

    return YES;
}

- (BBeBeeRingBufferStats)ringBufferStats {
    BBeBeeRingBufferStats stats = { 0, 0, 0, 0, 0 };
    if (_ringBuffer) {
        auto s = _ringBuffer->getStats();
        stats.totalFramesWritten = s.totalFramesWritten;
        stats.totalFramesRead = s.totalFramesRead;
        stats.droppedFrames = s.droppedFrames;
        stats.overflowCount = s.overflowCount;
        stats.underrunCount = s.underrunCount;
    }
    return stats;
}

- (BOOL)isMpvAvailable {
    // Symbols, not a created core — see currentState's mpvAvailable.
    return (s_mpv_create && s_mpv_initialize && s_mpv_destroy && s_mpv_command) ? YES : NO;
}

- (BOOL)isPcmTapAvailable {
    return _pcmTapRegistered.load();
}

- (BOOL)isPlaying {
    return _isPlaying.load();
}

@end
