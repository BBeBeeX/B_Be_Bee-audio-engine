#ifndef BBEBEE_TRANSCODE_CAPS_HPP
#define BBEBEE_TRANSCODE_CAPS_HPP

#include <map>
#include <string>

namespace bbebee {
namespace jni {

/*
 * Encoder availability as the JS layer consumes it — a plain
 * encoder-id → present map keyed by the same ids the desktop
 * `getEncoderCapabilities` uses (libmp3lame, flac, aac, libopus, libvorbis,
 * wmav2, alac, pcm_s16le, pcm_s24le, pcm_s16be). Name/description/format
 * derivation stays in TypeScript; the probe only answers what actually
 * exists in the linked libavcodec.
 *
 * Deliberately free of libav types: the JNI translation unit includes this
 * header, and this host has no FFmpeg SDK — only transcode_caps.cpp
 * (compiled solely under HAVE_LIBAV) sees the real headers.
 */
struct TranscodeCaps {
    /** True when the library was built with HAVE_LIBAV at all. */
    bool supported = false;
    std::map<std::string, bool> encoders;
};

/** Probe the linked libavcodec for every encoder id the contract knows. */
TranscodeCaps probeTranscodeCaps();

} // namespace jni
} // namespace bbebee

#endif // BBEBEE_TRANSCODE_CAPS_HPP
