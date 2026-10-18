/**
 * BBeBee Native Audio Engine - Transcode (libav*) support.
 *
 * Provides file-to-file audio transcoding using the FFmpeg libraries that are
 * already loaded alongside libmpv (libavformat / libavcodec / libavutil /
 * libswresample). This replaces the need to ship a separate `ffmpeg` CLI
 * binary: the engine process performs demux -> decode -> resample -> encode
 * -> mux directly.
 *
 * Build note: this translation unit requires FFmpeg development headers.
 * engine/CMakeLists.txt probes for them via pkg-config and defines HAVE_LIBAV.
 * Without HAVE_LIBAV a stub implementation is compiled that reports
 * "not supported" at runtime, so the engine still builds everywhere.
 */
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace audio_engine {

// Transcode job options. Mirrors the JSON action fields accepted by the
// engine's "transcode" action (see main.cpp).
struct TranscodeOptions {
    std::string jobId;
    std::string inputPath;    // native filesystem path (URI conversion happens upstream)
    std::string outputPath;   // native filesystem path
    std::string format = "mp3"; // target format key: mp3|m4a|aac|flac|ogg|opus|wav|alac|wma
    std::string bitrate;      // e.g. "192k", "192000", "" = encoder default
    int sampleRate = 0;       // 0 = keep source sample rate
    int channels = 0;         // 0 = keep source channel count
    double startTimeSec = 0.0;// trim start (seconds)
    double endTimeSec = 0.0;  // trim end (seconds), 0 = until end of input
};

struct TranscodeResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
};

// Resolved container muxer + encoder candidates for a format key.
// Pure logic, no FFmpeg dependency.
struct TranscodeTarget {
    bool valid = false;
    std::string container;              // libavformat muxer name
    std::vector<std::string> encoders;  // tried in order via avcodec_find_encoder_by_name
    std::string error;
};

// Map a user-facing format key to its container + encoder candidates.
// Case-insensitive. Never touches FFmpeg; safe to unit test anywhere.
TranscodeTarget resolveTranscodeTarget(const std::string& format);

// Parse "192k"/"192K"/"1.5M"/"192000"/"" into bits-per-second. Returns 0 for
// empty or unparseable input (caller treats 0 as "encoder default").
int parseBitrate(const std::string& s);

// Progress callback: progress01 in [0,1], currentSec/durationSec in seconds.
using TranscodeProgressCb = std::function<void(double progress01, double currentSec, double durationSec)>;

// Synchronous, blocking transcoder. No shared state between instances;
// run one per job on a worker thread. Progress is reported through
// progressCb (may be invoked from the calling thread only).
class Transcoder {
public:
    // True when built with FFmpeg support (HAVE_LIBAV).
    static bool isSupported();

    TranscodeResult transcode(const TranscodeOptions& opts,
                              TranscodeProgressCb progressCb,
                              const std::atomic<bool>* cancelFlag);
};

} // namespace audio_engine
