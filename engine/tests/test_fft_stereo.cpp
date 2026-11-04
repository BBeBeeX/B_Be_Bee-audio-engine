// Standalone check for the per-channel FFT path in fft.hpp.
// Compile: g++ -std=c++17 -O2 -Iengine engine/tests/test_fft_stereo.cpp -o /tmp/test_fft_stereo
// Run: /tmp/test_fft_stereo
// No libmpv, no test framework: plain asserts, exit code is the verdict.
#include "../fft.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace audio_engine;

static int argmax(const std::vector<uint8_t>& v) {
    int best = 0;
    for (size_t i = 1; i < v.size(); ++i) {
        if (v[i] > v[static_cast<size_t>(best)]) best = static_cast<int>(i);
    }
    return best;
}

int main() {
    // 1. Stereo: left carries a 1 kHz sine, right is silent.
    // fftSize 128 at 48 kHz -> bin width 375 Hz, so 1 kHz lands near bin 3.
    {
        FftProcessor fft;
        fft.setSmoothingTimeConstant(0.0f); // no smoothing: one frame is the verdict
        fft.setSampleRateHz(48000);
        const int N = 128;
        std::vector<float> pcm(static_cast<size_t>(N) * 2, 0.0f);
        // 0.05 amplitude: below the -30 dB ceiling, so the mono mix
        // (0.025) reads strictly lower than the left channel (0.05).
        // At 0.5 both would saturate at 255 and prove nothing.
        for (int i = 0; i < N; ++i) {
            pcm[static_cast<size_t>(i) * 2 + 0] = 0.05f * std::sin(2.0f * 3.14159265f * 1000.0f * i / 48000.0f);
            pcm[static_cast<size_t>(i) * 2 + 1] = 0.0f;
        }
        fft.processInterleaved(pcm.data(), N, 2);
        assert(fft.getChannels() == 2);
        assert(fft.hasStereo());
        assert(fft.getSampleRateHz() == 48000);
        const int leftPeak = argmax(fft.getLeftFrequencyData());
        assert(leftPeak >= 2 && leftPeak <= 4);
        assert(fft.getLeftFrequencyData()[static_cast<size_t>(leftPeak)] > 100);
        // Right is silence: every bin at the floor.
        for (uint8_t v : fft.getRightFrequencyData()) assert(v == 0);
        // Mono mix is half amplitude (0.25 sine): still peaks at the same bin, lower.
        const int monoPeak = argmax(fft.getFrequencyData());
        assert(monoPeak == leftPeak);
        assert(fft.getFrequencyData()[static_cast<size_t>(monoPeak)] > 50);
        assert(fft.getFrequencyData()[static_cast<size_t>(monoPeak)] <
               fft.getLeftFrequencyData()[static_cast<size_t>(leftPeak)]);
        // Time domain: left follows the sine, right sits at the
        // zero-sample value 127 (truncated (0+1)*127.5; 128 is only
        // the beyond-take fill, not a zero sample).
        assert(fft.getLeftTimeDomainData()[0] == 127); // sin(0) = 0
        assert(fft.getLeftTimeDomainData()[12] > 128);
        for (uint8_t v : fft.getRightTimeDomainData()) assert(v == 127);
        std::printf("stereo: left peak bin %d, mono peak bin %d — ok\n", leftPeak, monoPeak);
    }

    // 2. Mono input must never serve stereo data, even after a stereo frame.
    {
        FftProcessor fft;
        fft.setSmoothingTimeConstant(0.0f);
        std::vector<float> stereo(256, 0.5f);
        fft.processInterleaved(stereo.data(), 128, 2);
        assert(fft.hasStereo());
        std::vector<float> mono(128, 0.5f);
        fft.processInterleaved(mono.data(), 128, 1);
        assert(!fft.hasStereo());
        assert(fft.getChannels() == 1);
        for (uint8_t v : fft.getLeftFrequencyData()) assert(v == 0);
        for (uint8_t v : fft.getRightFrequencyData()) assert(v == 0);
        for (uint8_t v : fft.getLeftTimeDomainData()) assert(v == 128);
        for (uint8_t v : fft.getRightTimeDomainData()) assert(v == 128);
        std::printf("mono-after-stereo clears per-channel outputs — ok\n");
    }

    // 3. >2 channels: mix covers all channels, per-channel outputs are 0/1.
    {
        FftProcessor fft;
        fft.setSmoothingTimeConstant(0.0f);
        std::vector<float> quad(128 * 4, 0.0f);
        for (int i = 0; i < 128; ++i) {
            quad[static_cast<size_t>(i) * 4 + 0] = 0.5f;
            quad[static_cast<size_t>(i) * 4 + 1] = -0.5f;
            quad[static_cast<size_t>(i) * 4 + 2] = 0.5f;
            quad[static_cast<size_t>(i) * 4 + 3] = -0.5f;
        }
        fft.processInterleaved(quad.data(), 128, 4);
        assert(fft.hasStereo()); // >= 2 channels is a stereo frame
        assert(fft.getLeftTimeDomainData()[0] == 191); // (0.5+1)*127.5
        assert(fft.getRightTimeDomainData()[0] < 70);  // -0.5 -> ~63
        std::printf("quad: left[0]=%d right[0]=%d — ok\n",
                    fft.getLeftTimeDomainData()[0], fft.getRightTimeDomainData()[0]);
    }

    // 4. Sample rate: unknown stays 0, never guessed.
    {
        FftProcessor fft;
        assert(fft.getSampleRateHz() == 0);
        fft.setSampleRateHz(-44100);
        assert(fft.getSampleRateHz() == 0);
        std::printf("sample rate unknown stays 0 — ok\n");
    }

    std::printf("all stereo checks passed\n");
    return 0;
}
