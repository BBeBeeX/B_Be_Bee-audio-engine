#include "pcm_ring_buffer.hpp"
#include <thread>

namespace audio_engine {

PcmRingBuffer::PcmRingBuffer() = default;
PcmRingBuffer::~PcmRingBuffer() = default;

bool PcmRingBuffer::configure(int sampleRate, int channels, size_t capacityFrames) {
    if (sampleRate <= 0 || channels <= 0 || capacityFrames == 0) {
        return false;
    }

    constexpr size_t MAX_CAPACITY_FRAMES = 1ULL << 30;
    if (capacityFrames > MAX_CAPACITY_FRAMES) {
        return false;
    }

    // Round up capacity to next power of 2 for wrap-around masking
    size_t cap = 1;
    while (cap < capacityFrames) {
        if (cap > (MAX_CAPACITY_FRAMES >> 1)) {
            return false;
        }
        cap <<= 1;
    }

    // 1. Signal workers that reconfiguration is underway
    configuring_.store(true, std::memory_order_seq_cst);

    // 2. Wait for any active write() and read() invocations in flight to complete
    while (activeWriters_.load(std::memory_order_seq_cst) > 0 ||
           activeReaders_.load(std::memory_order_seq_cst) > 0) {
        std::this_thread::yield();
    }

    // 3. Reallocate buffer and update attributes
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    channels_.store(channels, std::memory_order_relaxed);
    capacity_ = cap;
    capacityMask_ = cap - 1;
    buffer_.assign(capacity_ * static_cast<size_t>(channels), 0.0f);

    clearRequested_.store(false, std::memory_order_release);
    writeIndex_.store(0, std::memory_order_release);
    readIndex_.store(0, std::memory_order_release);

    configuring_.store(false, std::memory_order_release);
    return true;
}

size_t PcmRingBuffer::write(const float* interleaved, size_t frames, int incomingChannels, int incomingSampleRate) {
    if (!interleaved || frames == 0) {
        return 0;
    }

    // Fast-path check: do not write while configure is in progress
    if (configuring_.load(std::memory_order_acquire)) {
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }

    activeWriters_.fetch_add(1, std::memory_order_seq_cst);

    // Double check after announcing active writer
    const int currentCh = channels_.load(std::memory_order_relaxed);
    const int currentSr = sampleRate_.load(std::memory_order_relaxed);
    if (configuring_.load(std::memory_order_seq_cst) || capacity_ == 0 || currentCh <= 0) {
        activeWriters_.fetch_sub(1, std::memory_order_seq_cst);
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }

    // Format mismatch check: if incoming channels/rate mismatch, drop safely without corrupting buffer
    if (incomingChannels > 0 && incomingChannels != currentCh) {
        activeWriters_.fetch_sub(1, std::memory_order_seq_cst);
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }
    if (incomingSampleRate > 0 && incomingSampleRate != currentSr) {
        activeWriters_.fetch_sub(1, std::memory_order_seq_cst);
        droppedFrames_.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }

    // Producer reads readIndex_ with acquire to ensure consumer reads from previous cycle have completed
    const size_t r = readIndex_.load(std::memory_order_acquire);
    // Producer reads writeIndex_ with relaxed since writeIndex_ is only updated by the producer thread
    const size_t w = writeIndex_.load(std::memory_order_relaxed);
    const size_t occupied = w - r;
    const size_t freeFrames = (occupied < capacity_) ? (capacity_ - occupied) : 0;

    const size_t toWrite = std::min(frames, freeFrames);
    if (toWrite < frames) {
        const size_t dropped = frames - toWrite;
        droppedFrames_.fetch_add(dropped, std::memory_order_relaxed);
        overflowCount_.fetch_add(1, std::memory_order_relaxed);
    }

    if (toWrite == 0) {
        activeWriters_.fetch_sub(1, std::memory_order_seq_cst);
        return 0;
    }

    const size_t ch = static_cast<size_t>(currentCh);
    const size_t wPos = w & capacityMask_;

    // Write in up to two contiguous chunks
    const size_t firstChunk = std::min(toWrite, capacity_ - wPos);
    std::memcpy(&buffer_[wPos * ch], interleaved, firstChunk * ch * sizeof(float));

    if (toWrite > firstChunk) {
        const size_t secondChunk = toWrite - firstChunk;
        std::memcpy(&buffer_[0], &interleaved[firstChunk * ch], secondChunk * ch * sizeof(float));
    }

    // Publish written frames with memory_order_release so consumer sees updated buffer content
    writeIndex_.store(w + toWrite, std::memory_order_release);
    totalFramesWritten_.fetch_add(toWrite, std::memory_order_relaxed);
    activeWriters_.fetch_sub(1, std::memory_order_seq_cst);
    return toWrite;
}

size_t PcmRingBuffer::read(float* interleaved, size_t maxFrames) {
    // Process deferred clear on consumer thread
    if (clearRequested_.exchange(false, std::memory_order_acq_rel)) {
        const size_t w = writeIndex_.load(std::memory_order_acquire);
        readIndex_.store(w, std::memory_order_release);
        return 0;
    }

    if (!interleaved || maxFrames == 0) {
        return 0;
    }

    if (configuring_.load(std::memory_order_acquire)) {
        return 0;
    }

    activeReaders_.fetch_add(1, std::memory_order_seq_cst);

    const int currentCh = channels_.load(std::memory_order_relaxed);
    if (configuring_.load(std::memory_order_seq_cst) || capacity_ == 0 || currentCh <= 0) {
        activeReaders_.fetch_sub(1, std::memory_order_seq_cst);
        return 0;
    }

    // Consumer reads writeIndex_ with acquire to synchronize with producer's release store
    const size_t w = writeIndex_.load(std::memory_order_acquire);
    // Consumer reads readIndex_ with relaxed since readIndex_ is only updated by the consumer thread
    const size_t r = readIndex_.load(std::memory_order_relaxed);
    if (w <= r) {
        underrunCount_.fetch_add(1, std::memory_order_relaxed);
        activeReaders_.fetch_sub(1, std::memory_order_seq_cst);
        return 0;
    }

    const size_t available = w - r;
    const size_t toRead = std::min(maxFrames, available);

    const size_t ch = static_cast<size_t>(currentCh);
    const size_t rPos = r & capacityMask_;

    // Read in up to two contiguous chunks
    const size_t firstChunk = std::min(toRead, capacity_ - rPos);
    std::memcpy(interleaved, &buffer_[rPos * ch], firstChunk * ch * sizeof(float));

    if (toRead > firstChunk) {
        const size_t secondChunk = toRead - firstChunk;
        std::memcpy(&interleaved[firstChunk * ch], &buffer_[0], secondChunk * ch * sizeof(float));
    }

    // Publish readIndex_ with memory_order_release so producer knows memory is reclaimed
    readIndex_.store(r + toRead, std::memory_order_release);
    totalFramesRead_.fetch_add(toRead, std::memory_order_relaxed);
    activeReaders_.fetch_sub(1, std::memory_order_seq_cst);
    return toRead;
}

void PcmRingBuffer::clear() {
    clearRequested_.store(true, std::memory_order_release);
}

size_t PcmRingBuffer::availableFrames() const {
    // No clearRequested_ short-circuit here on purpose: the deferred clear is
    // consumed only inside read()/discardExcessFrames(), which the consumer
    // reaches only through this availability gate. Gating on the flag made
    // every clear() issued outside that gate (play with a position, seek) wedge
    // the consumer at zero frames forever while the producer filled up and
    // dropped everything. read() still honours the pending clear: its first
    // invocation flushes and returns 0 once, so stale data is never visualized.
    const size_t w = writeIndex_.load(std::memory_order_acquire);
    const size_t r = readIndex_.load(std::memory_order_relaxed);
    return (w > r) ? (w - r) : 0;
}

size_t PcmRingBuffer::discardExcessFrames(size_t maxFramesToKeep) {
    if (clearRequested_.exchange(false, std::memory_order_acq_rel)) {
        const size_t w = writeIndex_.load(std::memory_order_acquire);
        readIndex_.store(w, std::memory_order_release);
        return 0;
    }

    const size_t w = writeIndex_.load(std::memory_order_acquire);
    const size_t r = readIndex_.load(std::memory_order_relaxed);
    if (w <= r) return 0;
    const size_t avail = w - r;
    if (avail > maxFramesToKeep) {
        const size_t discarded = avail - maxFramesToKeep;
        readIndex_.store(r + discarded, std::memory_order_release);
        return discarded;
    }
    return 0;
}

} // namespace audio_engine
