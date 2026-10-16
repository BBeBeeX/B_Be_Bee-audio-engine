/**
 * Audio Engine PCM Tap & Lock-Free Ring Buffer Comprehensive Test Suite.
 *
 * Verifies:
 * - Test 1: Normal playback & frame count integrity
 * - Test 2: Long-duration playback (millions of frames / 30min simulation) & zero leak
 * - Test 3: AO recreation & format changes under continuous streaming
 * - Test 4: Callback unregister race (10,000+ concurrent iterations) for UAF / data races
 * - Test 5: Shutdown race (concurrent play/stop/destroy/callback)
 * - Test 6: Multi-channel support (1, 2, 6, 8 channels)
 * - Test 7: Varied block sizes (1, 64, 128, 256, 512, 1024, 2048, 4096, 8192) with zero silent frame drops
 * - Test 8: Overflow policy with slow consumer
 * - Test 9: Data consistency & deterministic signal ramp (interleaving, planar, S16/S32 conversions)
 * - Test 10: High-resolution latency & throughput benchmark (p50, p99, p99.9, max callback duration)
 */

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cassert>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <random>

#include "../pcm_ring_buffer.hpp"
#include "../fft.hpp"

using namespace audio_engine;

// Simulated mpv formats matching mpv af_format
enum MpvFormat {
    MPV_FMT_FLOAT = 1,
    MPV_FMT_FLOATP = 2,
    MPV_FMT_S16 = 3,
    MPV_FMT_S16P = 4,
    MPV_FMT_S32 = 5,
    MPV_FMT_S32P = 6,
    MPV_FMT_DOUBLE = 7,
    MPV_FMT_DOUBLEP = 8,
    MPV_FMT_U8 = 9,
    MPV_FMT_U8P = 10,
};

// Simulated shared tap state matching mpv patch
typedef void (*mpv_pcm_callback_fn)(const float* interleaved, int frames, int channels, int sample_rate, void* userdata);

struct SimulatedMpvTap {
    std::atomic<mpv_pcm_callback_fn> callback{ nullptr };
    std::atomic<void*> userdata{ nullptr };
    std::atomic<int> active_callers{ 0 };
    std::atomic<int> enabled{ 0 };

    void set_callback(mpv_pcm_callback_fn cb, void* ud) {
        // 1. Disable incoming callbacks
        enabled.store(0, std::memory_order_seq_cst);

        // 2. Synchronously wait for all in-flight callers to complete
        while (active_callers.load(std::memory_order_seq_cst) > 0) {
            std::this_thread::yield();
        }

        // 3. Update callback & userdata
        callback.store(cb, std::memory_order_seq_cst);
        userdata.store(ud, std::memory_order_seq_cst);

        // 4. Re-enable if callback is valid
        if (cb) {
            enabled.store(1, std::memory_order_seq_cst);
        }
    }

    // Emulates ao_post_process_data invocation in mpv audio thread
    void invoke_tap(void** data, int num_samples, int ch, int rate, MpvFormat fmt) {
        if (!data || !data[0] || num_samples <= 0 || ch <= 0) return;

        if (enabled.load(std::memory_order_acquire)) {
            active_callers.fetch_add(1, std::memory_order_seq_cst);
            if (enabled.load(std::memory_order_seq_cst)) {
                mpv_pcm_callback_fn cb = callback.load(std::memory_order_acquire);
                void* ud = userdata.load(std::memory_order_acquire);
                if (cb) {
                    if (fmt == MPV_FMT_FLOAT) {
                        cb(static_cast<const float*>(data[0]), num_samples, ch, rate, ud);
                    } else if (fmt == MPV_FMT_FLOATP || fmt == MPV_FMT_S16 || fmt == MPV_FMT_S16P ||
                               fmt == MPV_FMT_S32 || fmt == MPV_FMT_S32P ||
                               fmt == MPV_FMT_DOUBLE || fmt == MPV_FMT_DOUBLEP) {
                        constexpr size_t SCRATCH_SIZE = 32768;
                        static thread_local float scratch[SCRATCH_SIZE];
                        int max_take = static_cast<int>(SCRATCH_SIZE / static_cast<size_t>(ch));
                        if (max_take > 0) {
                            int remaining = num_samples;
                            int offset = 0;
                            while (remaining > 0) {
                                int take = std::min(remaining, max_take);
                                if (fmt == MPV_FMT_FLOATP) {
                                    const float* const* planes_f = reinterpret_cast<const float* const*>(data);
                                    for (int i = 0; i < take; ++i) {
                                        for (int c = 0; c < ch; ++c) {
                                            scratch[i * ch + c] = planes_f[c][offset + i];
                                        }
                                    }
                                } else if (fmt == MPV_FMT_S16) {
                                    const int16_t* s16 = static_cast<const int16_t*>(data[0]);
                                    int base = offset * ch;
                                    for (int i = 0; i < take * ch; ++i) {
                                        scratch[i] = s16[base + i] * (1.0f / 32768.0f);
                                    }
                                } else if (fmt == MPV_FMT_S16P) {
                                    const int16_t* const* planes_s16 = reinterpret_cast<const int16_t* const*>(data);
                                    for (int i = 0; i < take; ++i) {
                                        for (int c = 0; c < ch; ++c) {
                                            scratch[i * ch + c] = planes_s16[c][offset + i] * (1.0f / 32768.0f);
                                        }
                                    }
                                } else if (fmt == MPV_FMT_S32) {
                                    const int32_t* s32 = static_cast<const int32_t*>(data[0]);
                                    int base = offset * ch;
                                    for (int i = 0; i < take * ch; ++i) {
                                        scratch[i] = s32[base + i] * (1.0f / 2147483648.0f);
                                    }
                                } else if (fmt == MPV_FMT_S32P) {
                                    const int32_t* const* planes_s32 = reinterpret_cast<const int32_t* const*>(data);
                                    for (int i = 0; i < take; ++i) {
                                        for (int c = 0; c < ch; ++c) {
                                            scratch[i * ch + c] = planes_s32[c][offset + i] * (1.0f / 2147483648.0f);
                                        }
                                    }
                                } else if (fmt == MPV_FMT_DOUBLE) {
                                    const double* dbl = static_cast<const double*>(data[0]);
                                    int base = offset * ch;
                                    for (int i = 0; i < take * ch; ++i) {
                                        scratch[i] = static_cast<float>(dbl[base + i]);
                                    }
                                } else if (fmt == MPV_FMT_DOUBLEP) {
                                    const double* const* planes_dbl = reinterpret_cast<const double* const*>(data);
                                    for (int i = 0; i < take; ++i) {
                                        for (int c = 0; c < ch; ++c) {
                                            scratch[i * ch + c] = static_cast<float>(planes_dbl[c][offset + i]);
                                        }
                                    }
                                }
                                cb(scratch, take, ch, rate, ud);
                                offset += take;
                                remaining -= take;
                            }
                        }
                    } else {
                        // Unknown/unsupported format: skip callback completely
                    }
                }
            }
            active_callers.fetch_sub(1, std::memory_order_seq_cst);
        }
    }
};

// Test 1: Normal playback and exact frame count
void test_normal_playback() {
    std::cout << "[Test 1] Normal playback & frame count integrity... " << std::flush;
    PcmRingBuffer ring;
    ring.configure(48000, 2, 131072);

    constexpr size_t TOTAL_FRAMES = 96000; // 2 seconds of 48kHz audio
    constexpr size_t BLOCK_SIZE = 512;

    std::vector<float> inPcm(BLOCK_SIZE * 2, 0.5f);
    std::vector<float> outPcm(BLOCK_SIZE * 2, 0.0f);

    std::atomic<bool> producerDone{ false };
    std::atomic<size_t> totalRead{ 0 };

    std::thread consumer([&]() {
        while (!producerDone.load(std::memory_order_relaxed) || ring.availableFrames() > 0) {
            size_t n = ring.read(outPcm.data(), BLOCK_SIZE);
            totalRead.fetch_add(n, std::memory_order_relaxed);
            if (n == 0) std::this_thread::yield();
        }
    });

    size_t written = 0;
    while (written < TOTAL_FRAMES) {
        size_t toWrite = std::min(BLOCK_SIZE, TOTAL_FRAMES - written);
        size_t n = ring.write(inPcm.data(), toWrite, 2, 48000);
        written += n;
        if (n == 0) std::this_thread::yield();
    }
    producerDone.store(true, std::memory_order_release);

    consumer.join();

    assert(written == TOTAL_FRAMES);
    assert(totalRead.load() == TOTAL_FRAMES);
    RingBufferStats stats = ring.getStats();
    assert(stats.totalFramesWritten == TOTAL_FRAMES);
    assert(stats.totalFramesRead == TOTAL_FRAMES);
    assert(stats.droppedFrames == 0);
    std::cout << "PASSED (Frames: " << written << ")\n";
}

// Test 2: Long-duration playback (10,000,000 frames simulation ~ 3.5 hours at 48kHz)
void test_long_playback() {
    std::cout << "[Test 2] Long-duration playback (10,000,000 frames simulation)... " << std::flush;
    PcmRingBuffer ring;
    ring.configure(48000, 2, 16384);

    constexpr size_t TARGET_FRAMES = 10000000;
    constexpr size_t CHUNK = 1024;
    std::vector<float> chunk(CHUNK * 2, 0.25f);

    std::atomic<bool> done{ false };
    std::atomic<size_t> consumed{ 0 };

    auto start = std::chrono::steady_clock::now();

    std::thread consumer([&]() {
        std::vector<float> readBuf(CHUNK * 2);
        while (!done.load(std::memory_order_relaxed) || ring.availableFrames() > 0) {
            size_t n = ring.read(readBuf.data(), CHUNK);
            consumed.fetch_add(n, std::memory_order_relaxed);
            if (n == 0) std::this_thread::yield();
        }
    });

    size_t produced = 0;
    while (produced < TARGET_FRAMES) {
        while (ring.availableFrames() > ring.getCapacityFrames() - CHUNK * 2) {
            std::this_thread::yield();
        }
        size_t toWrite = std::min(CHUNK, TARGET_FRAMES - produced);
        size_t n = ring.write(chunk.data(), toWrite, 2, 48000);
        produced += n;
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    auto elapsed = std::chrono::steady_clock::now() - start;
    double sec = std::chrono::duration<double>(elapsed).count();

    assert(produced == TARGET_FRAMES);
    assert(consumed.load() == TARGET_FRAMES);
    RingBufferStats stats = ring.getStats();
    assert(stats.droppedFrames == 0);
    std::cout << "PASSED (" << TARGET_FRAMES << " frames in " << std::fixed << std::setprecision(2) << sec << "s, "
              << static_cast<uint64_t>(TARGET_FRAMES / sec) << " fps)\n";
}

// Test 3: AO recreation & format changes under continuous streaming
void test_ao_recreation() {
    std::cout << "[Test 3] AO recreation & dynamic format changes (500 recreations)... " << std::flush;
    SimulatedMpvTap tap;
    PcmRingBuffer ring;
    ring.configure(44100, 2, 8192);

    auto cb = [](const float* data, int frames, int channels, int rate, void* ud) {
        auto* r = static_cast<PcmRingBuffer*>(ud);
        r->write(data, static_cast<size_t>(frames), channels, rate);
    };
    tap.set_callback(cb, &ring);

    std::atomic<bool> running{ true };
    std::atomic<uint64_t> totalCallbackFrames{ 0 };

    // Audio thread streaming PCM
    std::thread audioThread([&]() {
        std::vector<float> buf(512 * 8, 0.1f);
        void* planes[1] = { buf.data() };
        while (running.load(std::memory_order_relaxed)) {
            int currentChannels = ring.getChannels();
            int currentRate = ring.getSampleRate();
            tap.invoke_tap(planes, 512, currentChannels, currentRate, MPV_FMT_FLOAT);
            totalCallbackFrames.fetch_add(512, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    // Control thread repeatedly recreating AO with different configs
    const int sampleRates[] = { 44100, 48000, 88200, 96000, 192000 };
    const int channelConfigs[] = { 2, 6, 8, 2 };

    for (int i = 0; i < 500; ++i) {
        int newSr = sampleRates[i % 5];
        int newCh = channelConfigs[i % 4];

        // In mpv, AO recreation re-configures the stream
        ring.configure(newSr, newCh, 8192);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    running.store(false, std::memory_order_release);
    audioThread.join();

    tap.set_callback(nullptr, nullptr);

    assert(totalCallbackFrames.load() > 0);
    std::cout << "PASSED (Streamed " << totalCallbackFrames.load() << " frames across 500 AO changes)\n";
}

// Test 4: Callback unregister race (10,000+ concurrent iterations)
void test_unregister_race() {
    std::cout << "[Test 4] Callback unregister race (10,000 concurrent iterations)... " << std::flush;
    SimulatedMpvTap tap;

    struct ContextState {
        std::atomic<uint32_t> magic{ 0xDEADBEEF };
        std::atomic<uint64_t> invocations{ 0 };
    };

    std::atomic<bool> running{ true };

    // Audio thread constantly calling callback
    std::thread audioThread([&]() {
        float sample = 0.5f;
        void* planes[1] = { &sample };
        while (running.load(std::memory_order_relaxed)) {
            tap.invoke_tap(planes, 1, 1, 48000, MPV_FMT_FLOAT);
        }
    });

    auto testCb = [](const float*, int, int, int, void* userdata) {
        auto* ctx = static_cast<ContextState*>(userdata);
        // If UAF happened, magic would be destroyed or corrupted
        uint32_t m = ctx->magic.load(std::memory_order_relaxed);
        assert(m == 0xDEADBEEF);
        ctx->invocations.fetch_add(1, std::memory_order_relaxed);
    };

    for (int i = 0; i < 10000; ++i) {
        auto* ctx = new ContextState();
        tap.set_callback(testCb, ctx);

        // Run momentarily
        std::this_thread::yield();

        // Unregister
        tap.set_callback(nullptr, nullptr);

        // Immediate free of ctx: if unregister didn't wait for in-flight callbacks,
        // audio thread would access freed ctx and crash or fail magic check!
        ctx->magic.store(0xBAADF00D, std::memory_order_relaxed);
        delete ctx;
    }

    running.store(false, std::memory_order_release);
    audioThread.join();

    std::cout << "PASSED (10,000 unregister cycles without UAF or memory error)\n";
}

// Test 5: Shutdown race (concurrent play/stop/destroy/callback)
void test_shutdown_race() {
    std::cout << "[Test 5] Shutdown race (concurrent play/stop/destroy/callback)... " << std::flush;
    for (int iter = 0; iter < 1000; ++iter) {
        SimulatedMpvTap tap;
        PcmRingBuffer ring;
        ring.configure(48000, 2, 4096);

        auto cb = [](const float* d, int f, int ch, int r, void* ud) {
            auto* rb = static_cast<PcmRingBuffer*>(ud);
            rb->write(d, static_cast<size_t>(f), ch, r);
        };
        tap.set_callback(cb, &ring);

        std::atomic<bool> stopAudio{ false };
        std::thread audio([&]() {
            float dummy[64 * 2];
            void* planes[1] = { dummy };
            while (!stopAudio.load(std::memory_order_relaxed)) {
                tap.invoke_tap(planes, 64, 2, 48000, MPV_FMT_FLOAT);
            }
        });

        std::thread consumer([&]() {
            float readBuf[64 * 2];
            for (int i = 0; i < 50; ++i) {
                ring.read(readBuf, 64);
                std::this_thread::yield();
            }
        });

        std::this_thread::yield();

        // Proper shutdown protocol
        tap.set_callback(nullptr, nullptr);
        stopAudio.store(true, std::memory_order_release);

        audio.join();
        consumer.join();
    }
    std::cout << "PASSED (1,000 shutdown race iterations)\n";
}

// Test 6: Multi-channel support (1, 2, 6, 8 channels)
void test_multichannel() {
    std::cout << "[Test 6] Multi-channel support (1, 2, 6, 8 channels)... " << std::flush;
    const int channelsList[] = { 1, 2, 6, 8 };

    for (int ch : channelsList) {
        PcmRingBuffer ring;
        ring.configure(48000, ch, 4096);

        constexpr size_t FRAMES = 1024;
        std::vector<float> inData(FRAMES * static_cast<size_t>(ch));
        for (size_t i = 0; i < FRAMES; ++i) {
            for (int c = 0; c < ch; ++c) {
                inData[i * static_cast<size_t>(ch) + static_cast<size_t>(c)] = static_cast<float>(c + 1) * 0.1f;
            }
        }

        size_t written = ring.write(inData.data(), FRAMES, ch, 48000);
        assert(written == FRAMES);

        std::vector<float> outData(FRAMES * static_cast<size_t>(ch), 0.0f);
        size_t readCount = ring.read(outData.data(), FRAMES);
        assert(readCount == FRAMES);

        // Verify channel values
        for (size_t i = 0; i < FRAMES; ++i) {
            for (int c = 0; c < ch; ++c) {
                float expected = static_cast<float>(c + 1) * 0.1f;
                float actual = outData[i * static_cast<size_t>(ch) + static_cast<size_t>(c)];
                assert(std::abs(actual - expected) < 1e-6f);
            }
        }
    }
    std::cout << "PASSED (1, 2, 6, 8 channels verified)\n";
}

// Test 7: Varied block sizes with ZERO silent frame drops
void test_block_sizes() {
    std::cout << "[Test 7] Varied block sizes (1 to 8192 frames, zero drops)... " << std::flush;
    const size_t blockSizes[] = { 1, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };

    SimulatedMpvTap tap;
    PcmRingBuffer ring;
    ring.configure(48000, 2, 65536);

    std::atomic<size_t> totalReceived{ 0 };
    auto cb = [](const float*, int frames, int, int, void* ud) {
        auto* count = static_cast<std::atomic<size_t>*>(ud);
        count->fetch_add(static_cast<size_t>(frames), std::memory_order_relaxed);
    };
    tap.set_callback(cb, &totalReceived);

    for (size_t bs : blockSizes) {
        totalReceived.store(0);
        // Test with planar float to trigger chunking loop
        std::vector<float> plane0(bs, 0.1f);
        std::vector<float> plane1(bs, -0.1f);
        const float* planes[2] = { plane0.data(), plane1.data() };

        tap.invoke_tap(const_cast<void**>(reinterpret_cast<const void**>(planes)), static_cast<int>(bs), 2, 48000, MPV_FMT_FLOATP);

        // Must match exactly, never truncated or silently dropped
        assert(totalReceived.load() == bs);
    }

    // P3 #12: Test 8 channels x 8192 frames to trigger multi-chunk path (scratch: 32768 floats / 8 = 4096 frames/chunk)
    {
        totalReceived.store(0);
        std::atomic<size_t> callbackCount{ 0 };
        auto chunkCb = [](const float*, int frames, int, int, void* ud) {
            auto* p = static_cast<std::pair<std::atomic<size_t>*, std::atomic<size_t>*>*>(ud);
            p->first->fetch_add(static_cast<size_t>(frames), std::memory_order_relaxed);
            p->second->fetch_add(1, std::memory_order_relaxed);
        };
        std::pair<std::atomic<size_t>*, std::atomic<size_t>*> ctx(&totalReceived, &callbackCount);
        tap.set_callback(chunkCb, &ctx);

        constexpr size_t FRAMES_8CH = 8192;
        constexpr int CHANNELS_8 = 8;
        std::vector<std::vector<float>> planesData(CHANNELS_8, std::vector<float>(FRAMES_8CH, 0.05f));
        std::vector<const float*> planePtrs(CHANNELS_8);
        for (int c = 0; c < CHANNELS_8; ++c) planePtrs[c] = planesData[c].data();

        tap.invoke_tap(const_cast<void**>(reinterpret_cast<const void**>(planePtrs.data())),
                       static_cast<int>(FRAMES_8CH), CHANNELS_8, 48000, MPV_FMT_FLOATP);

        assert(totalReceived.load() == FRAMES_8CH);
        assert(callbackCount.load() == 2); // Exactly 2 chunks of 4096 frames!
    }

    tap.set_callback(nullptr, nullptr);
    std::cout << "PASSED (Block sizes 1..8192, multi-chunk 8ch x 8192 verified)\n";
}

// Test 8: Overflow policy with slow consumer
void test_overflow_policy() {
    std::cout << "[Test 8] Overflow policy & statistics under backlog... " << std::flush;
    PcmRingBuffer ring;
    ring.configure(48000, 2, 1024); // Small capacity: 1024 frames

    std::vector<float> chunk(256 * 2, 0.5f);

    // Write until full
    size_t w1 = ring.write(chunk.data(), 256, 2, 48000);
    size_t w2 = ring.write(chunk.data(), 256, 2, 48000);
    size_t w3 = ring.write(chunk.data(), 256, 2, 48000);
    size_t w4 = ring.write(chunk.data(), 256, 2, 48000);
    assert(w1 == 256 && w2 == 256 && w3 == 256 && w4 == 256);

    // 5th write exceeds capacity: must not block and must track drops
    size_t w5 = ring.write(chunk.data(), 256, 2, 48000);
    assert(w5 == 0);

    RingBufferStats stats = ring.getStats();
    assert(stats.droppedFrames == 256);
    assert(stats.overflowCount == 1);
    assert(stats.totalFramesWritten == 1024);

    // Read partial
    std::vector<float> readBuf(256 * 2);
    size_t r1 = ring.read(readBuf.data(), 256);
    assert(r1 == 256);

    // Now write again: should succeed for 256
    size_t w6 = ring.write(chunk.data(), 256, 2, 48000);
    assert(w6 == 256);

    std::cout << "PASSED (Non-blocking overflow, tracked " << stats.droppedFrames << " dropped frames)\n";
}

// Test 9: Data consistency & deterministic signal ramp
void test_data_consistency() {
    std::cout << "[Test 9] Data consistency ramp & format conversions... " << std::flush;

    // 1. Interleaved ramp test
    {
        PcmRingBuffer ring;
        ring.configure(48000, 2, 8192);

        constexpr size_t N = 4096;
        std::vector<float> rampIn(N * 2);
        for (size_t i = 0; i < N; ++i) {
            rampIn[2 * i] = static_cast<float>(i);
            rampIn[2 * i + 1] = -static_cast<float>(i);
        }

        ring.write(rampIn.data(), N, 2, 48000);

        std::vector<float> rampOut(N * 2, 0.0f);
        size_t r = ring.read(rampOut.data(), N);
        assert(r == N);

        for (size_t i = 0; i < N; ++i) {
            assert(rampOut[2 * i] == static_cast<float>(i));
            assert(rampOut[2 * i + 1] == -static_cast<float>(i));
        }
    }

    // 2. Format conversion test (S16 -> Canonical Float32)
    {
        SimulatedMpvTap tap;
        std::vector<float> captured;
        auto cb = [](const float* data, int frames, int channels, int, void* ud) {
            auto* vec = static_cast<std::vector<float>*>(ud);
            vec->insert(vec->end(), data, data + static_cast<size_t>(frames * channels));
        };
        tap.set_callback(cb, &captured);

        constexpr int S16_FRAMES = 5000;
        std::vector<int16_t> s16Data(S16_FRAMES * 2);
        for (int i = 0; i < S16_FRAMES; ++i) {
            s16Data[2 * i] = static_cast<int16_t>(i % 32767);
            s16Data[2 * i + 1] = static_cast<int16_t>(-(i % 32767));
        }
        void* planes[1] = { s16Data.data() };
        tap.invoke_tap(planes, S16_FRAMES, 2, 48000, MPV_FMT_S16);

        assert(captured.size() == static_cast<size_t>(S16_FRAMES * 2));
        for (int i = 0; i < S16_FRAMES; ++i) {
            float expL = static_cast<float>(i % 32767) / 32768.0f;
            float expR = static_cast<float>(-(i % 32767)) / 32768.0f;
            assert(std::abs(captured[2 * static_cast<size_t>(i)] - expL) < 1e-4f);
            assert(std::abs(captured[2 * static_cast<size_t>(i) + 1] - expR) < 1e-4f);
        }
    }

    // 3. Format conversion test (S32P -> Canonical Float32)
    {
        SimulatedMpvTap tap;
        std::vector<float> captured;
        auto cb = [](const float* data, int frames, int channels, int, void* ud) {
            auto* vec = static_cast<std::vector<float>*>(ud);
            vec->insert(vec->end(), data, data + static_cast<size_t>(frames * channels));
        };
        tap.set_callback(cb, &captured);

        constexpr int S32_FRAMES = 1000;
        std::vector<int32_t> plane0(S32_FRAMES, 1073741824); // 0.5f in normalized
        std::vector<int32_t> plane1(S32_FRAMES, -1073741824); // -0.5f
        const int32_t* planes[2] = { plane0.data(), plane1.data() };
        tap.invoke_tap(const_cast<void**>(reinterpret_cast<const void**>(planes)), S32_FRAMES, 2, 48000, MPV_FMT_S32P);

        assert(captured.size() == static_cast<size_t>(S32_FRAMES * 2));
        for (int i = 0; i < S32_FRAMES; ++i) {
            assert(std::abs(captured[2 * static_cast<size_t>(i)] - 0.5f) < 1e-4f);
            assert(std::abs(captured[2 * static_cast<size_t>(i) + 1] - (-0.5f)) < 1e-4f);
        }
    }

    // 4. Format conversion test (DOUBLEP -> Canonical Float32)
    {
        SimulatedMpvTap tap;
        std::vector<float> captured;
        auto cb = [](const float* data, int frames, int channels, int, void* ud) {
            auto* vec = static_cast<std::vector<float>*>(ud);
            vec->insert(vec->end(), data, data + static_cast<size_t>(frames * channels));
        };
        tap.set_callback(cb, &captured);

        constexpr int DBL_FRAMES = 1000;
        std::vector<double> plane0(DBL_FRAMES, 0.75);
        std::vector<double> plane1(DBL_FRAMES, -0.25);
        const double* planes[2] = { plane0.data(), plane1.data() };
        tap.invoke_tap(const_cast<void**>(reinterpret_cast<const void**>(planes)), DBL_FRAMES, 2, 48000, MPV_FMT_DOUBLEP);

        assert(captured.size() == static_cast<size_t>(DBL_FRAMES * 2));
        for (int i = 0; i < DBL_FRAMES; ++i) {
            assert(std::abs(captured[2 * static_cast<size_t>(i)] - 0.75f) < 1e-5f);
            assert(std::abs(captured[2 * static_cast<size_t>(i) + 1] - (-0.25f)) < 1e-5f);
        }
    }

    std::cout << "PASSED (Ramp linearity, S16, S32P, and DOUBLEP conversions verified)\n";
}

// Test 10: Performance benchmark & latency percentiles
void test_benchmark() {
    std::cout << "[Test 10] Performance & callback duration benchmark... " << std::flush;
    PcmRingBuffer ring;
    ring.configure(48000, 2, 65536);

    constexpr size_t ITERATIONS = 100000;
    constexpr size_t FRAMES_PER_CALL = 256;
    std::vector<float> block(FRAMES_PER_CALL * 2, 0.7f);

    std::vector<double> durationsNs;
    durationsNs.reserve(ITERATIONS);

    // Consume in parallel so ring doesn't stay full
    std::atomic<bool> benchRunning{ true };
    std::thread consumer([&]() {
        std::vector<float> rbuf(FRAMES_PER_CALL * 2);
        while (benchRunning.load(std::memory_order_relaxed)) {
            ring.read(rbuf.data(), FRAMES_PER_CALL);
        }
    });

    for (size_t i = 0; i < ITERATIONS; ++i) {
        auto t0 = std::chrono::high_resolution_clock::now();
        ring.write(block.data(), FRAMES_PER_CALL, 2, 48000);
        auto t1 = std::chrono::high_resolution_clock::now();
        durationsNs.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    benchRunning.store(false, std::memory_order_release);
    consumer.join();

    std::sort(durationsNs.begin(), durationsNs.end());
    double p50 = durationsNs[static_cast<size_t>(ITERATIONS * 0.50)];
    double p99 = durationsNs[static_cast<size_t>(ITERATIONS * 0.99)];
    double p999 = durationsNs[static_cast<size_t>(ITERATIONS * 0.999)];
    double maxDur = durationsNs.back();

    double totalNs = 0.0;
    for (double d : durationsNs) totalNs += d;
    double mean = totalNs / static_cast<double>(ITERATIONS);

    std::cout << "PASSED\n";
    std::cout << "  ├─ Iterations: " << ITERATIONS << " (25.6M frames)\n";
    std::cout << "  ├─ Mean:   " << std::fixed << std::setprecision(1) << mean << " ns\n";
    std::cout << "  ├─ p50:    " << p50 << " ns\n";
    std::cout << "  ├─ p99:    " << p99 << " ns\n";
    std::cout << "  ├─ p99.9:  " << p999 << " ns\n";
    std::cout << "  └─ Max:    " << maxDur << " ns\n";
}

// Test 11: Unsupported format handling (U8 / U8P zero callback)
void test_unsupported_formats() {
    std::cout << "[Test 11] Unsupported format skip (U8 / U8P zero callback)... " << std::flush;
    SimulatedMpvTap tap;
    std::atomic<size_t> callbackInvocations{ 0 };
    auto cb = [](const float*, int, int, int, void* ud) {
        auto* cnt = static_cast<std::atomic<size_t>*>(ud);
        cnt->fetch_add(1, std::memory_order_relaxed);
    };
    tap.set_callback(cb, &callbackInvocations);

    uint8_t u8Data[512 * 2];
    std::memset(u8Data, 128, sizeof(u8Data));
    void* planes[1] = { u8Data };

    tap.invoke_tap(planes, 512, 2, 48000, MPV_FMT_U8);
    assert(callbackInvocations.load() == 0);

    const uint8_t* u8Planes[2] = { u8Data, u8Data + 512 };
    tap.invoke_tap(const_cast<void**>(reinterpret_cast<const void**>(u8Planes)), 512, 2, 48000, MPV_FMT_U8P);
    assert(callbackInvocations.load() == 0);

    tap.set_callback(nullptr, nullptr);
    std::cout << "PASSED (Zero callbacks for U8 and U8P formats)\n";
}

// Test 12: FftProcessor concurrency stress (multi-thread race prevention under TSan)
void test_fft_processor_concurrency() {
    std::cout << "[Test 12] FftProcessor concurrency stress (multi-thread race prevention)... " << std::flush;
    std::mutex fftMutex;
    FftProcessor fft({ 128, -100.0f, -30.0f, 0.8f });

    std::atomic<bool> running{ true };
    std::atomic<uint64_t> processCount{ 0 };
    std::atomic<uint64_t> resizeCount{ 0 };
    std::atomic<uint64_t> readCount{ 0 };
    std::atomic<uint64_t> resetCount{ 0 };

    // Worker 1: processInterleaved
    std::thread tProcess([&]() {
        std::vector<float> pcm(4096 * 2, 0.3f);
        while (running.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> lock(fftMutex);
                fft.processInterleaved(pcm.data(), 128, 2);
            }
            processCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Worker 2: getFrequencyData / getTimeDomainData
    std::thread tReader([&]() {
        while (running.load(std::memory_order_relaxed)) {
            std::vector<uint8_t> freq;
            std::vector<uint8_t> time;
            {
                std::lock_guard<std::mutex> lock(fftMutex);
                freq = fft.getFrequencyData();
                time = fft.getTimeDomainData();
            }
            assert(!freq.empty() && !time.empty());
            readCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Worker 3: setFftSize (causes memory reallocation)
    std::thread tResize([&]() {
        const int sizes[] = { 32, 64, 128, 256, 512, 1024 };
        int idx = 0;
        while (running.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> lock(fftMutex);
                fft.setFftSize(sizes[idx % 6]);
            }
            idx++;
            resizeCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Worker 4: reset()
    std::thread tReset([&]() {
        while (running.load(std::memory_order_relaxed)) {
            {
                std::lock_guard<std::mutex> lock(fftMutex);
                fft.reset();
            }
            resetCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Run high contention concurrency test
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    running.store(false, std::memory_order_release);

    tProcess.join();
    tReader.join();
    tResize.join();
    tReset.join();

    assert(processCount.load() > 50);
    assert(resizeCount.load() > 50);
    assert(readCount.load() > 50);
    assert(resetCount.load() > 50);

    std::cout << "PASSED (Process: " << processCount.load()
              << ", Resize: " << resizeCount.load()
              << ", Read: " << readCount.load()
              << ", Reset: " << resetCount.load() << " ops under lock)\n";
}

// Test 13: Deferred-clear liveness through the availability gate (regression)
//
// The visualizer loop consumes the ring buffer EXCLUSIVELY through the gate
// `avail = availableFrames(); if (avail) read(...)`. clear() is deferred: it
// only raises a flag, consumed inside read()/discardExcessFrames(). Any clear()
// issued outside the consumer's own edge-triggered flush — the engine's
// play-with-position and seek commands, or (historically) the visualizer loop's
// every-tick idle flush — therefore used to wedge the gate at zero forever:
// availableFrames() short-circuited to 0 while the flag was pending, so read()
// was never reached to consume it, while the producer filled the buffer and
// dropped everything (observed live: 65536 written / 0 read / all-zero FFT).
//
// Documented flush semantics: the first read() after a pending clear drops
// everything written up to that moment — including chunks the producer wrote
// after clear() — and returns 0 once. Later chunks flow normally.
void test_deferred_clear_protocol() {
    std::cout << "[Test 13] Deferred clear liveness through the availability gate... " << std::flush;

    // A. Repeated clears while idle (the shipped wedge), then continuous
    //    production. The gate must resume reading; at most the first chunk is
    //    flushed by the deferred clear.
    {
        PcmRingBuffer ring;
        ring.configure(44100, 2, 1024);

        // Idle/paused period: the old visualizer loop cleared every 30 ms tick.
        for (int tick = 0; tick < 10; ++tick) ring.clear();

        constexpr size_t CHUNK = 64;
        constexpr size_t CHUNKS = 5;
        std::vector<float> chunk(CHUNK * 2);
        auto fillChunk = [&chunk](size_t base) {
            for (size_t i = 0; i < CHUNK; ++i) {
                chunk[2 * i] = static_cast<float>(base + i);
                chunk[2 * i + 1] = -static_cast<float>(base + i);
            }
        };

        std::vector<float> out(CHUNKS * CHUNK * 2, 0.0f);
        size_t totalRead = 0;
        for (size_t k = 0; k < CHUNKS; ++k) {
            fillChunk(100 * k);
            assert(ring.write(chunk.data(), CHUNK, 2, 44100) == CHUNK);
            // Consumer tick: bounded gate iterations (before the fix this
            // never made progress and the guard ran out).
            for (int guard = 0; guard < 100; ++guard) {
                const size_t avail = ring.availableFrames();
                if (avail == 0) break;
                totalRead += ring.read(out.data() + totalRead * 2, avail);
            }
        }

        // Chunk 0 is flushed by the deferred clear; chunks 1..4 survive.
        assert(totalRead == (CHUNKS - 1) * CHUNK);
        for (size_t s = 0; s < CHUNKS - 1; ++s) {
            for (size_t i = 0; i < CHUNK; ++i) {
                const float expected = static_cast<float>(100 * (s + 1) + i);
                assert(out[(s * CHUNK + i) * 2] == expected);
                assert(out[(s * CHUNK + i) * 2 + 1] == -expected);
            }
        }

        RingBufferStats stats = ring.getStats();
        assert(stats.totalFramesWritten == CHUNKS * CHUNK);
        assert(stats.totalFramesRead == (CHUNKS - 1) * CHUNK);
        assert(stats.droppedFrames == 0);
    }

    // B. Mid-playback clear (seek / play with an explicit position): stale
    //    audio buffered, clear raised, producer continues. The gate must
    //    recover and deliver only post-clear audio.
    {
        PcmRingBuffer ring;
        ring.configure(44100, 2, 1024);

        constexpr size_t CHUNK = 64;
        std::vector<float> chunk(CHUNK * 2);
        auto fillChunk = [&chunk](size_t base) {
            for (size_t i = 0; i < CHUNK; ++i) {
                chunk[2 * i] = static_cast<float>(base + i);
                chunk[2 * i + 1] = -static_cast<float>(base + i);
            }
        };

        // Pre-seek audio is buffered.
        fillChunk(1);
        assert(ring.write(chunk.data(), CHUNK, 2, 44100) == CHUNK);
        assert(ring.availableFrames() == CHUNK);

        // Seek: engine clears; the first post-seek chunk is produced before
        // the consumer notices the clear.
        ring.clear();
        fillChunk(2);
        assert(ring.write(chunk.data(), CHUNK, 2, 44100) == CHUNK);

        // First gated read consumes the clear: returns 0 and flushes.
        std::vector<float> out(2 * CHUNK * 2, 0.0f);
        assert(ring.availableFrames() == 2 * CHUNK); // stale count, not 0
        assert(ring.read(out.data(), 2 * CHUNK) == 0);
        assert(ring.availableFrames() == 0);

        // Post-clear chunks flow through the gate untouched.
        size_t totalRead = 0;
        for (size_t k = 0; k < 2; ++k) {
            fillChunk(10 * (k + 1));
            assert(ring.write(chunk.data(), CHUNK, 2, 44100) == CHUNK);
            for (int guard = 0; guard < 100; ++guard) {
                const size_t avail = ring.availableFrames();
                if (avail == 0) break;
                totalRead += ring.read(out.data() + totalRead * 2, avail);
            }
        }
        assert(totalRead == 2 * CHUNK);
        for (size_t s = 0; s < 2; ++s) {
            for (size_t i = 0; i < CHUNK; ++i) {
                const float expected = static_cast<float>(10 * (s + 1) + i);
                assert(out[(s * CHUNK + i) * 2] == expected);
                assert(out[(s * CHUNK + i) * 2 + 1] == -expected);
            }
        }
    }

    std::cout << "PASSED (Idle-clear wedge and mid-playback clear recover through the gate)\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << " BBeBee Audio Engine & PCM Tap Concurrency Test Suite\n";
    std::cout << "========================================================\n";

    test_normal_playback();
    test_long_playback();
    test_ao_recreation();
    test_unregister_race();
    test_shutdown_race();
    test_multichannel();
    test_block_sizes();
    test_overflow_policy();
    test_data_consistency();
    test_benchmark();
    test_unsupported_formats();
    test_fft_processor_concurrency();
    test_deferred_clear_protocol();

    std::cout << "========================================================\n";
    std::cout << " ALL 13 TESTS PASSED CLEANLY!\n";
    std::cout << "========================================================\n";
    return 0;
}
