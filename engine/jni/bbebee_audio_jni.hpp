#ifndef BBEBEE_AUDIO_JNI_HPP
#define BBEBEE_AUDIO_JNI_HPP

#include <jni.h>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstring>

#include "../mpv_client.h"
#include "../pcm_ring_buffer.hpp"
#include "../fft.hpp"

namespace bbebee {
namespace jni {

/**
 * Playback state as the JS bridge contract reports it — one JSON object per
 * `nativeGetState`, mirroring the desktop sidecar's `mpvGetState`.
 */
struct EngineState {
    double positionMs = 0.0;
    double durationMs = 0.0;
    // idle | playing | paused | ended
    std::string status = "idle";
    int64_t endedCount = 0;
};

/**
 * The load answer as the JS bridge contract reports it. `resumed` marks the
 * gapless re-bind: the requested file is the one already sounding, and the
 * load must not restart it (a `loadfile replace` of the current file stops
 * the track mid-flight).
 *
 * `sampleRate`/`channels`/`bitDepth` are the decoded output parameters
 * (mpv's `audio-params` properties), best-effort: 0 means "unknown yet" and
 * the JSON serializes the field away, which the contract spells as optional.
 */
struct LoadResult {
    bool ok = false;
    double durationMs = 0.0;
    bool resumed = false;
    int64_t endedCount = 0;
    int sampleRate = 0;
    int channels = 0;
    int bitDepth = 0;
};

class AudioEngineContext {
public:
    AudioEngineContext();
    ~AudioEngineContext();

    bool initialize(const std::string& configDir);
    void destroy();

    bool load(const std::string& uri);
    LoadResult loadEx(const std::string& uri, const std::string& headerFields);
    bool append(const std::string& uri, const std::string& headerFields);
    void play();
    void playAt(double atMs, bool hasAt);
    void pause();
    void stop();
    void seekMs(double ms);
    void setVolume(double volume01);
    void setMuted(bool muted);
    void setDspConfig(const std::string& af,
                      const std::string& replaygain,
                      bool replaygainClip,
                      const std::string& replaygainPreamp,
                      const std::string& replaygainFallback);
    void setVisualizer(bool enabled, int fftSize);
    void setOutputDevice(const std::string& device);

    bool getSpectrum(uint8_t* outFreqData, uint8_t* outTimeData, size_t binCount, size_t timeCount);
    audio_engine::RingBufferStats getRingBufferStats();
    EngineState getState();
    /** How many frequency bins one spectrum frame carries (fftSize / 2). */
    int getFftBinCount();
    bool isMpvAvailable() const { return mpvHandle_ != nullptr; }
    bool isPcmTapAvailable() const { return pcmTapRegistered_.load(); }

    void onPcmChunk(const float* samples, int frames, int channels, int sampleRate);

private:
    /**
     * Reconfigure the ring buffer when the tap's format differs from what
     * the buffer was configured for. Called on the module (JS) thread under
     * engineMutex_ — never on the audio thread, where `configure` would
     * stall the callback. Until this runs, the buffer's format guard drops
     * every tapped frame (a 48 kHz track on a 44.1 kHz-configured buffer
     * otherwise renders the visualizer permanently flat).
     */
    void syncRingBufferFormatLocked();

    /** Read a string property; empty when the property has no string value. */
    std::string getStringProp(const char* name);
    /** Read a numeric property via its string form; def when absent. */
    double getDoubleProp(const char* name, double def);

    mpv_handle* mpvHandle_{ nullptr };
    std::unique_ptr<audio_engine::PcmRingBuffer> ringBuffer_;
    std::unique_ptr<audio_engine::FftProcessor> fftProcessor_;
    std::atomic<bool> pcmTapRegistered_{ false };
    std::atomic<bool> isPlaying_{ false };
    std::mutex engineMutex_;
    std::vector<float> readPcmBuffer_;
    /* Rising-edge counter of eof transitions, for the JS rollover detector. */
    int64_t endedCount_{ 0 };
    bool lastEof_{ false };
    /* The tap's observed format, published from the audio thread; the
     * module thread reconfigures the ring buffer when it differs. */
    std::atomic<int> tapSampleRate_{ 0 };
    std::atomic<int> tapChannels_{ 0 };
};

} // namespace jni
} // namespace bbebee

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The JNI surface of org.bbebee.audio.BBeBeeAudioEngine.
 *
 * One method per entry in the `bbebeeMobileMpv` contract table
 * (B_Be_Bee/docs/audio/engine.md, "The native binding contract"); the JS
 * adapter in apps/mobile/modules/audio-engine maps between the two shapes.
 */

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeInit
  (JNIEnv *, jobject, jstring);

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeDestroy
  (JNIEnv *, jobject);

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeLoad
  (JNIEnv *, jobject, jstring);

/** Full load: returns `{ok, durationMs, resumed, endedCount}` as JSON. */
JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeLoadEx
  (JNIEnv *, jobject, jstring, jstring);

/** Gapless append (`loadfile append`). */
JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeAppend
  (JNIEnv *, jobject, jstring, jstring);

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePlay
  (JNIEnv *, jobject);

/** Play with an optional position in ms (NaN = keep the engine position). */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePlayAt
  (JNIEnv *, jobject, jdouble);

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePause
  (JNIEnv *, jobject);

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeStop
  (JNIEnv *, jobject);

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSeekMs
  (JNIEnv *, jobject, jdouble);

/** Volume 0..1 — mpv's own 0..100 scale is an engine-internal detail. */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetVolume
  (JNIEnv *, jobject, jdouble);

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetMuted
  (JNIEnv *, jobject, jboolean);

/**
 * Native DSP chain: the `af` lavfi fragment plus the ReplayGain knobs, the
 * same payload `dsp/af-changed` carries on desktop.
 */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetDspConfig
  (JNIEnv *, jobject, jstring, jstring, jboolean, jstring, jstring);

/** Enable the PCM tap consumer and size its FFT (0 keeps the current size). */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetVisualizer
  (JNIEnv *, jobject, jboolean, jint);

JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetOutputDevice
  (JNIEnv *, jobject, jstring);

/** No-op on Android (degraded-path flag the desktop engine knows; RNAA's own
 *  graph needs no signal). Kept so the JS adapter needs no platform branch. */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetStreamPlayback
  (JNIEnv *, jobject, jboolean);

/** No-op on Android (WASAPI exclusive mode is a desktop concept). */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetAudioExclusive
  (JNIEnv *, jobject, jboolean);

JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetSpectrum
  (JNIEnv *, jobject, jbyteArray, jbyteArray);

JNIEXPORT jlongArray JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetRingBufferStats
  (JNIEnv *, jobject);

/** `{positionMs, durationMs, status, endedCount}` as JSON. */
JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetState
  (JNIEnv *, jobject);

/** How many frequency bins one spectrum frame carries (fftSize / 2). */
JNIEXPORT jint JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetFftBinCount
  (JNIEnv *, jobject);

JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetEngineStatus
  (JNIEnv *, jobject);

/*
 * Transcoder exports. These are codec-side, not part of the `bbebeeMobileMpv`
 * playback contract: they deliberately do NOT take g_contextMutex (a running
 * transcode must never block playback control), they report progress through
 * atomics read by `nativeGetTranscodeState` — the same pull model every
 * other answer on this bridge uses — and cancellation is the cooperative
 * atomic flag the engine's NDJSON `cancelTranscode` action uses.
 *
 * URIs: real filesystem paths, with an optional `file://` prefix (stripped
 * before use). `content://` URIs are resolved to paths by the Kotlin layer
 * before reaching here, mirroring how the desktop shell resolves URIs for
 * the sidecar.
 */

/**
 * Start a transcode job on a detached worker thread. Answers
 * `{ok: true, jobId}` once registered (not when finished) or
 * `{ok: false, error}` synchronously. The job then surfaces through
 * `nativeGetTranscodeState` until done.
 */
JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeTranscode
  (JNIEnv *, jobject, jstring /*jobId*/, jstring /*input*/, jstring /*output*/,
   jstring /*format*/, jstring /*bitrate*/, jint /*sampleRate*/, jint /*channels*/,
   jdouble /*startSec*/, jdouble /*endSec*/);

/** Set the cooperative cancel flag for a running job. True when found. */
JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeCancelTranscode
  (JNIEnv *, jobject, jstring /*jobId*/);

/**
 * `{jobId, found, running, done, ok, cancelled, progress, currentTimeSec,
 * durationSec, error}` as JSON. Finished jobs stay retrievable for a while
 * (bounded registry) so a poll that races completion still sees the result.
 */
JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetTranscodeState
  (JNIEnv *, jobject, jstring /*jobId*/);

/**
 * `{supported, encoders: {<encoder id>: <bool>}}` — which encoders the
 * linked libavcodec actually carries, keyed by the desktop capability ids.
 */
JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetTranscodeCapabilities
  (JNIEnv *, jobject);

#ifdef __cplusplus
}
#endif

#endif // BBEBEE_AUDIO_JNI_HPP
