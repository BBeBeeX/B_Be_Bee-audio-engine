#include "pcm_broadcaster.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketFd = SOCKET;
static constexpr SocketFd INVALID_SOCK = INVALID_SOCKET;
static void closeSocketFd(SocketFd fd) {
    if (fd != INVALID_SOCKET) {
        closesocket(fd);
    }
}
static void ensureWinsockInit() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using SocketFd = int;
static constexpr SocketFd INVALID_SOCK = -1;
static void closeSocketFd(SocketFd fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}
static void ensureWinsockInit() {}
#endif

namespace audio_engine {

namespace {

constexpr size_t BROADCAST_RING_CAPACITY_FRAMES = 65536; // ~1.36s at 48 kHz
constexpr size_t READ_CHUNK_FRAMES = 1024;

bool sendAll(SocketFd fd, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#if defined(_WIN32)
        int n = ::send(fd, reinterpret_cast<const char*>(data + sent), static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
#else
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = ::send(fd, data + sent, len - sent, flags);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
#endif
    }
    return true;
}

SocketFd connectLoopbackTcp(int port) {
    ensureWinsockInit();
    SocketFd fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK) return INVALID_SOCK;

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    // Bounded send timeout so a stalled consumer drops and reconnects cleanly
    // instead of wedging worker shutdown.
#if defined(_WIN32)
    DWORD timeoutMs = 250;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 250 * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocketFd(fd);
        return INVALID_SOCK;
    }
    return fd;
}

} // namespace

PcmBroadcaster::PcmBroadcaster() {
    ring_.configure(48000, 2, BROADCAST_RING_CAPACITY_FRAMES);
}

PcmBroadcaster::~PcmBroadcaster() {
    stop();
}

std::array<uint8_t, PcmBroadcaster::HEADER_BYTES> PcmBroadcaster::encodeHeader(
    uint32_t sampleRate,
    uint16_t channels,
    uint16_t format) {
    std::array<uint8_t, HEADER_BYTES> hdr{};
    hdr[0] = 'B';
    hdr[1] = 'B';
    hdr[2] = 'P';
    hdr[3] = 'C';
    // version = 1 (uint16 LE)
    hdr[4] = static_cast<uint8_t>(PROTOCOL_VERSION & 0xFF);
    hdr[5] = static_cast<uint8_t>((PROTOCOL_VERSION >> 8) & 0xFF);
    // format (uint16 LE)
    hdr[6] = static_cast<uint8_t>(format & 0xFF);
    hdr[7] = static_cast<uint8_t>((format >> 8) & 0xFF);
    // sampleRate (uint32 LE)
    hdr[8] = static_cast<uint8_t>(sampleRate & 0xFF);
    hdr[9] = static_cast<uint8_t>((sampleRate >> 8) & 0xFF);
    hdr[10] = static_cast<uint8_t>((sampleRate >> 16) & 0xFF);
    hdr[11] = static_cast<uint8_t>((sampleRate >> 24) & 0xFF);
    // channels (uint16 LE)
    hdr[12] = static_cast<uint8_t>(channels & 0xFF);
    hdr[13] = static_cast<uint8_t>((channels >> 8) & 0xFF);
    // reserved = 0 (uint16 LE)
    hdr[14] = 0;
    hdr[15] = 0;
    return hdr;
}

void PcmBroadcaster::float32ToInt16Le(const float* src, size_t sampleCount, uint8_t* dst) {
    for (size_t i = 0; i < sampleCount; ++i) {
        float s = src[i];
        if (s > 1.0f) s = 1.0f;
        else if (s < -1.0f) s = -1.0f;
        else if (!(s == s)) s = 0.0f; // NaN guard
        const int32_t scaled = static_cast<int32_t>(s < 0.0f ? s * 32768.0f : s * 32767.0f);
        const int16_t clamped = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, scaled)));
        dst[i * 2] = static_cast<uint8_t>(clamped & 0xFF);
        dst[i * 2 + 1] = static_cast<uint8_t>((static_cast<uint16_t>(clamped) >> 8) & 0xFF);
    }
}

bool PcmBroadcaster::isLoopbackHost(const std::string& host) {
    return host.empty() || host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool PcmBroadcaster::setTarget(bool enabled, const std::string& host, int port) {
    if (enabled) {
        if (!isLoopbackHost(host) || port <= 0 || port > 65535) {
            std::cerr << "[audio-engine] PcmBroadcaster rejected invalid target " << host << ":" << port << "\n";
            return false;
        }
    }

    std::lock_guard<std::mutex> lock(configMutex_);
    if (shutdown_.load(std::memory_order_acquire)) {
        return false;
    }

    targetHost_ = host.empty() ? "127.0.0.1" : host;
    targetPort_ = enabled ? port : 0;
    active_.store(enabled, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    if (!enabled) {
        ring_.clear();
    }

    if (enabled && !workerThread_.joinable()) {
        workerThread_ = std::thread(&PcmBroadcaster::workerLoop, this);
    }
    return true;
}

void PcmBroadcaster::configureFormat(int sampleRate, int channels) {
    if (sampleRate <= 0 || channels <= 0) return;
    const int prevSr = sampleRate_.exchange(sampleRate, std::memory_order_acq_rel);
    const int prevCh = channels_.exchange(channels, std::memory_order_acq_rel);
    ring_.configure(sampleRate, channels, BROADCAST_RING_CAPACITY_FRAMES);
    if (prevSr != sampleRate || prevCh != channels) {
        generation_.fetch_add(1, std::memory_order_acq_rel);
    }
}

void PcmBroadcaster::clearBuffer() {
    ring_.clear();
}

void PcmBroadcaster::pushPcm(const float* interleaved, size_t frames, int channels, int sampleRate) noexcept {
    if (!active_.load(std::memory_order_acquire)) return;
    if (!interleaved || frames == 0) return;
    ring_.write(interleaved, frames, channels, sampleRate);
}

void PcmBroadcaster::stop() {
    active_.store(false, std::memory_order_release);
    shutdown_.store(true, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    ring_.clear();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    connected_.store(false, std::memory_order_release);
}

void PcmBroadcaster::workerLoop() {
    SocketFd sock = INVALID_SOCK;
    uint64_t connectedGen = 0;
    std::vector<float> floatBuf;
    std::vector<uint8_t> pcmBytes;

    while (!shutdown_.load(std::memory_order_acquire)) {
        if (!active_.load(std::memory_order_acquire)) {
            if (sock != INVALID_SOCK) {
                closeSocketFd(sock);
                sock = INVALID_SOCK;
                connected_.store(false, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        const uint64_t curGen = generation_.load(std::memory_order_acquire);
        if (sock != INVALID_SOCK && curGen != connectedGen) {
            closeSocketFd(sock);
            sock = INVALID_SOCK;
            connected_.store(false, std::memory_order_release);
        }

        if (sock == INVALID_SOCK) {
            int port = 0;
            {
                std::lock_guard<std::mutex> lock(configMutex_);
                port = targetPort_;
            }
            if (port <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }

            sock = connectLoopbackTcp(port);
            if (sock == INVALID_SOCK) {
                // Retry with bounded backoff while active.
                for (int i = 0; i < 10 && !shutdown_.load(std::memory_order_acquire) && active_.load(std::memory_order_acquire); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                continue;
            }

            const int sr = std::max(8000, sampleRate_.load(std::memory_order_acquire));
            const int ch = std::max(1, std::min(8, channels_.load(std::memory_order_acquire)));
            const auto hdr = encodeHeader(
                static_cast<uint32_t>(sr),
                static_cast<uint16_t>(ch),
                FORMAT_S16LE);
            if (!sendAll(sock, hdr.data(), hdr.size())) {
                closeSocketFd(sock);
                sock = INVALID_SOCK;
                connected_.store(false, std::memory_order_release);
                continue;
            }
            connectedGen = curGen;
            connected_.store(true, std::memory_order_release);
        }

        const int sr = std::max(8000, sampleRate_.load(std::memory_order_acquire));
        const int ch = std::max(1, std::min(8, channels_.load(std::memory_order_acquire)));

        // Cap ring backlog to ~250ms to prevent drift if the TCP consumer stalls.
        const size_t maxBacklogFrames = std::max<size_t>(READ_CHUNK_FRAMES * 2, static_cast<size_t>(sr / 4));
        const size_t avail = ring_.availableFrames();
        if (avail > maxBacklogFrames) {
            ring_.discardExcessFrames(READ_CHUNK_FRAMES * 2);
        }

        const size_t neededFloats = READ_CHUNK_FRAMES * static_cast<size_t>(ch);
        if (floatBuf.size() < neededFloats) {
            floatBuf.resize(neededFloats);
            pcmBytes.resize(neededFloats * 2);
        }

        const size_t framesRead = ring_.read(floatBuf.data(), READ_CHUNK_FRAMES);
        if (framesRead == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }

        const size_t sampleCount = framesRead * static_cast<size_t>(ch);
        float32ToInt16Le(floatBuf.data(), sampleCount, pcmBytes.data());
        if (!sendAll(sock, pcmBytes.data(), sampleCount * 2)) {
            closeSocketFd(sock);
            sock = INVALID_SOCK;
            connected_.store(false, std::memory_order_release);
        }
    }

    if (sock != INVALID_SOCK) {
        closeSocketFd(sock);
        connected_.store(false, std::memory_order_release);
    }
}

} // namespace audio_engine
