#include "bbebee_audio_jni.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

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
typedef int (*mpv_set_pcm_callback_fn)(mpv_handle*, mpv_pcm_callback_fn, void*);

static void* g_libmpvHandle = nullptr;
static mpv_create_fn g_mpv_create = nullptr;
static mpv_initialize_fn g_mpv_initialize = nullptr;
static mpv_destroy_fn g_mpv_destroy = nullptr;
static mpv_command_fn g_mpv_command = nullptr;
static mpv_set_option_string_fn g_mpv_set_option_string = nullptr;
static mpv_set_property_string_fn g_mpv_set_property_string = nullptr;
static mpv_error_string_fn g_mpv_error_string = nullptr;
static mpv_set_pcm_callback_fn g_mpv_set_pcm_callback = nullptr;

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

static void pcmCallbackTrampoline(const float* data, size_t frames, int channels, int sample_rate, void* user_data) {
    if (user_data) {
        auto* ctx = reinterpret_cast<AudioEngineContext*>(user_data);
        ctx->onPcmChunk(data, frames, channels, sample_rate);
    }
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
        int tapErr = g_mpv_set_pcm_callback(mpvHandle_, pcmCallbackTrampoline, this);
        if (tapErr == 0) {
            pcmTapRegistered_.store(true);
            LOGI("libmpv PCM tap registered successfully on Android");
        } else {
            pcmTapRegistered_.store(false);
            LOGW("mpv_set_pcm_callback returned error code: %d", tapErr);
        }
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
    ringBuffer_->clear();
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

void AudioEngineContext::play() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_ && g_mpv_set_property_string) {
        g_mpv_set_property_string(mpvHandle_, "pause", "no");
        isPlaying_.store(true);
    }
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

void AudioEngineContext::seek(double seconds) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_ && g_mpv_command) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3) << seconds;
        std::string secStr = ss.str();
        const char* cmd[] = { "seek", secStr.c_str(), "absolute", nullptr };
        g_mpv_command(mpvHandle_, cmd);
    }
}

void AudioEngineContext::setVolume(double volume) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (mpvHandle_ && g_mpv_set_property_string) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << volume;
        std::string volStr = ss.str();
        g_mpv_set_property_string(mpvHandle_, "volume", volStr.c_str());
    }
}

void AudioEngineContext::onPcmChunk(const float* samples, size_t frames, int channels, int sampleRate) {
    if (ringBuffer_) {
        ringBuffer_->write(samples, frames, channels, sampleRate);
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

} // namespace jni
} // namespace bbebee

// JNI Export Implementations
using namespace bbebee::jni;

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeInit
  (JNIEnv *env, jobject, jstring configDirStr) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (!g_engineContext) {
        g_engineContext = new AudioEngineContext();
    }

    std::string configDir;
    if (configDirStr) {
        const char* nativeChars = env->GetStringUTFChars(configDirStr, nullptr);
        if (nativeChars) {
            configDir = nativeChars;
            env->ReleaseStringUTFChars(configDirStr, nativeChars);
        }
    }

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
    if (!g_engineContext || !uriStr) return JNI_FALSE;

    const char* nativeUri = env->GetStringUTFChars(uriStr, nullptr);
    if (!nativeUri) return JNI_FALSE;
    std::string uri(nativeUri);
    env->ReleaseStringUTFChars(uriStr, nativeUri);

    return g_engineContext->load(uri) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePlay
  (JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->play();
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

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSeek
  (JNIEnv *, jobject, jdouble seconds) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->seek(seconds);
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetVolume
  (JNIEnv *, jobject, jdouble volume) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    if (g_engineContext) g_engineContext->setVolume(volume);
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

JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetEngineStatus
  (JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    bool mpvAvail = g_engineContext ? g_engineContext->isMpvAvailable() : false;
    bool pcmAvail = g_engineContext ? g_engineContext->isPcmTapAvailable() : false;

    std::ostringstream ss;
    ss << "{\"mpvAvailable\":" << (mpvAvail ? "true" : "false")
       << ",\"pcmTapAvailable\":" << (pcmAvail ? "true" : "false")
       << ",\"platform\":\"android\"}";
    return env->NewStringUTF(ss.str().c_str());
}
