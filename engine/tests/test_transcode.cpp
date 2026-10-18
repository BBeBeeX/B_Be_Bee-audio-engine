/**
 * Audio Engine transcode test suite.
 *
 * Covers:
 *  - resolveTranscodeTarget(): format key -> container/encoder mapping
 *  - parseBitrate(): "192k"/"192000"/"" handling
 *  - Transcoder::isSupported() consistency
 *  - Without HAVE_LIBAV: stub reports "not supported"
 *  - With HAVE_LIBAV (integration): real WAV->WAV / WAV->FLAC / WAV->M4A
 *    transcodes of a synthesized sine-wave input, error paths, cancellation.
 *
 * Build: part of engine/CMakeLists.txt (audio-engine-transcode-tests).
 */
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../transcode.hpp"

using namespace audio_engine;

static int g_failed = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAIL " << __LINE__ << ": " #cond "\n"; \
        ++g_failed; \
    } \
} while (0)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string makeTempDir() {
    char tmpl[] = "/tmp/bbebee-transcode-test-XXXXXX";
    char* d = mkdtemp(tmpl);
    assert(d);
    return std::string(d);
}

static void writeU16(std::ofstream& f, uint16_t v) {
    f.put((char)(v & 0xff));
    f.put((char)((v >> 8) & 0xff));
}
static void writeU32(std::ofstream& f, uint32_t v) {
    f.put((char)(v & 0xff));
    f.put((char)((v >> 8) & 0xff));
    f.put((char)((v >> 16) & 0xff));
    f.put((char)((v >> 24) & 0xff));
}
static void writeI16(std::ofstream& f, int16_t v) { writeU16(f, (uint16_t)v); }

// Synthesize a 16-bit PCM WAV (sine 440 Hz) for use as transcode input.
static void writeSineWav(const std::string& path, double seconds,
                         int sampleRate = 44100, int channels = 2) {
    const int32_t nSamples = (int32_t)(seconds * sampleRate);
    const int32_t dataSize = nSamples * channels * 2;
    std::ofstream f(path, std::ios::binary);
    assert(f.good());
    f.write("RIFF", 4);
    writeU32(f, 36 + (uint32_t)dataSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    writeU32(f, 16);
    writeU16(f, 1); // PCM
    writeU16(f, (uint16_t)channels);
    writeU32(f, (uint32_t)sampleRate);
    writeU32(f, (uint32_t)(sampleRate * channels * 2));
    writeU16(f, (uint16_t)(channels * 2));
    writeU16(f, 16);
    f.write("data", 4);
    writeU32(f, (uint32_t)dataSize);
    for (int32_t i = 0; i < nSamples; ++i) {
        double t = (double)i / sampleRate;
        int16_t s = (int16_t)(0.5 * 32767.0 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
        for (int c = 0; c < channels; ++c) writeI16(f, s);
    }
    f.close();
    assert(f.good());
}

static bool fileExists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}
static long fileSize(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.good()) return -1;
    return (long)f.tellg();
}
static std::string fileMagic(const std::string& path, size_t n) {
    std::ifstream f(path, std::ios::binary);
    std::string m(n, '\0');
    f.read(&m[0], (std::streamsize)n);
    m.resize((size_t)f.gcount());
    return m;
}

// ---------------------------------------------------------------------------
// Pure-logic tests (no FFmpeg needed)
// ---------------------------------------------------------------------------

static void testResolveTarget() {
    {
        TranscodeTarget t = resolveTranscodeTarget("m4a");
        CHECK(t.valid && t.container == "mp4" && !t.encoders.empty() && t.encoders[0] == "aac");
    }
    {
        TranscodeTarget t = resolveTranscodeTarget("MP3"); // case-insensitive
        CHECK(t.valid && t.container == "mp3" && t.encoders[0] == "libmp3lame");
    }
    {
        TranscodeTarget t = resolveTranscodeTarget("flac");
        CHECK(t.valid && t.container == "flac" && t.encoders[0] == "flac");
    }
    {
        TranscodeTarget t = resolveTranscodeTarget("ogg");
        CHECK(t.valid && t.container == "ogg");
    }
    {
        TranscodeTarget t = resolveTranscodeTarget("opus");
        CHECK(t.valid && t.container == "ogg" && t.encoders[0] == "libopus");
    }
    {
        TranscodeTarget t = resolveTranscodeTarget("wav");
        CHECK(t.valid && t.container == "wav" && t.encoders[0] == "pcm_s16le");
    }
    {
        TranscodeTarget t = resolveTranscodeTarget("alac");
        CHECK(t.valid && t.container == "mp4" && t.encoders[0] == "alac");
    }
    {
        TranscodeTarget t = resolveTranscodeTarget("wma");
        CHECK(t.valid && t.container == "asf");
    }
    {
        TranscodeTarget t = resolveTranscodeTarget("bogus");
        CHECK(!t.valid && !t.error.empty());
    }
    {
        TranscodeTarget t = resolveTranscodeTarget("");
        CHECK(!t.valid);
    }
    std::cout << "[ok] resolveTranscodeTarget\n";
}

static void testParseBitrate() {
    CHECK(parseBitrate("192k") == 192000);
    CHECK(parseBitrate("192K") == 192000);
    CHECK(parseBitrate("192000") == 192000);
    CHECK(parseBitrate("1.5M") == 1500000);
    CHECK(parseBitrate("  64k  ") == 64000);
    CHECK(parseBitrate("") == 0);
    CHECK(parseBitrate("   ") == 0);
    CHECK(parseBitrate("abc") == 0);
    CHECK(parseBitrate("12x") == 0);
    CHECK(parseBitrate("0") == 0);
    std::cout << "[ok] parseBitrate\n";
}

// ---------------------------------------------------------------------------
// Transcoder tests
// ---------------------------------------------------------------------------

static void testValidation() {
    Transcoder tr;
    std::atomic<bool> cancel{false};
    int progressCalls = 0;

    // Empty input path.
    {
        TranscodeOptions o;
        o.outputPath = "/tmp/x.wav";
        o.format = "wav";
        TranscodeResult r = tr.transcode(o, nullptr, &cancel);
        CHECK(!r.ok && !r.error.empty());
    }
    // Unsupported format.
    {
        TranscodeOptions o;
        o.inputPath = "/tmp/x.wav";
        o.outputPath = "/tmp/y.wav";
        o.format = "bogus";
        TranscodeResult r = tr.transcode(o, nullptr, &cancel);
        CHECK(!r.ok && !r.error.empty());
        (void)progressCalls;
    }
    std::cout << "[ok] validation\n";
}

#ifdef HAVE_LIBAV

static void testWavToWav(const std::string& dir) {
    const std::string in = dir + "/in.wav";
    const std::string out = dir + "/out.wav";
    writeSineWav(in, 2.0);

    Transcoder tr;
    std::atomic<bool> cancel{false};
    int progressCalls = 0;
    double lastProgress = -1.0;
    TranscodeOptions o;
    o.jobId = "t1";
    o.inputPath = in;
    o.outputPath = out;
    o.format = "wav";

    TranscodeResult r = tr.transcode(
        o,
        [&](double p, double cur, double dur) {
            ++progressCalls;
            if (p >= 0.0) {
                CHECK(p >= 0.0 && p <= 1.0);
                CHECK(p >= lastProgress); // monotonic
                lastProgress = p;
            }
            (void)cur; (void)dur;
        },
        &cancel);
    CHECK(r.ok);
    if (!r.ok) std::cerr << "  error: " << r.error << "\n";
    CHECK(fileExists(out));
    CHECK(fileSize(out) > 1000);
    CHECK(fileMagic(out, 4) == "RIFF");
    CHECK(progressCalls > 0);
    std::cout << "[ok] wav->wav (" << fileSize(out) << " bytes, " << progressCalls << " progress cbs)\n";
}

static void testWavToFlac(const std::string& dir) {
    const std::string in = dir + "/in.wav";
    const std::string out = dir + "/out.flac";
    Transcoder tr;
    std::atomic<bool> cancel{false};
    TranscodeOptions o;
    o.inputPath = in;
    o.outputPath = out;
    o.format = "flac";
    TranscodeResult r = tr.transcode(o, nullptr, &cancel);
    CHECK(r.ok);
    if (!r.ok) std::cerr << "  error: " << r.error << "\n";
    CHECK(fileExists(out) && fileSize(out) > 1000);
    CHECK(fileMagic(out, 4) == "fLaC");
    std::cout << "[ok] wav->flac\n";
}

static void testWavToM4a(const std::string& dir) {
    const std::string in = dir + "/in.wav";
    const std::string out = dir + "/out.m4a";
    Transcoder tr;
    std::atomic<bool> cancel{false};
    TranscodeOptions o;
    o.inputPath = in;
    o.outputPath = out;
    o.format = "m4a";
    o.bitrate = "128k";
    o.sampleRate = 44100;
    o.channels = 2;
    TranscodeResult r = tr.transcode(o, nullptr, &cancel);
    CHECK(r.ok);
    if (!r.ok) std::cerr << "  error: " << r.error << "\n";
    CHECK(fileExists(out) && fileSize(out) > 1000);
    std::cout << "[ok] wav->m4a (aac)\n";
}

static void testResample(const std::string& dir) {
    const std::string in = dir + "/in.wav"; // 44100 Hz stereo
    const std::string out = dir + "/out_22k.wav";
    Transcoder tr;
    std::atomic<bool> cancel{false};
    TranscodeOptions o;
    o.inputPath = in;
    o.outputPath = out;
    o.format = "wav";
    o.sampleRate = 22050;
    o.channels = 1;
    TranscodeResult r = tr.transcode(o, nullptr, &cancel);
    CHECK(r.ok);
    if (!r.ok) std::cerr << "  error: " << r.error << "\n";
    // in.wav is 2.0s @44100/stereo -> 2.0s @22050/mono ~= 88244 bytes + header;
    // actual output is slightly larger due to resampler/encoder delay.
    const long sz = fileSize(out);
    CHECK(sz > 80000 && sz < 100000);
    std::cout << "[ok] resample 44100/stereo -> 22050/mono (" << sz << " bytes)\n";
}

static void testTrim(const std::string& dir) {
    const std::string in = dir + "/in.wav"; // 2.0s
    const std::string out = dir + "/out_trim.wav";
    Transcoder tr;
    std::atomic<bool> cancel{false};
    TranscodeOptions o;
    o.inputPath = in;
    o.outputPath = out;
    o.format = "wav";
    o.startTimeSec = 0.5;
    o.endTimeSec = 1.5;
    TranscodeResult r = tr.transcode(o, nullptr, &cancel);
    CHECK(r.ok);
    if (!r.ok) std::cerr << "  error: " << r.error << "\n";
    // ~1.0s * 44100 * 2ch * 2 bytes + 44 ~= 176444
    const long sz = fileSize(out);
    CHECK(sz > 150000 && sz < 200000);
    std::cout << "[ok] trim 0.5-1.5s (" << sz << " bytes)\n";
}

static void testMissingInput(const std::string& dir) {
    Transcoder tr;
    std::atomic<bool> cancel{false};
    TranscodeOptions o;
    o.inputPath = dir + "/does-not-exist.wav";
    o.outputPath = dir + "/out.wav";
    o.format = "wav";
    TranscodeResult r = tr.transcode(o, nullptr, &cancel);
    CHECK(!r.ok && !r.cancelled && !r.error.empty());
    std::cout << "[ok] missing input -> clean error\n";
}

static void testPreCancelled(const std::string& dir) {
    Transcoder tr;
    std::atomic<bool> cancel{true}; // already cancelled
    TranscodeOptions o;
    o.inputPath = dir + "/in.wav";
    o.outputPath = dir + "/out_cancel.wav";
    o.format = "wav";
    TranscodeResult r = tr.transcode(o, nullptr, &cancel);
    CHECK(!r.ok && r.cancelled);
    std::cout << "[ok] pre-cancelled -> cancelled result\n";
}

static void testCancelMidRun(const std::string& dir) {
    const std::string in = dir + "/long.wav";
    const std::string out = dir + "/out_midcancel.wav";
    writeSineWav(in, 20.0); // long enough to observe cancellation
    Transcoder tr;
    std::atomic<bool> cancel{false};
    TranscodeOptions o;
    o.inputPath = in;
    o.outputPath = out;
    o.format = "flac"; // slower encoder -> wider cancel window
    TranscodeResult r = tr.transcode(
        o,
        [&](double, double, double) { cancel.store(true); }, // cancel on first progress
        &cancel);
    CHECK(!r.ok && r.cancelled);
    std::cout << "[ok] mid-run cancel -> cancelled result\n";
}

#else // !HAVE_LIBAV

static void testStub() {
    CHECK(!Transcoder::isSupported());
    Transcoder tr;
    std::atomic<bool> cancel{false};
    TranscodeOptions o;
    o.inputPath = "/tmp/x.wav";
    o.outputPath = "/tmp/y.wav";
    o.format = "wav";
    TranscodeResult r = tr.transcode(o, nullptr, &cancel);
    CHECK(!r.ok && r.error.find("not supported") != std::string::npos);
    std::cout << "[ok] stub reports not supported\n";
}

#endif

int main() {
    std::cout << "Transcoder::isSupported() = " << (Transcoder::isSupported() ? "yes" : "no") << "\n";
    testResolveTarget();
    testParseBitrate();
    testValidation();
#ifdef HAVE_LIBAV
    CHECK(Transcoder::isSupported());
    const std::string dir = makeTempDir();
    std::cout << "temp dir: " << dir << "\n";
    testWavToWav(dir);
    testWavToFlac(dir);
    testWavToM4a(dir);
    testResample(dir);
    testTrim(dir);
    testMissingInput(dir);
    testPreCancelled(dir);
    testCancelMidRun(dir);
#else
    testStub();
#endif
    if (g_failed == 0) {
        std::cout << "\nALL TRANSCODE TESTS PASSED\n";
        return 0;
    }
    std::cerr << "\n" << g_failed << " CHECK(S) FAILED\n";
    return 1;
}
