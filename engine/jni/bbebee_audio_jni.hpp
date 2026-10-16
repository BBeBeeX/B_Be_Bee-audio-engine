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

class AudioEngineContext {
public:
    AudioEngineContext();
    ~AudioEngineContext();

    bool initialize(const std::string& configDir);
    void destroy();

    bool load(const std::string& uri);
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    void setVolume(double volume);

    bool getSpectrum(uint8_t* outFreqData, uint8_t* outTimeData, size_t binCount, size_t timeCount);
    audio_engine::RingBufferStats getRingBufferStats();
    bool isMpvAvailable() const { return mpvHandle_ != nullptr; }
    bool isPcmTapAvailable() const { return pcmTapRegistered_.load(); }

    void onPcmChunk(const float* samples, int frames, int channels, int sampleRate);

private:
    mpv_handle* mpvHandle_{ nullptr };
    std::unique_ptr<audio_engine::PcmRingBuffer> ringBuffer_;
    std::unique_ptr<audio_engine::FftProcessor> fftProcessor_;
    std::atomic<bool> pcmTapRegistered_{ false };
    std::atomic<bool> isPlaying_{ false };
    std::mutex engineMutex_;
    std::vector<float> readPcmBuffer_;
};

} // namespace jni
} // namespace bbebee

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativeInit
 * Signature: (Ljava/lang/String;)Z
 */
JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeInit
  (JNIEnv *, jobject, jstring);

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativeDestroy
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeDestroy
  (JNIEnv *, jobject);

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativeLoad
 * Signature: (Ljava/lang/String;)Z
 */
JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeLoad
  (JNIEnv *, jobject, jstring);

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativePlay
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePlay
  (JNIEnv *, jobject);

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativePause
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativePause
  (JNIEnv *, jobject);

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativeStop
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeStop
  (JNIEnv *, jobject);

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativeSeek
 * Signature: (D)V
 */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSeek
  (JNIEnv *, jobject, jdouble);

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativeSetVolume
 * Signature: (D)V
 */
JNIEXPORT void JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeSetVolume
  (JNIEnv *, jobject, jdouble);

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativeGetSpectrum
 * Signature: ([B[B)Z
 */
JNIEXPORT jboolean JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetSpectrum
  (JNIEnv *, jobject, jbyteArray, jbyteArray);

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativeGetRingBufferStats
 * Signature: ()[J
 */
JNIEXPORT jlongArray JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetRingBufferStats
  (JNIEnv *, jobject);

/*
 * Class:     org_bbebee_audio_BBeBeeAudioEngine
 * Method:    nativeGetEngineStatus
 * Signature: ()Ljava/lang/String;
 */
JNIEXPORT jstring JNICALL Java_org_bbebee_audio_BBeBeeAudioEngine_nativeGetEngineStatus
  (JNIEnv *, jobject);

#ifdef __cplusplus
}
#endif

#endif // BBEBEE_AUDIO_JNI_HPP
