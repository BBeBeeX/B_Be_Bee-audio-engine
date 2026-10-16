#ifndef PCM_RING_BUFFER_HPP
#define PCM_RING_BUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cstring>

namespace audio_engine {

struct RingBufferStats {
    uint64_t totalFramesWritten = 0;
    uint64_t totalFramesRead = 0;
    uint64_t droppedFrames = 0;
    uint64_t overflowCount = 0;
    uint64_t underrunCount = 0;
};

/**
 * Realtime-safe Single-Producer Single-Consumer (SPSC) Lock-Free RingBuffer for audio PCM.
 *
 * Designed specifically for bridging libmpv's realtime audio callback thread
 * to the visualizer / DSP worker thread:
 * - Producer (realtime audio thread) NEVER acquires mutexes, NEVER allocates memory, NEVER blocks.
 * - Basic unit is AUDIO FRAME (1 frame = channels interleaved float32 samples).
 * - Fixed power-of-2 capacity guaranteeing safe index wrap-around without integer modulo anomalies.
 * - Explicit acquire-release memory fences establishing strict happens-before visibility.
 * - 64-byte cache-line separation on write/read heads eliminating false sharing.
 */
class PcmRingBuffer {
public:
    PcmRingBuffer();
    ~PcmRingBuffer();

    PcmRingBuffer(const PcmRingBuffer&) = delete;
    PcmRingBuffer& operator=(const PcmRingBuffer&) = delete;

    /**
     * Configure buffer capacity and audio stream attributes.
     * Synchronizes with both writer and reader quiescence before updating memory.
     */
    bool configure(int sampleRate, int channels, size_t capacityFrames);

    /**
     * Producer method: writes interleaved float frames into ring buffer.
     * Realtime safe: non-blocking, lock-free, zero allocation.
     * If buffer cannot fit all frames, drops excess frames and records stats without blocking.
     * @param interleaved Interleaved float32 samples [-1.0, 1.0)
     * @param frames Number of audio frames (samples per channel)
     * @param incomingChannels Expected channel count (0 skips validation)
     * @param incomingSampleRate Expected sample rate in Hz (0 skips validation)
     * @return Number of frames actually written.
     */
    size_t write(const float* interleaved, size_t frames, int incomingChannels = 0, int incomingSampleRate = 0);

    /**
     * Consumer method: reads up to maxFrames interleaved float frames into caller buffer.
     * @return Number of frames actually read.
     */
    size_t read(float* interleaved, size_t maxFrames);

    /**
     * Clear buffer content instantaneously via single-consumer deferred request.
     */
    void clear();

    /**
     * Returns total frames currently buffered and ready to read.
     */
    size_t availableFrames() const;

    /**
     * Consumer helper: if available frames exceed threshold, advances read pointer.
     */
    size_t discardExcessFrames(size_t maxFramesToKeep);

    int getSampleRate() const { return sampleRate_.load(std::memory_order_relaxed); }
    int getChannels() const { return channels_.load(std::memory_order_relaxed); }
    size_t getCapacityFrames() const { return capacity_; }

    RingBufferStats getStats() const {
        RingBufferStats s;
        s.totalFramesWritten = totalFramesWritten_.load(std::memory_order_relaxed);
        s.totalFramesRead = totalFramesRead_.load(std::memory_order_relaxed);
        s.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
        s.overflowCount = overflowCount_.load(std::memory_order_relaxed);
        s.underrunCount = underrunCount_.load(std::memory_order_relaxed);
        return s;
    }

    void resetStats() {
        totalFramesWritten_.store(0, std::memory_order_relaxed);
        totalFramesRead_.store(0, std::memory_order_relaxed);
        droppedFrames_.store(0, std::memory_order_relaxed);
        overflowCount_.store(0, std::memory_order_relaxed);
        underrunCount_.store(0, std::memory_order_relaxed);
    }

private:
    std::atomic<int> sampleRate_{ 44100 };
    std::atomic<int> channels_{ 2 };
    size_t capacity_{ 0 };
    size_t capacityMask_{ 0 };
    std::vector<float> buffer_;

    // Protection during reconfiguration
    std::atomic<bool> configuring_{ false };
    std::atomic<int> activeWriters_{ 0 };
    std::atomic<int> activeReaders_{ 0 };

    // Deferred clear request to preserve SPSC single-consumer guarantee
    std::atomic<bool> clearRequested_{ false };

    // Statistics
    std::atomic<uint64_t> totalFramesWritten_{ 0 };
    std::atomic<uint64_t> totalFramesRead_{ 0 };
    std::atomic<uint64_t> droppedFrames_{ 0 };
    std::atomic<uint64_t> overflowCount_{ 0 };
    std::atomic<uint64_t> underrunCount_{ 0 };

    // Separate producer and consumer indices into independent cache lines (64 bytes)
    alignas(64) std::atomic<size_t> writeIndex_{ 0 };
    uint8_t padProducer_[64 - sizeof(std::atomic<size_t>)];

    alignas(64) std::atomic<size_t> readIndex_{ 0 };
    uint8_t padConsumer_[64 - sizeof(std::atomic<size_t>)];
};

} // namespace audio_engine

#endif // PCM_RING_BUFFER_HPP
