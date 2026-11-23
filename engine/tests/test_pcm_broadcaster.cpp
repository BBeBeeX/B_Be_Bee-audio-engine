#include "../pcm_broadcaster.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using SockFd = SOCKET;
static constexpr SockFd BAD_SOCK = INVALID_SOCKET;
static void closeSock(SockFd fd) { if (fd != INVALID_SOCKET) closesocket(fd); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockFd = int;
static constexpr SockFd BAD_SOCK = -1;
static void closeSock(SockFd fd) { if (fd >= 0) ::close(fd); }
#endif

using audio_engine::PcmBroadcaster;

static void testHeaderEncoding() {
    auto hdr = PcmBroadcaster::encodeHeader(48000, 2, PcmBroadcaster::FORMAT_S16LE);
    assert(hdr[0] == 'B' && hdr[1] == 'B' && hdr[2] == 'P' && hdr[3] == 'C');
    // version = 1 LE
    assert(hdr[4] == 1 && hdr[5] == 0);
    // format = 1 (s16le) LE
    assert(hdr[6] == 1 && hdr[7] == 0);
    // sampleRate = 48000 (0x0000BB80) LE
    assert(hdr[8] == 0x80 && hdr[9] == 0xBB && hdr[10] == 0x00 && hdr[11] == 0x00);
    // channels = 2 LE
    assert(hdr[12] == 2 && hdr[13] == 0);
    // reserved = 0
    assert(hdr[14] == 0 && hdr[15] == 0);
    std::cout << "[PASS] testHeaderEncoding\n";
}

static void testFloat32ToInt16Le() {
    const float samples[5] = { 0.0f, 1.0f, -1.0f, 2.5f, -2.5f };
    uint8_t out[10] = {};
    PcmBroadcaster::float32ToInt16Le(samples, 5, out);
    auto readI16 = [&](size_t idx) -> int16_t {
        return static_cast<int16_t>(static_cast<uint16_t>(out[idx * 2]) | (static_cast<uint16_t>(out[idx * 2 + 1]) << 8));
    };
    assert(readI16(0) == 0);
    assert(readI16(1) == 32767);
    assert(readI16(2) == -32768);
    assert(readI16(3) == 32767);
    assert(readI16(4) == -32768);
    std::cout << "[PASS] testFloat32ToInt16Le\n";
}

static void testLoopbackGuard() {
    PcmBroadcaster b;
    assert(PcmBroadcaster::isLoopbackHost("127.0.0.1"));
    assert(PcmBroadcaster::isLoopbackHost("localhost"));
    assert(!PcmBroadcaster::isLoopbackHost("192.168.1.5"));
    assert(!PcmBroadcaster::isLoopbackHost("0.0.0.0"));
    assert(!b.setTarget(true, "192.168.1.5", 53321));
    assert(!b.isActive());
    assert(!b.setTarget(true, "127.0.0.1", 0));
    assert(!b.isActive());
    std::cout << "[PASS] testLoopbackGuard\n";
}

static void testLoopbackStreamingE2E() {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SockFd server = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(server != BAD_SOCK);
    int opt = 1;
    ::setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(server, 1) != 0) {
        closeSock(server);
        std::cout << "[SKIP] testLoopbackStreamingE2E (loopback bind blocked in environment)\n";
        return;
    }

    socklen_t len = sizeof(addr);
    assert(::getsockname(server, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    const int port = ntohs(addr.sin_port);

    std::vector<uint8_t> received;
    std::atomic<bool> done{ false };
    std::thread reader([&]() {
        SockFd client = ::accept(server, nullptr, nullptr);
        if (client == BAD_SOCK) return;
        uint8_t buf[4096];
        while (received.size() < 16 + 256 * 2 * 2) {
            auto n = ::recv(client, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) break;
            received.insert(received.end(), buf, buf + n);
        }
        closeSock(client);
        done.store(true, std::memory_order_release);
    });

    PcmBroadcaster broadcaster;
    broadcaster.configureFormat(48000, 2);
    assert(broadcaster.setTarget(true, "127.0.0.1", port));

    // Wait briefly for loopback TCP connection.
    for (int i = 0; i < 40 && !broadcaster.isConnected(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(broadcaster.isConnected());

    std::vector<float> pcm(256 * 2, 0.5f);
    broadcaster.pushPcm(pcm.data(), 256, 2, 48000);

    for (int i = 0; i < 50 && !done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    broadcaster.stop();
    closeSock(server);
    reader.join();

    assert(received.size() >= 16 + 256 * 2 * 2);
    assert(received[0] == 'B' && received[1] == 'B' && received[2] == 'P' && received[3] == 'C');
    assert(received[4] == 1 && received[5] == 0);
    assert(received[6] == 1 && received[7] == 0);
    // First s16le sample should be round(0.5 * 32767) = 16383
    const int16_t s0 = static_cast<int16_t>(static_cast<uint16_t>(received[16]) | (static_cast<uint16_t>(received[17]) << 8));
    assert(s0 >= 16380 && s0 <= 16385);
    std::cout << "[PASS] testLoopbackStreamingE2E\n";
}

int main() {
    testHeaderEncoding();
    testFloat32ToInt16Le();
    testLoopbackGuard();
    testLoopbackStreamingE2E();
    std::cout << "All PcmBroadcaster tests passed.\n";
    return 0;
}
