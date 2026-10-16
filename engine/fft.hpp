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
    }

    const std::vector<uint8_t>& getFrequencyData() const { return frequencyData; }
    const std::vector<uint8_t>& getTimeDomainData() const { return timeDomainData; }

    /**
     * Process interleaved multi-channel PCM audio.
     * Downmixes to mono, applies Hann window, executes Radix-2 FFT, and produces
     * both frequency spectrum and real time-domain waveform data into internal buffers.
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

        // 3. Time domain data conversion: [-1.0, 1.0] -> [0, 255], midpoint 128
        // N samples output to match frontend protocol requirements
        for (size_t i = 0; i < take; ++i) {
            float s = monoBuffer[i];
            float norm = (s + 1.0f) * 127.5f;
            timeDomainData[i] = static_cast<uint8_t>(std::clamp(norm, 0.0f, 255.0f));
        }
        for (size_t i = take; i < static_cast<size_t>(N); ++i) {
            timeDomainData[i] = 128;
        }

        // 4. Apply Hann window and populate fftBuffer
        for (int i = 0; i < N; ++i) {
            fftBuffer[i] = std::complex<float>(monoBuffer[i] * window[i], 0.0f);
        }

        // 5. Cooley-Tukey Radix-2 FFT
        cooleyTukey(fftBuffer);

        // 6. Calculate magnitude, dB conversion, and temporal smoothing
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
            float smoothed = opts.smoothingTimeConstant * previousSpectrum[i] +
                             (1.0f - opts.smoothingTimeConstant) * clamped;
            if (std::isnan(smoothed) || std::isinf(smoothed)) {
                smoothed = 0.0f;
            }
            previousSpectrum[i] = smoothed;
            frequencyData[i] = static_cast<uint8_t>(std::clamp(std::round(smoothed), 0.0f, 255.0f));
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

    void initBuffers() {
        const int N = opts.fftSize;
        const int binCount = N / 2;

        window.resize(N);
        previousSpectrum.assign(binCount, 0.0f);
        monoBuffer.assign(N, 0.0f);
        fftBuffer.assign(N, std::complex<float>(0.0f, 0.0f));
        frequencyData.assign(binCount, 0);
        timeDomainData.assign(N, 128);

        constexpr float PI = 3.14159265358979323846f;
        for (int i = 0; i < N; ++i) {
            window[i] = 0.5f * (1.0f - std::cos(2.0f * PI * static_cast<float>(i) / static_cast<float>(N - 1)));
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
