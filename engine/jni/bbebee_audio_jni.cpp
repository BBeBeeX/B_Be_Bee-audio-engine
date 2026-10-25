#include "bbebee_audio_jni.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <cmath>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

#include "../json.hpp"

#define LOG_TAG "BBeBeeAudioJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace bbebee {
namespace jni {

static AudioEngineContext* g_engineContext = nullptr;
static std::mutex g_contextMutex;

// Function pointer signatures for libmpv symbols
typedef mpv_handle* (*mpv_create_fn)();
typedef int (*mpv_initialize_fn)(mpv_handle*);
typedef void (*mpv_destroy_fn)(mpv_handle*);
typedef int (*mpv_command_fn)(mpv_handle*, const char**);
typedef int (*mpv_set_option_string_fn)(mpv_handle*, const char*, const char*);
typedef int (*mpv_set_property_string_fn)(mpv_handle*, const char*, const char*);
typedef const char* (*mpv_error_string_fn)(int);
typedef void (*mpv_set_pcm_callback_fn)(mpv_handle*, mpv_pcm_callback_fn, void*);
typedef char* (*mpv_get_property_string_fn)(mpv_handle*, const char*);
typedef void (*mpv_free_fn)(void*);

static void* g_libmpvHandle = nullptr;
static mpv_create_fn g_mpv_create = nullptr;
static mpv_initialize_fn g_mpv_initialize = nullptr;
static mpv_destroy_fn g_mpv_destroy = nullptr;
static mpv_command_fn g_mpv_command = nullptr;
static mpv_set_option_string_fn g_mpv_set_option_string = nullptr;
static mpv_set_property_string_fn g_mpv_set_property_string = nullptr;
static mpv_error_string_fn g_mpv_error_string = nullptr;
static mpv_set_pcm_callback_fn g_mpv_set_pcm_callback = nullptr;
static mpv_get_property_string_fn g_mpv_get_property_string = nullptr;
static mpv_free_fn g_mpv_free = nullptr;

static bool loadLibmpvSymbols() {
    if (g_libmpvHandle) return true;

    // Try standard loaded library or search in Android library loader path
    g_libmpvHandle = dlopen("libmpv.so", RTLD_NOW);
    if (!g_libmpvHandle) {
        LOGW("dlopen(libmpv.so) failed: %s. Audio engine will run in stub mode.", dlerror());
        return false;
    }

    g_mpv_create = (mpv_create_fn)dlsym(g_libmpvHandle, "mpv_create");
    g_mpv_initialize = (mpv_initialize_fn)dlsym(g_libmpvHandle, "mpv_initialize");
    g_mpv_destroy = (mpv_destroy_fn)dlsym(g_libmpvHandle, "mpv_destroy");
    g_mpv_command = (mpv_command_fn)dlsym(g_libmpvHandle, "mpv_command");
    g_mpv_set_option_string = (mpv_set_option_string_fn)dlsym(g_libmpvHandle, "mpv_set_option_string");
    g_mpv_set_property_string = (mpv_set_property_string_fn)dlsym(g_libmpvHandle, "mpv_set_property_string");
    g_mpv_error_string = (mpv_error_string_fn)dlsym(g_libmpvHandle, "mpv_error_string");
    g_mpv_set_pcm_callback = (mpv_set_pcm_callback_fn)dlsym(g_libmpvHandle, "mpv_set_pcm_callback");
    g_mpv_get_property_string = (mpv_get_property_string_fn)dlsym(g_libmpvHandle, "mpv_get_property_string");
    g_mpv_free = (mpv_free_fn)dlsym(g_libmpvHandle, "mpv_free");

    if (!g_mpv_create || !g_mpv_initialize || !g_mpv_destroy || !g_mpv_command) {
        LOGE("Failed to resolve mandatory libmpv core symbols");
        dlclose(g_libmpvHandle);
        g_libmpvHandle = nullptr;
        return false;
    }

    LOGI("libmpv symbols resolved successfully. PCM tap support: %s",
         (g_mpv_set_pcm_callback != nullptr) ? "AVAILABLE" : "UNAVAILABLE");
    return true;
}

static void pcmCallbackTrampoline(const float* data, int frames, int channels, int sample_rate, void* user_data) {
    if (user_data) {
        auto* ctx = reinterpret_cast<AudioEngineContext*>(user_data);
        ctx->onPcmChunk(data, frames, channels, sample_rate);
    }
}

/** `file:///a/b` and `/a/b` name the same file as far as callers care. */
static std::string stripFileScheme(const std::string& uri) {
    const std::string prefix = "file://";
    if (uri.rfind(prefix, 0) == 0) return uri.substr(prefix.size());
    return uri;
}

AudioEngineContext::AudioEngineContext()
    : ringBuffer_(std::make_unique<audio_engine::PcmRingBuffer>()),
      fftProcessor_(std::make_unique<audio_engine::FftProcessor>()) {
    ringBuffer_->configure(44100, 2, 8192);
    readPcmBuffer_.resize(2048 * 2);
}

AudioEngineContext::~AudioEngineContext() {
    destroy();
}

bool AudioEngineContext::initialize(const std::string& configDir) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_) return true;

    if (!loadLibmpvSymbols()) {
        LOGW("libmpv could not be loaded; continuing in mock/fallback mode.");
        return true;
    }

    mpvHandle_ = g_mpv_create();
    if (!mpvHandle_) {
        LOGE("mpv_create failed");
        return false;
    }

    if (!configDir.empty() && g_mpv_set_option_string) {
        g_mpv_set_option_string(mpvHandle_, "config-dir", configDir.c_str());
    }

    // Configure low latency audio-only playback on Android
    if (g_mpv_set_option_string) {
        g_mpv_set_option_string(mpvHandle_, "video", "no");
        g_mpv_set_option_string(mpvHandle_, "audio-client-name", "BBeBeeAudio");
        g_mpv_set_option_string(mpvHandle_, "ao", "opensles,audiotrack");
    }

    int err = g_mpv_initialize(mpvHandle_);
    if (err < 0) {
        LOGE("mpv_initialize error: %s", g_mpv_error_string ? g_mpv_error_string(err) : "unknown");
        g_mpv_destroy(mpvHandle_);
        mpvHandle_ = nullptr;
        return false;
    }

    if (g_mpv_set_pcm_callback) {
        g_mpv_set_pcm_callback(mpvHandle_, pcmCallbackTrampoline, this);
        pcmTapRegistered_.store(true);
        LOGI("libmpv PCM tap registered successfully on Android");
    }

    LOGI("AudioEngineContext initialized successfully on Android");
    return true;
}

void AudioEngineContext::destroy() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_) {
        if (g_mpv_set_pcm_callback && pcmTapRegistered_.load()) {
            g_mpv_set_pcm_callback(mpvHandle_, nullptr, nullptr);
            pcmTapRegistered_.store(false);
        }
        if (g_mpv_destroy) {
            g_mpv_destroy(mpvHandle_);
        }
        mpvHandle_ = nullptr;
    }
    isPlaying_.store(false);
    endedCount_ = 0;
    lastEof_ = false;
    ringBuffer_->clear();
}

std::string AudioEngineContext::getStringProp(const char* name) {
    if (!mpvHandle_ || !g_mpv_get_property_string) return std::string();
    char* value = g_mpv_get_property_string(mpvHandle_, name);
    if (!value) return std::string();
    std::string out(value);
    if (g_mpv_free) g_mpv_free(value);
    return out;
}

double AudioEngineContext::getDoubleProp(const char* name, double def) {
    const std::string raw = getStringProp(name);
    if (raw.empty()) return def;
    char* end = nullptr;
    const double value = std::strtod(raw.c_str(), &end);
    if (end == raw.c_str()) return def;
    return value;
}

bool AudioEngineContext::load(const std::string& uri) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (!mpvHandle_ || !g_mpv_command) return false;

    const char* cmd[] = { "loadfile", uri.c_str(), "replace", nullptr };
    int err = g_mpv_command(mpvHandle_, cmd);
    if (err < 0) {
        LOGE("loadfile failed: %s", g_mpv_error_string ? g_mpv_error_string(err) : "unknown");
        return false;
    }
    isPlaying_.store(true);
    return true;
}

LoadResult AudioEngineContext::loadEx(const std::string& uri, const std::string& headerFields) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    LoadResult result;
    if (!mpvHandle_ || !g_mpv_command) return result;

    /*
     * The gapless re-bind: loading exactly the file that is already sounding
     * must NOT issue `loadfile replace` — replace of the current file stops
     * the track mid-flight and leaves the engine paused at zero. The desktop
     * sidecar answers `resumed: true` for this case; so does the contract.
     */
    const std::string target = stripFileScheme(uri);
    const std::string current = stripFileScheme(getStringProp("path"));
    const bool sounding = isPlaying_.load() && !lastEof_ && getDoubleProp("time-pos", 0.0) > 0.5;
    if (!current.empty() && current == target && sounding) {
        result.ok = true;
        result.resumed = true;
        result.durationMs = getDoubleProp("duration", 0.0) * 1000.0;
        result.endedCount = endedCount_;
        return result;
    }

    if (!headerFields.empty() && g_mpv_set_option_string) {
        g_mpv_set_option_string(mpvHandle_, "http-header-fields", headerFields.c_str());
    }

    const char* cmd[] = { "loadfile", uri.c_str(), "replace", nullptr };
    int err = g_mpv_command(mpvHandle_, cmd);
    if (err < 0) {
        LOGE("loadfile failed: %s", g_mpv_error_string ? g_mpv_error_string(err) : "unknown");
        return result;
    }
    isPlaying_.store(true);
    lastEof_ = false;

    // `loadfile` answers before the file is open; the duration is what the
    // caller builds its progress bar from, so give mpv a moment to probe it.
    // A live stream never grows one — the poll also ends on that shape, with
    // durationMs 0 meaning "unknown", exactly as the contract spells it.
    for (int i = 0; i < 40; i++) {
        const double duration = getDoubleProp("duration", 0.0);
        if (duration > 0.0) {
            result.durationMs = duration * 1000.0;
            break;
        }
        if (getStringProp("idle-active") == "yes") {
            // The load was refused (dead URL, unsupported file).
            LOGE("loadEx: engine went idle after loadfile — load refused");
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    result.ok = true;
    result.endedCount = endedCount_;
    return result;
}

bool AudioEngineContext::append(const std::string& uri, const std::string& headerFields) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (!mpvHandle_ || !g_mpv_command) return false;

    if (!headerFields.empty() && g_mpv_set_option_string) {
        g_mpv_set_option_string(mpvHandle_, "http-header-fields", headerFields.c_str());
    }
    const char* cmd[] = { "loadfile", uri.c_str(), "append", nullptr };
    int err = g_mpv_command(mpvHandle_, cmd);
    if (err < 0) {
        LOGE("loadfile append failed: %s", g_mpv_error_string ? g_mpv_error_string(err) : "unknown");
        return false;
    }
    return true;
}

void AudioEngineContext::play() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_ && g_mpv_set_property_string) {
        g_mpv_set_property_string(mpvHandle_, "pause", "no");
        isPlaying_.store(true);
    }
}

void AudioEngineContext::playAt(double atMs, bool hasAt) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (!mpvHandle_ || !g_mpv_set_property_string) return;
    if (hasAt && atMs >= 0.0 && g_mpv_command) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3) << atMs / 1000.0;
        const std::string secStr = ss.str();
        const char* cmd[] = { "seek", secStr.c_str(), "absolute", nullptr };
        g_mpv_command(mpvHandle_, cmd);
    }
    g_mpv_set_property_string(mpvHandle_, "pause", "no");
    isPlaying_.store(true);
}

void AudioEngineContext::pause() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_ && g_mpv_set_property_string) {
        g_mpv_set_property_string(mpvHandle_, "pause", "yes");
        isPlaying_.store(false);
    }
}

void AudioEngineContext::stop() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_ && g_mpv_command) {
        const char* cmd[] = { "stop", nullptr };
        g_mpv_command(mpvHandle_, cmd);
        isPlaying_.store(false);
        ringBuffer_->clear();
    }
}

void AudioEngineContext::seekMs(double ms) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_ && g_mpv_command) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3) << ms / 1000.0;
        std::string secStr = ss.str();
        const char* cmd[] = { "seek", secStr.c_str(), "absolute", nullptr };
        g_mpv_command(mpvHandle_, cmd);
    }
}

void AudioEngineContext::setVolume(double volume01) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_ && g_mpv_set_property_string) {
        const double clamped = std::max(0.0, std::min(1.0, volume01));
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << clamped * 100.0;
        std::string volStr = ss.str();
        g_mpv_set_property_string(mpvHandle_, "volume", volStr.c_str());
    }
}

void AudioEngineContext::setMuted(bool muted) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_ && g_mpv_set_property_string) {
        g_mpv_set_property_string(mpvHandle_, "mute", muted ? "yes" : "no");
    }
}

void AudioEngineContext::setDspConfig(const std::string& af,
                                       const std::string& replaygain,
                                       bool replaygainClip,
                                       const std::string& replaygainPreamp,
                                       const std::string& replaygainFallback) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (!mpvHandle_ || !g_mpv_set_property_string) return;

    // The lavfi chain as one `af` fragment; an empty string clears the chain.
    g_mpv_set_property_string(mpvHandle_, "af", af.c_str());

    // ReplayGain knobs are options, but options are settable as properties
    // on a live handle — the same trick the desktop sidecar uses.
    if (!replaygain.empty()) {
        g_mpv_set_property_string(mpvHandle_, "replaygain", replaygain.c_str());
    }
    g_mpv_set_property_string(mpvHandle_, "replaygain-clip", replaygainClip ? "yes" : "no");
    if (!replaygainPreamp.empty()) {
        g_mpv_set_property_string(mpvHandle_, "replaygain-preamp", replaygainPreamp.c_str());
    }
    if (!replaygainFallback.empty()) {
        g_mpv_set_property_string(mpvHandle_, "replaygain-fallback", replaygainFallback.c_str());
    }
}

void AudioEngineContext::setVisualizer(bool enabled, int fftSize) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (!enabled) {
        ringBuffer_->clear();
        return;
    }
    if (fftSize > 0 && fftProcessor_) {
        fftProcessor_->setFftSize(fftSize);
    }
}

void AudioEngineContext::setOutputDevice(const std::string& device) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_ && g_mpv_set_property_string && !device.empty()) {
        g_mpv_set_property_string(mpvHandle_, "audio-device", device.c_str());
    }
}

void AudioEngineContext::onPcmChunk(const float* samples, int frames, int channels, int sampleRate) {
    if (ringBuffer_ && frames > 0) {
        ringBuffer_->write(samples, static_cast<size_t>(frames), channels, sampleRate);
    }
}

bool AudioEngineContext::getSpectrum(uint8_t* outFreqData, uint8_t* outTimeData, size_t binCount, size_t timeCount) {
    if (!ringBuffer_ || !fftProcessor_) return false;

    size_t reqFrames = fftProcessor_->getFftSize();
    int channels = ringBuffer_->getChannels();
    if (channels <= 0) channels = 2;

    if (readPcmBuffer_.size() < reqFrames * channels) {
        readPcmBuffer_.resize(reqFrames * channels);
    }

    size_t readFrames = ringBuffer_->read(readPcmBuffer_.data(), reqFrames);
    fftProcessor_->processInterleaved(readPcmBuffer_.data(), readFrames, channels);

    const auto& freq = fftProcessor_->getFrequencyData();
    const auto& time = fftProcessor_->getTimeDomainData();

    if (outFreqData && binCount > 0) {
        size_t copyBins = std::min(binCount, freq.size());
        std::memcpy(outFreqData, freq.data(), copyBins);
    }

    if (outTimeData && timeCount > 0) {
        size_t copyTime = std::min(timeCount, time.size());
        std::memcpy(outTimeData, time.data(), copyTime);
    }

    return true;
}

audio_engine::RingBufferStats AudioEngineContext::getRingBufferStats() {
    if (ringBuffer_) {
        return ringBuffer_->getStats();
    }
    return {};
}

EngineState AudioEngineContext::getState() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    EngineState state;
    if (!mpvHandle_) return state;

    const double position = getDoubleProp("time-pos", -1.0);
    const double duration = getDoubleProp("duration", 0.0);
    const bool idle = getStringProp("idle-active") == "yes";
    const bool paused = getStringProp("pause") == "yes";
    const bool eof = getStringProp("eof-reached") == "yes";

    // Rising-edge counting: eof only announces a *new* end when it was not
    // already the state at the previous poll — a stalled ended file polled
    // forever must not inflate the counter the JS rollover detector reads.
    if (eof && !lastEof_) endedCount_++;
    lastEof_ = eof;

    if (idle) {
        state.status = "idle";
    } else if (eof) {
        state.status = "ended";
    } else if (paused) {
        state.status = "paused";
    } else {
        state.status = "playing";
    }

    state.positionMs = position > 0.0 ? position * 1000.0 : 0.0;
    state.durationMs = duration > 0.0 ? duration * 1000.0 : 0.0;
    state.endedCount = endedCount_;
    return state;
}

int AudioEngineContext::getFftBinCount() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    return fftProcessor_ ? fftProcessor_->getFrequencyBinCount() : 64;
}

} // namespace jni
} // namespace bbebee

// JNI Export Implementations
using namespace bbebee::jni;
using audio_engine::JsonValue;

static std::string jstringToStd(JNIEnv* env, jstring s) {
    if (!s || !env) return std::string();
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (!chars) return std::string();
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

static jstring stdToJstring(JNIEnv* env, const std::string& s) {
    return env ? env->NewStringUTF(s.c_str()) : nullptr;
}

static std::string stateToJson(const EngineState& state) {
    JsonValue v = JsonValue::object();
    v.objVal["positionMs"] = JsonValue(state.positionMs);
    v.objVal["durationMs"] = JsonValue(state.durationMs);
    v.objVal["status"] = JsonValue(state.status);
    v.objVal["endedCount"] = JsonValue(static_cast<int64_t>(state.endedCount));
    return v.serialize();
}

static std::string loadResultToJson(const LoadResult& result) {
    JsonValue v = JsonValue::object();
    v.objVal["ok"] = JsonValue(result.ok);
    v.objVal["durationMs"] = JsonValue(result.durationMs);
    v.objVal["resumed"] = JsonValue(result.resumed);
    v.objVal["endedCount"] = JsonValue(static_cast<int64_t>(result.endedCount));
    return v.serialize();
}

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeInit
  (JNIEnv *env, jobject, jstring configDirStr) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (!g_engineContext) {
        g_engineContext = new AudioEngineContext();
    }

    std::string configDir = jstringToStd(env, configDirStr);

    return g_engineContext->initialize(configDir) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeDestroy
  (JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) {
        delete g_engineContext;
        g_engineContext = nullptr;
    }
}

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeLoad
  (JNIEnv *env, jobject, jstring uriStr) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (!g_engineContext) return JNI_FALSE;
    const std::string uri = jstringToStd(env, uriStr);
    return g_engineContext->load(uri) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeLoadEx
  (JNIEnv *env, jobject, jstring uriStr, jstring headersStr) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (!g_engineContext) return stdToJstring(env, loadResultToJson(LoadResult()));
    const std::string uri = jstringToStd(env, uriStr);
    const std::string headers = jstringToStd(env, headersStr);
    return stdToJstring(env, loadResultToJson(g_engineContext->loadEx(uri, headers)));
}

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeAppend
  (JNIEnv *env, jobject, jstring uriStr, jstring headersStr) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (!g_engineContext) return JNI_FALSE;
    const std::string uri = jstringToStd(env, uriStr);
    const std::string headers = jstringToStd(env, headersStr);
    return g_engineContext->append(uri, headers) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePlay
  (JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->play();
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePlayAt
  (JNIEnv *, jobject, jdouble atMs) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->playAt(atMs, !std::isnan(atMs));
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePause
  (JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->pause();
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeStop
  (JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->stop();
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSeekMs
  (JNIEnv *, jobject, jdouble ms) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->seekMs(ms);
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetVolume
  (JNIEnv *, jobject, jdouble volume01) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->setVolume(volume01);
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetMuted
  (JNIEnv *, jobject, jboolean muted) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->setMuted(muted == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetDspConfig
  (JNIEnv *env, jobject, jstring afStr, jstring replaygainStr, jboolean clip, jstring preampStr, jstring fallbackStr) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (!g_engineContext) return;
    g_engineContext->setDspConfig(
        jstringToStd(env, afStr),
        jstringToStd(env, replaygainStr),
        clip == JNI_TRUE,
        jstringToStd(env, preampStr),
        jstringToStd(env, fallbackStr));
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetVisualizer
  (JNIEnv *, jobject, jboolean enabled, jint fftSize) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->setVisualizer(enabled == JNI_TRUE, static_cast<int>(fftSize));
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetOutputDevice
  (JNIEnv *env, jobject, jstring deviceStr) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->setOutputDevice(jstringToStd(env, deviceStr));
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetStreamPlayback
  (JNIEnv *, jobject, jboolean) {
    // No-op on Android: the degraded media-element path is a desktop-engine
    // concept; react-native-audio-api drives its own graph.
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetAudioExclusive
  (JNIEnv *, jobject, jboolean) {
    // No-op on Android: WASAPI exclusive mode is a desktop concept.
}

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetSpectrum
  (JNIEnv *env, jobject, jbyteArray outFreqArr, jbyteArray outTimeArr) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (!g_engineContext) return JNI_FALSE;

    jsize freqLen = outFreqArr ? env->GetArrayLength(outFreqArr) : 0;
    jsize timeLen = outTimeArr ? env->GetArrayLength(outTimeArr) : 0;

    std::vector<uint8_t> freqBuf(freqLen > 0 ? freqLen : 1);
    std::vector<uint8_t> timeBuf(timeLen > 0 ? timeLen : 1);

    bool ok = g_engineContext->getSpectrum(
        freqLen > 0 ? freqBuf.data() : nullptr,
        timeLen > 0 ? timeBuf.data() : nullptr,
        freqLen, timeLen
    );

    if (ok) {
        if (freqLen > 0 && outFreqArr) {
            env->SetByteArrayRegion(outFreqArr, 0, freqLen, reinterpret_cast<const jbyte*>(freqBuf.data()));
        }
        if (timeLen > 0 && outTimeArr) {
            env->SetByteArrayRegion(outTimeArr, 0, timeLen, reinterpret_cast<const jbyte*>(timeBuf.data()));
        }
    }

    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlongArray JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetRingBufferStats
  (JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    jlongArray result = env->NewLongArray(5);
    if (!result) return nullptr;

    jlong stats[5] = { 0, 0, 0, 0, 0 };
    if (g_engineContext) {
        auto s = g_engineContext->getRingBufferStats();
        stats[0] = static_cast<jlong>(s.totalFramesWritten);
        stats[1] = static_cast<jlong>(s.totalFramesRead);
        stats[2] = static_cast<jlong>(s.droppedFrames);
        stats[3] = static_cast<jlong>(s.overflowCount);
        stats[4] = static_cast<jlong>(s.underrunCount);
    }

    env->SetLongArrayRegion(result, 0, 5, stats);
    return result;
}

JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetState
  (JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (!g_engineContext) return stdToJstring(env, stateToJson(EngineState()));
    return stdToJstring(env, stateToJson(g_engineContext->getState()));
}

JNIEXPORT jint JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetFftBinCount
  (JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (!g_engineContext) return 64;
    return static_cast<jint>(g_engineContext->getFftBinCount());
}

JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetEngineStatus
  (JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    bool running = g_engineContext != nullptr;
    bool mpvAvail = g_engineContext ? g_engineContext->isMpvAvailable() : false;
    bool pcmAvail = g_engineContext ? g_engineContext->isPcmTapAvailable() : false;

    JsonValue v = JsonValue::object();
    v.objVal["running"] = JsonValue(running);
    v.objVal["mpvAvailable"] = JsonValue(mpvAvail);
    v.objVal["pcmTapAvailable"] = JsonValue(pcmAvail);
    v.objVal["platform"] = JsonValue("android");
    return stdToJstring(env, v.serialize());
}
