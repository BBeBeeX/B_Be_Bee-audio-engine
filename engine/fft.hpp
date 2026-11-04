#ifndef FFT_HPP
#define FFT_HPP

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <utility>

namespace audio_engine {

struct FftOptions {
    int fftSize = 128;
    float minDecibels = -100.0f;
    float maxDecibels = -30.0f;
    float smoothingTimeConstant = 0.8f;
};

class FftProcessor {
public:
    using Options = FftOptions;

    FftProcessor() : opts() {
        initBuffers();
    }

    explicit FftProcessor(const FftOptions& options) : opts(options) {
        initBuffers();
    }

    void setFftSize(int newSize) {
        if (newSize < 16) newSize = 16;
        if (newSize > 4096) newSize = 4096;
        // Ensure power of 2
        int p = 1;
        while (p < newSize) p <<= 1;
        opts.fftSize = p;
        initBuffers();
    }

    int getFftSize() const { return opts.fftSize; }
    int getFrequencyBinCount() const { return opts.fftSize / 2; }

    void setMinDecibels(float minDb) { opts.minDecibels = minDb; }
    float getMinDecibels() const { return opts.minDecibels; }

    void setMaxDecibels(float maxDb) { opts.maxDecibels = maxDb; }
    float getMaxDecibels() const { return opts.maxDecibels; }

    void setSmoothingTimeConstant(float smoothing) {
        opts.smoothingTimeConstant = std::clamp(smoothing, 0.0f, 1.0f);
    }
    float getSmoothingTimeConstant() const { return opts.smoothingTimeConstant; }

    void reset() {
        const int N = opts.fftSize;
        const int binCount = N / 2;
        previousSpectrum.assign(binCount, 0.0f);
        frequencyData.assign(binCount, 0);
        timeDomainData.assign(N, 128);
        previousSpectrumLeft.assign(binCount, 0.0f);
        previousSpectrumRight.assign(binCount, 0.0f);
        leftFrequencyData.assign(binCount, 0);
        rightFrequencyData.assign(binCount, 0);
        leftTimeDomainData.assign(N, 128);
        rightTimeDomainData.assign(N, 128);
    }

    const std::vector<uint8_t>& getFrequencyData() const { return frequencyData; }
    const std::vector<uint8_t>& getTimeDomainData() const { return timeDomainData; }
    const std::vector<uint8_t>& getLeftFrequencyData() const { return leftFrequencyData; }
    const std::vector<uint8_t>& getRightFrequencyData() const { return rightFrequencyData; }
    const std::vector<uint8_t>& getLeftTimeDomainData() const { return leftTimeDomainData; }
    const std::vector<uint8_t>& getRightTimeDomainData() const { return rightTimeDomainData; }

    /** Channel count of the most recent process call (0 = none yet). */
    int getChannels() const { return lastChannels; }
    /** True only when the most recent frame actually carried stereo. */
    bool hasStereo() const { return lastChannels >= 2; }

    /**
     * Sample rate of the stream feeding this processor, set by the
     * caller that knows it (the engine's visualizer loop, from the
     * PCM tap format). 0 = unknown. This processor never infers it
     * from the FFT size — a rate is a stream property, not an FFT
     * property — so callers that do not know it must leave it 0
     * rather than guessing 44100 or 48000.
     */
    void setSampleRateHz(int hz) { sampleRateHz = hz > 0 ? hz : 0; }
    int getSampleRateHz() const { return sampleRateHz; }

    /**
     * Process interleaved multi-channel PCM audio.
     * Produces the mono mix exactly as before (downmix, Hann window,
     * Radix-2 FFT, dB conversion, temporal smoothing) and, when the
     * input has at least two channels, the same analysis for channels
     * 0 and 1 individually. For >2 channels the mix still covers all
     * channels; the per-channel outputs are channels 0 and 1 only —
     * a surround meter is a separate feature, not a silent extension
     * of this one. When the input is mono, the per-channel outputs
     * are cleared to silence so a mono frame can never be mistaken
     * for a stereo frame downstream.
     * Zero heap allocations during steady state.
     *
     * @param pcm Interleaved float32 audio samples [-1.0, 1.0]
     * @param frames Number of audio frames (samples per channel)
     * @param channels Channel count (1=mono, 2=stereo, >2 multi-channel)
     */
    void processInterleaved(
        const float* pcm,
        size_t frames,
        int channels
    ) {
        const int N = opts.fftSize;
        const int binCount = N / 2;

        if (channels > 0) lastChannels = channels;

        const size_t take = (pcm && frames > 0 && channels > 0)
            ? std::min(frames, static_cast<size_t>(N))
            : 0;

        // 1. Downmix interleaved PCM into monoBuffer
        if (take > 0 && pcm != nullptr) {
            if (channels == 1) {
                for (size_t i = 0; i < take; ++i) {
                    monoBuffer[i] = pcm[i];
                }
            } else if (channels == 2) {
                for (size_t i = 0; i < take; ++i) {
                    monoBuffer[i] = 0.5f * (pcm[2 * i] + pcm[2 * i + 1]);
                }
            } else {
                const float invCh = 1.0f / static_cast<float>(channels);
                for (size_t i = 0; i < take; ++i) {
                    float sum = 0.0f;
                    const size_t offset = i * static_cast<size_t>(channels);
                    for (int c = 0; c < channels; ++c) {
                        sum += pcm[offset + static_cast<size_t>(c)];
                    }
                    monoBuffer[i] = sum * invCh;
                }
            }
        }

        // 2. Zero-padding for remaining samples to prevent residue from previous frames
        for (size_t i = take; i < static_cast<size_t>(N); ++i) {
            monoBuffer[i] = 0.0f;
        }

        // 2b. Per-channel extraction (channels 0 and 1 only, see header).
        if (channels >= 2) {
            if (take > 0 && pcm != nullptr) {
                for (size_t i = 0; i < take; ++i) {
                    leftBuffer[i] = pcm[i * static_cast<size_t>(channels) + 0];
                    rightBuffer[i] = pcm[i * static_cast<size_t>(channels) + 1];
                }
            }
            for (size_t i = take; i < static_cast<size_t>(N); ++i) {
                leftBuffer[i] = 0.0f;
                rightBuffer[i] = 0.0f;
            }
        }

        // 3-6. Mono mix (bit-for-bit the previous behaviour), then the
        // per-channel analyses through the same steps. The shared
        // fftBuffer is reused sequentially, never concurrently.
        runChannel(monoBuffer.data(), take, frequencyData, timeDomainData, previousSpectrum);
        if (channels >= 2) {
            runChannel(leftBuffer.data(), take, leftFrequencyData, leftTimeDomainData, previousSpectrumLeft);
            runChannel(rightBuffer.data(), take, rightFrequencyData, rightTimeDomainData, previousSpectrumRight);
        } else {
            leftFrequencyData.assign(binCount, 0);
            rightFrequencyData.assign(binCount, 0);
            leftTimeDomainData.assign(N, 128);
            rightTimeDomainData.assign(N, 128);
        }
    }

    /**
     * Interleaved multi-channel processing interface (sample-count based).
     * If channels > 1, sampleCount represents total float samples (frames * channels).
     * Defaults to mono (channels = 1) for backward compatibility.
     */
    void process(
        const float* pcmSamples,
        size_t sampleCount,
        int channels = 1
    ) {
        if (channels <= 0) channels = 1;
        const size_t frames = sampleCount / static_cast<size_t>(channels);
        processInterleaved(pcmSamples, frames, channels);
    }

private:
    Options opts;
    std::vector<float> window;
    std::vector<float> previousSpectrum;
    std::vector<float> monoBuffer;
    std::vector<std::complex<float>> fftBuffer;
    std::vector<uint8_t> frequencyData;
    std::vector<uint8_t> timeDomainData;
    // Per-channel state (channels 0 and 1). The FFT working buffer is
    // shared and reused sequentially by runChannel, never held across
    // calls, so one fftBuffer serves all three analyses.
    std::vector<float> leftBuffer;
    std::vector<float> rightBuffer;
    std::vector<float> previousSpectrumLeft;
    std::vector<float> previousSpectrumRight;
    std::vector<uint8_t> leftFrequencyData;
    std::vector<uint8_t> rightFrequencyData;
    std::vector<uint8_t> leftTimeDomainData;
    std::vector<uint8_t> rightTimeDomainData;
    int lastChannels = 0;
    int sampleRateHz = 0;

    void initBuffers() {
        const int N = opts.fftSize;
        const int binCount = N / 2;

        window.resize(N);
        previousSpectrum.assign(binCount, 0.0f);
        monoBuffer.assign(N, 0.0f);
        fftBuffer.assign(N, std::complex<float>(0.0f, 0.0f));
        frequencyData.assign(binCount, 0);
        timeDomainData.assign(N, 128);
        leftBuffer.assign(N, 0.0f);
        rightBuffer.assign(N, 0.0f);
        previousSpectrumLeft.assign(binCount, 0.0f);
        previousSpectrumRight.assign(binCount, 0.0f);
        leftFrequencyData.assign(binCount, 0);
        rightFrequencyData.assign(binCount, 0);
        leftTimeDomainData.assign(N, 128);
        rightTimeDomainData.assign(N, 128);

        constexpr float PI = 3.14159265358979323846f;
        for (int i = 0; i < N; ++i) {
            window[i] = 0.5f * (1.0f - std::cos(2.0f * PI * static_cast<float>(i) / static_cast<float>(N - 1)));
        }
    }

    /**
     * One channel's analysis: time-domain byte conversion, Hann
     * window, Radix-2 FFT, dB conversion, temporal smoothing. This is
     * exactly the mono pipeline that used to live inline in
     * processInterleaved; extracting it is what lets the mono mix and
     * the two channels share one definition instead of three copies
     * that could drift. `samples` is size N, zero-padded beyond
     * `take`; samples beyond `take` still read as silence (128) in
     * the time-domain output, matching the old behaviour, not 127.
     */
    void runChannel(
        const float* samples,
        size_t take,
        std::vector<uint8_t>& outFreq,
        std::vector<uint8_t>& outTime,
        std::vector<float>& prevSpectrum
    ) {
        const int N = opts.fftSize;
        const int binCount = N / 2;

        // Time domain data conversion: [-1.0, 1.0] -> [0, 255], midpoint 128
        for (size_t i = 0; i < take; ++i) {
            float s = samples[i];
            float norm = (s + 1.0f) * 127.5f;
            outTime[i] = static_cast<uint8_t>(std::clamp(norm, 0.0f, 255.0f));
        }
        for (size_t i = take; i < static_cast<size_t>(N); ++i) {
            outTime[i] = 128;
        }

        // Apply Hann window and populate fftBuffer
        for (int i = 0; i < N; ++i) {
            fftBuffer[i] = std::complex<float>(samples[i] * window[i], 0.0f);
        }

        // Cooley-Tukey Radix-2 FFT
        cooleyTukey(fftBuffer);

        // Calculate magnitude, dB conversion, and temporal smoothing
        float dbRange = opts.maxDecibels - opts.minDecibels;
        if (dbRange <= 0.0f) dbRange = 1.0f;

        for (int i = 0; i < binCount; ++i) {
            // Normalized magnitude: DC uses N, AC bins use N/2
            float normFactor = (i == 0) ? static_cast<float>(N) : static_cast<float>(N / 2.0f);
            float mag = std::abs(fftBuffer[i]) / normFactor;
            if (std::isnan(mag) || std::isinf(mag) || mag < 0.0f) {
                mag = 0.0f;
            }

            // Safe dB conversion
            float db = 20.0f * std::log10(mag + 1e-6f);
            float normalized = ((db - opts.minDecibels) / dbRange) * 255.0f;
            float clamped = std::clamp(normalized, 0.0f, 255.0f);

            // Temporal exponential smoothing
            float smoothed = opts.smoothingTimeConstant * prevSpectrum[i] +
                             (1.0f - opts.smoothingTimeConstant) * clamped;
            if (std::isnan(smoothed) || std::isinf(smoothed)) {
                smoothed = 0.0f;
            }
            prevSpectrum[i] = smoothed;
            outFreq[i] = static_cast<uint8_t>(std::clamp(std::round(smoothed), 0.0f, 255.0f));
        }
    }

    void cooleyTukey(std::vector<std::complex<float>>& a) {
        int n = static_cast<int>(a.size());
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) {
                j ^= bit;
            }
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }

        constexpr float PI = 3.14159265358979323846f;
        for (int len = 2; len <= n; len <<= 1) {
            float ang = -2.0f * PI / len;
            std::complex<float> wlen(std::cos(ang), std::sin(ang));
            for (int i = 0; i < n; i += len) {
                std::complex<float> w(1.0f, 0.0f);
                for (int j = 0; j < len / 2; ++j) {
                    std::complex<float> u = a[i + j];
                    std::complex<float> v = a[i + j + len / 2] * w;
                    a[i + j] = u + v;
                    a[i + j + len / 2] = u - v;
                    w *= wlen;
                }
            }
        }
    }
};

} // namespace audio_engine

#endif // FFT_HPP
