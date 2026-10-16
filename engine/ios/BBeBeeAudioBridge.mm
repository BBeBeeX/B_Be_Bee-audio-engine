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

// Dynamic function pointers for libmpv symbols on iOS / macOS
typedef mpv_handle* (*mpv_create_fn)();
typedef int (*mpv_initialize_fn)(mpv_handle*);
typedef void (*mpv_destroy_fn)(mpv_handle*);
typedef int (*mpv_command_fn)(mpv_handle*, const char**);
typedef int (*mpv_set_option_string_fn)(mpv_handle*, const char*, const char*);
typedef int (*mpv_set_property_string_fn)(mpv_handle*, const char*, const char*);
typedef const char* (*mpv_error_string_fn)(int);
typedef int (*mpv_set_pcm_callback_fn)(mpv_handle*, mpv_pcm_callback_fn, void*);

static void* s_libmpvHandle = nullptr;
static mpv_create_fn s_mpv_create = nullptr;
static mpv_initialize_fn s_mpv_initialize = nullptr;
static mpv_destroy_fn s_mpv_destroy = nullptr;
static mpv_command_fn s_mpv_command = nullptr;
static mpv_set_option_string_fn s_mpv_set_option_string = nullptr;
static mpv_set_property_string_fn s_mpv_set_property_string = nullptr;
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
    s_mpv_error_string = (mpv_error_string_fn)dlsym(s_libmpvHandle, "mpv_error_string");
    s_mpv_set_pcm_callback = (mpv_set_pcm_callback_fn)dlsym(s_libmpvHandle, "mpv_set_pcm_callback");

    return (s_mpv_create && s_mpv_initialize && s_mpv_destroy && s_mpv_command);
}

@interface BBeBeeAudioBridge () {
    mpv_handle* _mpv;
    std::unique_ptr<audio_engine::PcmRingBuffer> _ringBuffer;
    std::unique_ptr<audio_engine::FftProcessor> _fftProcessor;
    std::vector<float> _pcmReadBuffer;
    std::mutex _mutex;
    std::atomic<bool> _pcmTapRegistered;
    std::atomic<bool> _isPlaying;
}
- (void)onPcmFrames:(const float *)samples frames:(size_t)frames channels:(int)channels sampleRate:(int)sampleRate;
@end

static void iosPcmCallback(const float* data, size_t frames, int channels, int sample_rate, void* user_data) {
    if (user_data) {
        auto* bridge = (__bridge BBeBeeAudioBridge*)user_data;
        [bridge onPcmFrames:data frames:frames channels:channels sampleRate:sample_rate];
    }
}

@implementation BBeBeeAudioBridge

- (instancetype)initWithConfigDirectory:(nullable NSString *)configDir error:(NSError **)error {
    self = [super init];
    if (!self) return nil;

    _mpv = nullptr;
    _pcmTapRegistered.store(false);
    _isPlaying.store(false);
    _ringBuffer = std::make_unique<audio_engine::PcmRingBuffer>();
    _fftProcessor = std::make_unique<audio_engine::FftProcessor>();
    _ringBuffer->configure(44100, 2, 8192);
    _pcmReadBuffer.resize(2048 * 2);

    if (!loadIosMpvSymbols()) {
        NSLog(@"[BBeBeeAudioBridge] Warning: libmpv symbols unavailable. Running in stub mode.");
        return self;
    }

    _mpv = s_mpv_create();
    if (!_mpv) {
        if (error) {
            *error = [NSError errorWithDomain:@"org.bbebee.audio" code:-1
                                     userInfo:@{NSLocalizedDescriptionKey: @"Failed to create mpv instance"}];
        }
        return nil;
    }

    if (configDir && s_mpv_set_option_string) {
        s_mpv_set_option_string(_mpv, "config-dir", [configDir UTF8String]);
    }

    if (s_mpv_set_option_string) {
        s_mpv_set_option_string(_mpv, "video", "no");
        s_mpv_set_option_string(_mpv, "ao", "audiounit,coreaudio");
    }

    int initErr = s_mpv_initialize(_mpv);
    if (initErr < 0) {
        const char* errStr = s_mpv_error_string ? s_mpv_error_string(initErr) : "unknown";
        if (error) {
            *error = [NSError errorWithDomain:@"org.bbebee.audio" code:initErr
                                     userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:errStr]}];
        }
        s_mpv_destroy(_mpv);
        _mpv = nullptr;
        return nil;
    }

    if (s_mpv_set_pcm_callback) {
        int tapErr = s_mpv_set_pcm_callback(_mpv, iosPcmCallback, (__bridge void*)self);
        if (tapErr == 0) {
            _pcmTapRegistered.store(true);
            NSLog(@"[BBeBeeAudioBridge] libmpv PCM tap registered successfully on iOS.");
        }
    }

    return self;
}

- (void)dealloc {
    [self destroy];
}

- (void)destroy {
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
    if (_ringBuffer) {
        _ringBuffer->clear();
    }
}

- (BOOL)loadURI:(NSString *)uri error:(NSError **)error {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_mpv || !s_mpv_command) return NO;

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
    _isPlaying.store(true);
    return YES;
}

- (void)play {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mpv && s_mpv_set_property_string) {
        s_mpv_set_property_string(_mpv, "pause", "no");
        _isPlaying.store(true);
    }
}

- (void)pause {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mpv && s_mpv_set_property_string) {
        s_mpv_set_property_string(_mpv, "pause", "yes");
        _isPlaying.store(false);
    }
}

- (void)stop {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mpv && s_mpv_command) {
        const char* cmd[] = { "stop", nullptr };
        s_mpv_command(_mpv, cmd);
        _isPlaying.store(false);
        if (_ringBuffer) _ringBuffer->clear();
    }
}

- (void)seekToSeconds:(double)seconds {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mpv && s_mpv_command) {
        NSString *secStr = [NSString stringWithFormat:@"%.3f", seconds];
        const char* cmd[] = { "seek", [secStr UTF8String], "absolute", nullptr };
        s_mpv_command(_mpv, cmd);
    }
}

- (void)setVolume:(double)volume {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_mpv && s_mpv_set_property_string) {
        NSString *volStr = [NSString stringWithFormat:@"%.1f", volume];
        s_mpv_set_property_string(_mpv, "volume", [volStr UTF8String]);
    }
}

- (void)onPcmFrames:(const float *)samples frames:(size_t)frames channels:(int)channels sampleRate:(int)sampleRate {
    if (_ringBuffer) {
        _ringBuffer->write(samples, frames, channels, sampleRate);
    }
}

- (BOOL)getFrequencyData:(uint8_t *)outFrequency
          timeDomainData:(nullable uint8_t *)outTimeDomain
                   count:(NSUInteger)count {
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
    return _mpv != nullptr;
}

- (BOOL)isPcmTapAvailable {
    return _pcmTapRegistered.load();
}

- (BOOL)isPlaying {
    return _isPlaying.load();
}

@end
