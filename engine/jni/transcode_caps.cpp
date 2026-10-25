#include "transcode_caps.hpp"

#ifdef HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

namespace bbebee {
namespace jni {

namespace {

/*
 * The exact id set the desktop `getEncoderCapabilities` reports
 * (packages/core/core-codec-node — `knownEncoders`). Availability is probed
 * rather than assumed: an encoder the FFmpeg build left out answers false,
 * which is the honesty rule the converter UI's promise rests on.
 */
const char* kKnownEncoders[] = {
    "libmp3lame",
    "flac",
    "aac",
    "libopus",
    "libvorbis",
    "wmav2",
    "alac",
    "pcm_s16le",
    "pcm_s24le",
    "pcm_s16be",
};

} // namespace

TranscodeCaps probeTranscodeCaps() {
    TranscodeCaps caps;
#ifdef HAVE_LIBAV
    caps.supported = true;
    for (const char* id : kKnownEncoders) {
        caps.encoders[id] = avcodec_find_encoder_by_name(id) != nullptr;
    }
#endif
    return caps;
}

} // namespace jni
} // namespace bbebee
