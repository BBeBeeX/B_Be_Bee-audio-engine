#include "bbebee_audio_jni.hpp"

#include <android/log.h>
#include <dlfcn.h>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <thread>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

#include "../json.hpp"
#include "../transcode.hpp"
#include "transcode_caps.hpp"

#define LOG_TAG "BBeBeeAudioJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace bbebee {
namespace jni {

/* Shared ownership: JNI exports capture a local copy, so an in-flight call
 * (notably the loadEx probe, which sleeps outside engineMutex_) survives
 * nativeDestroy resetting the global. The object is destroyed only once
 * the last in-flight call returns; methods still serialize on
 * engineMutex_ and treat a null mpvHandle_ as "engine gone". */
static std::shared_ptr<AudioEngineContext> g_engineContext;
static std::mutex g_contextMutex;

/** Snapshot the global context under g_contextMutex; may be null. */
static std::shared_ptr<AudioEngineContext> captureContext() {
    std::lock_guard<std::mutex> lock(g_contextMutex);
    return g_engineContext;
}

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

    /* mpv_free is mandatory, not optional: getStringProp() polls several
     * properties per getState() call, and a missing mpv_free would leak
     * every returned string for the life of the process. Without it the
     * engine falls back to mock mode (see initialize()) instead of
     * running with a slow leak. */
    if (!g_mpv_create || !g_mpv_initialize || !g_mpv_destroy || !g_mpv_command || !g_mpv_free) {
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

/** `file:///a/b`, `file://localhost/a/b` and `/a/b` name the same file
 * as far as callers care. A `file://host/path` form with any other host
 * keeps its host as the first path component (there is no remote-file
 * interpretation here); the important property is that the mapping is
 * consistent, so two spellings of one file compare equal below. */
static std::string stripFileScheme(const std::string& uri) {
    const std::string prefix = "file://";
    if (uri.rfind(prefix, 0) != 0) return uri;
    std::string rest = uri.substr(prefix.size());
    if (!rest.empty() && rest[0] == '/') return rest;
    const std::string localhost = "localhost/";
    if (rest.rfind(localhost, 0) == 0) return "/" + rest.substr(localhost.size());
    return rest;
}

/**
 * Lexical normalization for same-file comparison only (no filesystem
 * access, so symlinks and hard links are NOT resolved. That errs in
 * the safe direction for this guard: lexical variants of one path
 * (`file:///...`, `/a/./b`, `/a/x/../b`) are caught, distinct files
 * are never falsely rejected, but a symlink/hardlink alias of the
 * input still compares unequal and would slip through — a residual
 * risk, not a false rejection).
 */
static std::string normalizeFilePathForComparison(const std::string& raw) {
    std::string path = stripFileScheme(raw);
    // A URI query/fragment is not part of a local file path.
    const size_t cut = path.find_first_of("?#");
    if (cut != std::string::npos) path = path.substr(0, cut);
    std::vector<std::string> parts;
    std::string cur;
    const bool absolute = !path.empty() && path[0] == '/';
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (cur.empty() || cur == ".") {
                // skip
            } else if (cur == "..") {
                if (!parts.empty() && parts.back() != "..") parts.pop_back();
                else if (!absolute) parts.push_back(cur);
            } else {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += path[i];
        }
    }
    std::string out = absolute ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "/";
        out += parts[i];
    }
    return out;
}

static bool pathsReferToSameFile(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    return normalizeFilePathForComparison(a) == normalizeFilePathForComparison(b);
}

/**
 * No CRLF strip here, on purpose: `headerFields` is a CRLF-joined
 * multi-header string, and stripping every CR/LF would collapse it into
 * one line and corrupt multi-header requests. CRLF sanitization belongs
 * at the adapter, per header value before joining (see
 * BBeBee's adapter.ts `headersToFields`, which strips CR/LF from each
 * name and value, then joins with CRLF). Direct JNI callers must do the
 * same per-value sanitization; this layer must not re-strip the join.
 */

/**
 * mpv sample-format name → bit depth (`u8`, `s16(p)`, `s24(p)`, `s32(p)`,
 * `float(p)`, `double(p)`). 0 when the format is unknown — the contract
 * reports bit depth as optional, never as a guess.
 */
static int audioFormatBitDepth(const std::string& format) {
    if (format.empty()) return 0;
    if (format.find("double") != std::string::npos) return 64;
    if (format.find("float") != std::string::npos) return 32;
    if (format.rfind("s32", 0) == 0 || format.rfind("u32", 0) == 0) return 32;
    if (format.rfind("s24", 0) == 0 || format.rfind("u24", 0) == 0) return 24;
    if (format.rfind("s16", 0) == 0 || format.rfind("u16", 0) == 0) return 16;
    if (format.rfind("u8", 0) == 0 || format.rfind("s8", 0) == 0) return 8;
    return 0;
}

// ---------------------------------------------------------------------------
// Transcode job registry.
//
// Deliberately NOT under g_contextMutex / engineMutex_: a running transcode
// is a minutes-long operation and must never stall playback control, which
// every engine-context export serializes through. The worker thread owns its
// job record via shared_ptr, so even a pruned record outlives the thread;
// progress fields are atomics and the terminal fields are published by the
// release-store of `done` (single writer per record).
// ---------------------------------------------------------------------------
struct TranscodeJob {
    std::string jobId;
    /** Normalized output path, for the active-output registry below. */
    std::string outputPath;
    std::shared_ptr<std::atomic<bool>> cancelFlag = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> running{ true };
    std::atomic<bool> done{ false };
    std::atomic<bool> ok{ false };
    std::atomic<bool> cancelled{ false };
    // progress 0..1 scaled by 10000; times in ms
    std::atomic<int64_t> progressBps{ 0 };
    std::atomic<int64_t> currentTimeMs{ 0 };
    std::atomic<int64_t> durationMs{ 0 };
    /** Epoch ms of completion — finished jobs stay readable for a grace window. */
    std::atomic<int64_t> doneAtMs{ 0 };
    std::string error;
};

static std::mutex g_jobsMutex;
static std::map<std::string, std::shared_ptr<TranscodeJob>> g_transcodeJobs;
/** Normalized output path -> jobId for jobs not yet done. Two workers
 * must never write one destination: the second registration is refused
 * (see nativeTranscode) instead of racing the first one's truncate. */
static std::map<std::string, std::string> g_activeTranscodeOutputs;
/** Detached transcode workers in flight. nativeDestroy cancels them and
 * waits (bounded) for this count to reach zero; workers never touch the
 * engine context, only their shared job record, so a timeout still leaves
 * no dangling context use — only a worker that keeps running to its own
 * terminal state on a record that outlives destroy. */
static std::atomic<int> g_transcodeWorkersActive{ 0 };
static std::mutex g_workersMutex;
static std::condition_variable g_workersCv;

/** Finished jobs vanish after this long; a poll racing completion reads ms
 *  later and always finds the record, a cancelled-and-restarted job does not
 *  leak memory across a long session. */
static constexpr int64_t kFinishedJobRetentionMs = 5 * 60 * 1000;

/** Caller must hold g_jobsMutex — its only caller runs inside the same
 *  critical section that registers the new job, and std::mutex does not
 *  recurse. */
static void pruneFinishedJobsLocked() {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    for (auto it = g_transcodeJobs.begin(); it != g_transcodeJobs.end();) {
        const bool expired = it->second->done.load(std::memory_order_acquire) &&
                             it->second->doneAtMs.load(std::memory_order_acquire) > 0 &&
                             now - it->second->doneAtMs.load(std::memory_order_acquire) > kFinishedJobRetentionMs;
        if (expired) {
            it = g_transcodeJobs.erase(it);
        } else {
            ++it;
        }
    }
}

static std::shared_ptr<TranscodeJob> findJob(const std::string& jobId) {
    std::lock_guard<std::mutex> lock(g_jobsMutex);
    auto it = g_transcodeJobs.find(jobId);
    return it != g_transcodeJobs.end() ? it->second : nullptr;
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
    stopRequested_ = false;
    hadLoad_ = false;
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
    // A fresh load invalidates both error-status markers: the previous file's
    // eof says nothing about this one, and whatever is idle after this point
    // is no longer the user's stop.
    lastEof_ = false;
    stopRequested_ = false;
    hadLoad_ = true;
    return true;
}

/*
 * Trust boundary for load()/loadEx()/append()/setDspConfig(): the URI,
 * the `af` lavfi fragment and the header fields are passed to mpv
 * exactly as the caller supplied them (header fields with each value
 * already CRLF-sanitized by the adapter before joining). No allowlist
 * is applied here on purpose: an `af`
 * allowlist would break legitimate DSP chains and a URI allowlist
 * would break legitimate playback, so the responsibility sits with the
 * caller — the Kotlin/JS adapter must only forward values that come
 * from trusted app settings or the user's own library, never verbatim
 * from an untrusted source document. If that boundary ever changes,
 * the allowlist belongs at the adapter, where a header value can still
 * be sanitized before it is joined into a multi-header string.
 */
LoadResult AudioEngineContext::loadEx(const std::string& uri, const std::string& headerFields) {
    std::unique_lock<std::mutex> lock(engineMutex_);
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

    // headerFields arrives CRLF-joined with each value already sanitized
    // by the adapter; do not strip CRLF here (that would collapse the
    // multi-header join into one line).
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
    // The command succeeded, so a load "took" even if the probe below catches
    // a refusal: an engine left idle by a dead URL must read as error, not as
    // a fresh boot.
    stopRequested_ = false;
    hadLoad_ = true;
    // The handle this load belongs to. The probe below drops engineMutex_
    // while it sleeps (see below); if destroy() runs in that window it
    // nulls mpvHandle_, and the probe must notice and stop instead of
    // reporting a load on a dead engine as ok.
    mpv_handle* const handleAtLoad = mpvHandle_;

    // `loadfile` answers before the file is open; the duration is what the
    // caller builds its progress bar from, so give mpv a moment to probe it.
    // A live stream never grows one — the poll also ends on that shape, with
    // durationMs 0 meaning "unknown", exactly as the contract spells it.
    // The decoded output parameters ride along on the same poll: they are
    // optional in the contract and only reported once the audio chain knows
    // them (0 stays out of the JSON on the JS side).
    //
    // The lock is held only for each property read, not for the sleep:
    // the old loop held engineMutex_ for the whole worst-case 2 s probe,
    // and every control call (play/pause/getState/...) serializes on that
    // same mutex, so one slow probe stalled all of them. The context
    // object itself stays alive across the unlocked sleeps because JNI
    // exports hold it by shared_ptr (see g_engineContext), and the
    // handle check below is what makes the unlocked window safe.
    bool probeSatisfied = false;
    for (int i = 0; i < 40; i++) {
        if (mpvHandle_ != handleAtLoad) return result;
        // E3: audio-out-params, not audio-params. audio-params is the
        // decoder output; audio-out-params is what the AO actually
        // plays after resampling/mixing, which is what the JS side
        // (and the desktop engine, which fixed this same mismatch)
        // must be told. Reporting decoder parameters here could
        // disagree with the PCM tap's actual format.
        if (result.sampleRate <= 0) result.sampleRate = static_cast<int>(getDoubleProp("audio-out-params/samplerate", 0.0));
        if (result.channels <= 0) result.channels = static_cast<int>(getDoubleProp("audio-out-params/channel-count", 0.0));
        if (result.durationMs <= 0.0) {
            const double duration = getDoubleProp("duration", 0.0);
            if (duration > 0.0) result.durationMs = duration * 1000.0;
        }
        if (result.durationMs > 0.0 && result.sampleRate > 0 && result.channels > 0) {
            probeSatisfied = true;
            break;
        }
        if (getStringProp("idle-active") == "yes") {
            // The load was refused (dead URL, unsupported file).
            LOGE("loadEx: engine went idle after loadfile — load refused");
            return result;
        }
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        lock.lock();
    }
    // The loop's last sleep is unchecked: a load refused exactly on the
    // 2 s boundary used to be reported ok with duration 0. Re-check idle
    // once, under the lock, before claiming success — but only when the
    // probe was NOT satisfied: a very short file that loaded, probed and
    // already ended is idle too, and must not be reclassified as refused.
    if (mpvHandle_ != handleAtLoad) return result;
    if (!probeSatisfied && getStringProp("idle-active") == "yes") {
        LOGE("loadEx: engine went idle after loadfile — load refused");
        return result;
    }
    result.ok = true;
    result.endedCount = endedCount_;
    if (result.sampleRate <= 0) result.sampleRate = static_cast<int>(getDoubleProp("audio-out-params/samplerate", 0.0));
    if (result.channels <= 0) result.channels = static_cast<int>(getDoubleProp("audio-out-params/channel-count", 0.0));
    result.bitDepth = audioFormatBitDepth(getStringProp("audio-out-params/format"));
    return result;
}

bool AudioEngineContext::append(const std::string& uri, const std::string& headerFields) {
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (!mpvHandle_ || !g_mpv_command) return false;

    // See loadEx: headerFields is already per-value sanitized by the
    // adapter; do not strip CRLF here.
    if (!headerFields.empty() && g_mpv_set_option_string) {
        g_mpv_set_option_string(mpvHandle_, "http-header-fields", headerFields.c_str());
    }
    const char* cmd[] = { "loadfile", uri.c_str(), "append", nullptr };
    int err = g_mpv_command(mpvHandle_, cmd);
    if (err < 0) {
        LOGE("loadfile append failed: %s", g_mpv_error_string ? g_mpv_error_string(err) : "unknown");
        return false;
    }
    // An accepted append is playlist content the engine owes an answer about:
    // it keeps the error-status markers honest across the playlist boundary.
    stopRequested_ = false;
    hadLoad_ = true;
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
    // Recorded before the command: the idle state `stop` produces is
    // obedience, and getState must never read it as a mid-track failure —
    // even if the engine is already gone (the flag then only moots an error
    // the wedged engine would otherwise keep reporting).
    stopRequested_ = true;
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
        // Publish the observed format for the lazy reconfiguration, then
        // write — which is a no-op while the buffer's format guard sees a
        // mismatch, until syncRingBufferFormatLocked() runs.
        tapSampleRate_.store(sampleRate, std::memory_order_relaxed);
        tapChannels_.store(channels, std::memory_order_relaxed);
        ringBuffer_->write(samples, static_cast<size_t>(frames), channels, sampleRate);
    }
}

void AudioEngineContext::syncRingBufferFormatLocked() {
    if (!ringBuffer_) return;
    const int sr = tapSampleRate_.load(std::memory_order_relaxed);
    const int ch = tapChannels_.load(std::memory_order_relaxed);
    if (sr <= 0 || ch <= 0) return;
    if (ringBuffer_->getSampleRate() == sr && ringBuffer_->getChannels() == ch) return;
    // configure() synchronizes with writer and reader quiescence; the tap
    // drops frames for the brief window, exactly like any format switch.
    if (ringBuffer_->configure(sr, ch, 8192)) {
        LOGI("ring buffer reconfigured to %d Hz / %d ch", sr, ch);
    }
}

bool AudioEngineContext::getSpectrum(uint8_t* outFreqData, uint8_t* outTimeData, size_t binCount, size_t timeCount) {
    // Serialized here, not by the caller: JNI exports no longer serialize
    // on g_contextMutex (see g_engineContext), so destroy() could
    // otherwise run concurrently with this read. No caller holds
    // engineMutex_ when calling getSpectrum, so this lock cannot recurse.
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (!ringBuffer_ || !fftProcessor_) return false;

    // The tap's format wins: without this, a 48 kHz track on the 44.1 kHz
    // default configuration drops every frame and the visualizer sits at zero.
    syncRingBufferFormatLocked();

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
    std::lock_guard<std::mutex> lock(engineMutex_);
    if (ringBuffer_) {
        return ringBuffer_->getStats();
    }
    return {};
}

EngineState AudioEngineContext::getState() {
    std::lock_guard<std::mutex> lock(engineMutex_);
    EngineState state;
    if (!mpvHandle_) {
        // A context that lost its mpv core after a load had taken is wedged,
        // not idle — the default answer here had the JS handle poll a
        // healthy-looking zero forever, with engineStatus still claiming the
        // engine runs. Report it as what the desktop sidecar would: error.
        if (hadLoad_) state.status = "error";
        return state;
    }

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
        // Idle after a load, with no user stop and no natural end in between,
        // is how a mid-track failure (dead URL, decoder error) looks in the
        // polling model: mpv unloads the file and goes idle, and without
        // keep-open the eof transient is usually gone before the next poll —
        // the mirror of the desktop engine's END_FILE reason=error. The JS
        // handle reads error at position zero as the end of the track, so a
        // refused load skips instead of stalling the queue.
        if (hadLoad_ && !lastEof_ && !stopRequested_) {
            state.status = "error";
        } else {
            state.status = "idle";
        }
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

/**
 * Whether this build can transcode at all. Under HAVE_LIBAV the real
 * Transcoder answers; without it transcode.cpp is not linked, so the
 * constant false is the only safe answer — every transcode export funnels
 * through this and degrades exactly like the desktop engine's stub.
 */
static bool transcodeSupported() {
#ifdef HAVE_LIBAV
    return audio_engine::Transcoder::isSupported();
#else
    return false;
#endif
}

static std::string jstringToStd(JNIEnv* env, jstring s) {
    if (!s || !env) return std::string();
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (!chars) {
        /* A null return means a pending JNI exception (typically OOM).
         * Clear it here: every caller treats the empty string as "no
         * value" and continues into further JNI calls, and a pending
         * exception would make all of those fail opaquely instead. */
        if (env->ExceptionCheck()) env->ExceptionClear();
        return std::string();
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

static jstring stdToJstring(JNIEnv* env, const std::string& s) {
    if (!env) return nullptr;
    jstring out = env->NewStringUTF(s.c_str());
    if (!out && env->ExceptionCheck()) {
        /* Same contract as above: null is the failure signal the Java
         * side already handles; a pending exception must not leak into
         * the next JNI call on this thread. */
        env->ExceptionClear();
    }
    return out;
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
    // Optional in the contract: a zero means "unknown", not "zero Hz", and
    // the field stays absent rather than lying.
    if (result.sampleRate > 0) v.objVal["sampleRate"] = JsonValue(static_cast<int64_t>(result.sampleRate));
    if (result.channels > 0) v.objVal["channels"] = JsonValue(static_cast<int64_t>(result.channels));
    if (result.bitDepth > 0) v.objVal["bitDepth"] = JsonValue(static_cast<int64_t>(result.bitDepth));
    return v.serialize();
}

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeInit
  (JNIEnv *env, jobject, jstring configDirStr) {
    std::shared_ptr<AudioEngineContext> ctx;
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        if (!g_engineContext) {
            g_engineContext = std::make_shared<AudioEngineContext>();
        }
        ctx = g_engineContext;
    }

    std::string configDir = jstringToStd(env, configDirStr);

    return ctx->initialize(configDir) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeDestroy
  (JNIEnv *, jobject) {
    std::shared_ptr<AudioEngineContext> ctx;
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        ctx = g_engineContext;
        g_engineContext.reset();
    }
    if (ctx) ctx->destroy();
    // Cancel every transcode job, then wait — bounded — for the detached
    // workers to actually exit. The wait holds no lock a worker needs
    // (workers take g_jobsMutex only briefly at completion), so it
    // cannot deadlock against them; a worker stuck in a blocking libav
    // read past the deadline simply keeps running to its own terminal
    // state on a shared job record that outlives this call, and it never
    // touches the engine context, so the timeout is safe, not a leak of
    // a dangling use. The deadline exists so destroy() can never hang
    // the calling thread on a wedged transcode.
    {
        std::lock_guard<std::mutex> jobsLock(g_jobsMutex);
        for (auto& entry : g_transcodeJobs) {
            entry.second->cancelFlag->store(true, std::memory_order_relaxed);
        }
    }
    {
        std::unique_lock<std::mutex> workersLock(g_workersMutex);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        g_workersCv.wait_until(workersLock, deadline, [] {
            return g_transcodeWorkersActive.load(std::memory_order_acquire) == 0;
        });
    }
}

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeLoad
  (JNIEnv *env, jobject, jstring uriStr) {
    auto ctx = captureContext();
    if (!ctx) return JNI_FALSE;
    const std::string uri = jstringToStd(env, uriStr);
    return ctx->load(uri) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeLoadEx
  (JNIEnv *env, jobject, jstring uriStr, jstring headersStr) {
    auto ctx = captureContext();
    if (!ctx) return stdToJstring(env, loadResultToJson(LoadResult()));
    const std::string uri = jstringToStd(env, uriStr);
    const std::string headers = jstringToStd(env, headersStr);
    return stdToJstring(env, loadResultToJson(ctx->loadEx(uri, headers)));
}

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeAppend
  (JNIEnv *env, jobject, jstring uriStr, jstring headersStr) {
    auto ctx = captureContext();
    if (!ctx) return JNI_FALSE;
    const std::string uri = jstringToStd(env, uriStr);
    const std::string headers = jstringToStd(env, headersStr);
    return ctx->append(uri, headers) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePlay
  (JNIEnv *, jobject) {
    auto ctx = captureContext();
    if (ctx) ctx->play();
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePlayAt
  (JNIEnv *, jobject, jdouble atMs) {
    auto ctx = captureContext();
    if (ctx) ctx->playAt(atMs, !std::isnan(atMs));
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePause
  (JNIEnv *, jobject) {
    auto ctx = captureContext();
    if (ctx) ctx->pause();
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeStop
  (JNIEnv *, jobject) {
    auto ctx = captureContext();
    if (ctx) ctx->stop();
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSeekMs
  (JNIEnv *, jobject, jdouble ms) {
    auto ctx = captureContext();
    if (ctx) ctx->seekMs(ms);
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetVolume
  (JNIEnv *, jobject, jdouble volume01) {
    auto ctx = captureContext();
    if (ctx) ctx->setVolume(volume01);
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetMuted
  (JNIEnv *, jobject, jboolean muted) {
    auto ctx = captureContext();
    if (ctx) ctx->setMuted(muted == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetDspConfig
  (JNIEnv *env, jobject, jstring afStr, jstring replaygainStr, jboolean clip, jstring preampStr, jstring fallbackStr) {
    auto ctx = captureContext();
    if (!ctx) return;
    ctx->setDspConfig(
        jstringToStd(env, afStr),
        jstringToStd(env, replaygainStr),
        clip == JNI_TRUE,
        jstringToStd(env, preampStr),
        jstringToStd(env, fallbackStr));
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetVisualizer
  (JNIEnv *, jobject, jboolean enabled, jint fftSize) {
    auto ctx = captureContext();
    if (ctx) ctx->setVisualizer(enabled == JNI_TRUE, static_cast<int>(fftSize));
}

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetOutputDevice
  (JNIEnv *env, jobject, jstring deviceStr) {
    auto ctx = captureContext();
    if (ctx) ctx->setOutputDevice(jstringToStd(env, deviceStr));
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
    auto ctx = captureContext();
    if (!ctx) return JNI_FALSE;

    jsize freqLen = outFreqArr ? env->GetArrayLength(outFreqArr) : 0;
    jsize timeLen = outTimeArr ? env->GetArrayLength(outTimeArr) : 0;

    std::vector<uint8_t> freqBuf(freqLen > 0 ? freqLen : 1);
    std::vector<uint8_t> timeBuf(timeLen > 0 ? timeLen : 1);

    bool ok = ctx->getSpectrum(
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
    auto ctx = captureContext();
    jlongArray result = env->NewLongArray(5);
    if (!result) return nullptr;

    jlong stats[5] = { 0, 0, 0, 0, 0 };
    if (ctx) {
        auto s = ctx->getRingBufferStats();
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
    auto ctx = captureContext();
    if (!ctx) return stdToJstring(env, stateToJson(EngineState()));
    return stdToJstring(env, stateToJson(ctx->getState()));
}

JNIEXPORT jint JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetFftBinCount
  (JNIEnv *, jobject) {
    auto ctx = captureContext();
    if (!ctx) return 64;
    return static_cast<jint>(ctx->getFftBinCount());
}

JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetEngineStatus
  (JNIEnv *env, jobject) {
    auto ctx = captureContext();
    bool running = ctx != nullptr;
    bool mpvAvail = ctx ? ctx->isMpvAvailable() : false;
    bool pcmAvail = ctx ? ctx->isPcmTapAvailable() : false;

    JsonValue v = JsonValue::object();
    v.objVal["running"] = JsonValue(running);
    v.objVal["mpvAvailable"] = JsonValue(mpvAvail);
    v.objVal["pcmTapAvailable"] = JsonValue(pcmAvail);
    v.objVal["transcodeAvailable"] = JsonValue(transcodeSupported());
    v.objVal["platform"] = JsonValue("android");
    return stdToJstring(env, v.serialize());
}

// ---------------------------------------------------------------------------
// Transcoder exports
// ---------------------------------------------------------------------------

static std::string transcodeStateToJson(const TranscodeJob& job) {
    JsonValue v = JsonValue::object();
    v.objVal["jobId"] = JsonValue(job.jobId);
    v.objVal["found"] = JsonValue(true);
    v.objVal["running"] = JsonValue(job.running.load(std::memory_order_acquire));
    v.objVal["done"] = JsonValue(job.done.load(std::memory_order_acquire));
    v.objVal["ok"] = JsonValue(job.ok.load(std::memory_order_acquire));
    v.objVal["cancelled"] = JsonValue(job.cancelled.load(std::memory_order_acquire));
    v.objVal["progress"] = JsonValue(static_cast<double>(job.progressBps.load(std::memory_order_acquire)) / 10000.0);
    v.objVal["currentTimeSec"] = JsonValue(static_cast<double>(job.currentTimeMs.load(std::memory_order_acquire)) / 1000.0);
    v.objVal["durationSec"] = JsonValue(static_cast<double>(job.durationMs.load(std::memory_order_acquire)) / 1000.0);
    if (job.done.load(std::memory_order_acquire) && !job.error.empty()) {
        v.objVal["error"] = JsonValue(job.error);
    }
    return v.serialize();
}

JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeTranscode
  (JNIEnv *env, jobject, jstring jobIdStr, jstring inputStr, jstring outputStr,
   jstring formatStr, jstring bitrateStr, jint sampleRate, jint channels,
   jdouble startSec, jdouble endSec) {
    if (!transcodeSupported()) {
        JsonValue v = JsonValue::object();
        v.objVal["ok"] = JsonValue(false);
        v.objVal["error"] = JsonValue("transcoding is not supported in this build (no FFmpeg)");
        return stdToJstring(env, v.serialize());
    }

    std::string jobId = jstringToStd(env, jobIdStr);
    if (jobId.empty()) {
        // A bare millisecond timestamp collides when two callers fire in
        // the same millisecond — and a collision used to mean the second
        // job cancelled-and-replaced the first. The monotonic counter
        // makes generated ids unique within the process lifetime.
        static std::atomic<unsigned long> generatedJobCounter{ 0 };
        const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        jobId = "transcode-" + std::to_string(ms) + "-" +
                std::to_string(generatedJobCounter.fetch_add(1, std::memory_order_relaxed));
    }

    audio_engine::TranscodeOptions opts;
    opts.jobId = jobId;
    opts.inputPath = stripFileScheme(jstringToStd(env, inputStr));
    opts.outputPath = stripFileScheme(jstringToStd(env, outputStr));
    opts.format = jstringToStd(env, formatStr);
    if (opts.format.empty()) opts.format = "mp3";
    opts.bitrate = jstringToStd(env, bitrateStr);
    opts.sampleRate = static_cast<int>(sampleRate);
    opts.channels = static_cast<int>(channels);
    opts.startTimeSec = startSec > 0.0 ? startSec : 0.0;
    opts.endTimeSec = endSec > 0.0 ? endSec : 0.0;

    JsonValue answer = JsonValue::object();
    if (opts.inputPath.empty() || opts.outputPath.empty()) {
        answer.objVal["ok"] = JsonValue(false);
        answer.objVal["error"] = JsonValue("input and output paths are required");
        answer.objVal["jobId"] = JsonValue(jobId);
        return stdToJstring(env, answer.serialize());
    }
    // M1: input == output would truncate the source mid-decode (the
    // transcoder opens the input, then opens the output for writing).
    // Reject before any worker starts, on the normalized spellings, so
    // `file:///a/b` vs `/a/b` vs `file://localhost/a/b` all match.
    // E4 (comment correction): this is defense in depth, not the only
    // guard — Transcoder::transcode has its own same-path check since
    // 49224ff, plus a file-identity check, so both the JNI and the
    // desktop NDJSON paths are covered even if a caller skips this one.
    if (pathsReferToSameFile(opts.inputPath, opts.outputPath)) {
        answer.objVal["ok"] = JsonValue(false);
        answer.objVal["error"] = JsonValue("input and output paths must differ");
        answer.objVal["jobId"] = JsonValue(jobId);
        return stdToJstring(env, answer.serialize());
    }
    const std::string normalizedOutput = normalizeFilePathForComparison(opts.outputPath);

    {
        std::lock_guard<std::mutex> lock(g_jobsMutex);
        auto existing = g_transcodeJobs.find(jobId);
        if (existing != g_transcodeJobs.end() && !existing->second->done.load(std::memory_order_acquire)) {
            // M3: a live jobId is refused, not cancelled-and-replaced.
            // Replacing left the old worker writing until its next
            // cancel check while the new worker truncated the same
            // output — two writers, one file, unpredictable result.
            // The caller must cancel (nativeCancelTranscode) and wait
            // for done, or use a fresh id.
            answer.objVal["ok"] = JsonValue(false);
            answer.objVal["error"] = JsonValue("transcode job '" + jobId + "' is already running");
            answer.objVal["jobId"] = JsonValue(jobId);
            return stdToJstring(env, answer.serialize());
        }
        auto outputOwner = g_activeTranscodeOutputs.find(normalizedOutput);
        if (outputOwner != g_activeTranscodeOutputs.end()) {
            // Same-output concurrency, by path rather than by id: a
            // different jobId writing the same destination is the same
            // two-writer hazard, so it is refused with the owner named.
            answer.objVal["ok"] = JsonValue(false);
            answer.objVal["error"] = JsonValue("output '" + opts.outputPath +
                "' is already being written by transcode job '" + outputOwner->second + "'");
            answer.objVal["jobId"] = JsonValue(jobId);
            return stdToJstring(env, answer.serialize());
        }
        pruneFinishedJobsLocked();
        auto job = std::make_shared<TranscodeJob>();
        job->jobId = jobId;
        job->outputPath = normalizedOutput;
        g_transcodeJobs[jobId] = job;
        g_activeTranscodeOutputs[normalizedOutput] = jobId;
        g_transcodeWorkersActive.fetch_add(1, std::memory_order_relaxed);

        /*
         * The pattern engine/main.cpp wraps in NDJSON, translated one-to-one:
         * a detached worker runs the synchronous Transcoder, streams progress
         * into the record's atomics, and publishes the terminal answer with
         * a release-store of `done`. Nothing here touches the engine context,
         * so playback keeps running — and being scrubbed — underneath.
         */
        std::thread([job, opts]() {
            audio_engine::Transcoder transcoder;
            const audio_engine::TranscodeResult result = transcoder.transcode(
                opts,
                [job](double progress, double currentSec, double durationSec) {
                    double clamped = progress < 0.0 ? 0.0 : (progress > 1.0 ? 1.0 : progress);
                    job->progressBps.store(static_cast<int64_t>(clamped * 10000.0), std::memory_order_relaxed);
                    job->currentTimeMs.store(static_cast<int64_t>(currentSec * 1000.0), std::memory_order_relaxed);
                    job->durationMs.store(static_cast<int64_t>(durationSec * 1000.0), std::memory_order_relaxed);
                },
                job->cancelFlag.get());
            job->ok.store(result.ok, std::memory_order_relaxed);
            job->cancelled.store(result.cancelled, std::memory_order_relaxed);
            job->error = result.error;
            job->running.store(false, std::memory_order_relaxed);
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            job->doneAtMs.store(now, std::memory_order_relaxed);
            // Release the output path before publishing done: a caller
            // polling for done and immediately reusing the destination
            // must never race this worker's last write, and the worker
            // performs no file writes after this point.
            {
                std::lock_guard<std::mutex> lock(g_jobsMutex);
                auto it = g_activeTranscodeOutputs.find(job->outputPath);
                if (it != g_activeTranscodeOutputs.end() && it->second == job->jobId) {
                    g_activeTranscodeOutputs.erase(it);
                }
            }
            job->done.store(true, std::memory_order_release);
            // Last: decrement + notify, so nativeDestroy's bounded wait
            // only returns once the record above is fully published.
            if (g_transcodeWorkersActive.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> workersLock(g_workersMutex);
                g_workersCv.notify_all();
            }
        }).detach();
    }

    answer.objVal["ok"] = JsonValue(true);
    answer.objVal["jobId"] = JsonValue(jobId);
    return stdToJstring(env, answer.serialize());
}

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeCancelTranscode
  (JNIEnv *env, jobject, jstring jobIdStr) {
    const std::string jobId = jstringToStd(env, jobIdStr);
    const std::shared_ptr<TranscodeJob> job = findJob(jobId);
    if (!job || job->done.load(std::memory_order_acquire)) return JNI_FALSE;
    job->cancelFlag->store(true, std::memory_order_relaxed);
    return JNI_TRUE;
}

JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetTranscodeState
  (JNIEnv *env, jobject, jstring jobIdStr) {
    const std::string jobId = jstringToStd(env, jobIdStr);
    const std::shared_ptr<TranscodeJob> job = findJob(jobId);
    if (!job) {
        JsonValue v = JsonValue::object();
        v.objVal["jobId"] = JsonValue(jobId);
        v.objVal["found"] = JsonValue(false);
        v.objVal["running"] = JsonValue(false);
        v.objVal["done"] = JsonValue(false);
        v.objVal["ok"] = JsonValue(false);
        v.objVal["cancelled"] = JsonValue(false);
        v.objVal["progress"] = JsonValue(0.0);
        v.objVal["currentTimeSec"] = JsonValue(0.0);
        v.objVal["durationSec"] = JsonValue(0.0);
        return stdToJstring(env, v.serialize());
    }
    return stdToJstring(env, transcodeStateToJson(*job));
}

JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetTranscodeCapabilities
  (JNIEnv *env, jobject) {
#ifdef HAVE_LIBAV
    const TranscodeCaps caps = probeTranscodeCaps();
#else
    // transcode_caps.cpp is only compiled under HAVE_LIBAV; the fallback
    // answer must be constructible here without touching it.
    const TranscodeCaps caps;
#endif
    JsonValue v = JsonValue::object();
    v.objVal["supported"] = JsonValue(caps.supported);
    JsonValue encoders = JsonValue::object();
    for (const auto& entry : caps.encoders) {
        encoders.objVal[entry.first] = JsonValue(entry.second);
    }
    v.objVal["encoders"] = encoders;
    return stdToJstring(env, v.serialize());
}
