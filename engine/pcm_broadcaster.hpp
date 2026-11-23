#ifndef PCM_BROADCASTER_HPP
#define PCM_BROADCASTER_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pcm_ring_buffer.hpp"

namespace audio_engine {

/**
 * Scheme A post-DSP LAN audio broadcast tap.
 *
 * Mirrors real-time interleaved float32 PCM from libmpv's `mpv_set_pcm_callback`
 * into a dedicated lock-free SPSC PcmRingBuffer (zero locks, zero allocations,
 * zero I/O on the audio callback thread) and streams converted s16le PCM over
 * a loopback-only TCP socket to the host sidecar's `AudioBroadcastServer`.
 *
 * Wire header (16 bytes, little-endian):
 *   0..3   : ASCII "BBPC"
 *   4..5   : uint16 version  (1)
 *   6..7   : uint16 format   (1 = s16le, 2 = f32le)
 *   8..11  : uint32 sampleRate
 *   12..13 : uint16 channels
 *   14..15 : uint16 reserved (0)
 */
class PcmBroadcaster {
public:
    static constexpr uint16_t PROTOCOL_VERSION = 1;
    static constexpr uint16_t FORMAT_S16LE = 1;
    static constexpr uint16_t FORMAT_F32LE = 2;
    static constexpr size_t HEADER_BYTES = 16;

    PcmBroadcaster();
    ~PcmBroadcaster();

    PcmBroadcaster(const PcmBroadcaster&) = delete;
    PcmBroadcaster& operator=(const PcmBroadcaster&) = delete;

    /** Encode the 16-byte little-endian BBPC stream header. */
    static std::array<uint8_t, HEADER_BYTES> encodeHeader(
        uint32_t sampleRate,
        uint16_t channels,
        uint16_t format = FORMAT_S16LE);

    /** Convert interleaved float32 [-1.0, 1.0] samples to clamped little-endian int16. */
    static void float32ToInt16Le(const float* src, size_t sampleCount, uint8_t* dst);

    /** Returns true only for loopback addresses (`127.0.0.1`, `localhost`, `::1`). */
    static bool isLoopbackHost(const std::string& host);

    /**
     * Configure or disable the loopback TCP broadcast target.
     * Non-loopback hosts are rejected and return false.
     */
    bool setTarget(bool enabled, const std::string& host, int port);

    /**
     * Update audio stream format (from mpv `audio-out-params` properties).
     * Reconfigures the ring buffer and triggers a clean reconnect + new BBPC header.
     */
    void configureFormat(int sampleRate, int channels);

    /** Discard buffered frames (e.g. on seek, load, stop, EOF). */
    void clearBuffer();

    /**
     * Real-time audio thread entry point: lock-free, wait-free fast path.
     * Costs a single atomic load when broadcasting is disabled.
     */
    void pushPcm(const float* interleaved, size_t frames, int channels, int sampleRate) noexcept;

    /** Stop the worker thread and close any active socket cleanly. */
    void stop();

    bool isActive() const { return active_.load(std::memory_order_acquire); }
    bool isConnected() const { return connected_.load(std::memory_order_acquire); }

private:
    void workerLoop();

    PcmRingBuffer ring_;
    std::mutex configMutex_;
    std::string targetHost_ = "127.0.0.1";
    int targetPort_ = 0;

    std::atomic<bool> shutdown_{ false };
    std::atomic<bool> active_{ false };
    std::atomic<bool> connected_{ false };
    std::atomic<uint64_t> generation_{ 0 };
    std::atomic<int> sampleRate_{ 48000 };
    std::atomic<int> channels_{ 2 };

    std::thread workerThread_;
};

} // namespace audio_engine

#endif // PCM_BROADCASTER_HPP
