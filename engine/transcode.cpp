/**
 * BBeBee Native Audio Engine - Transcode implementation.
 *
 * File-to-file audio transcoding using libavformat / libavcodec /
 * libswresample, performed inside the engine process (no external `ffmpeg`
 * binary needed). Standard pipeline:
 *
 *   demux -> decode -> resample -> encode -> mux
 *
 * with progress callbacks, trim support and cooperative cancellation.
 */
#include "transcode.hpp"

#ifdef HAVE_LIBAV
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}
#endif

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace audio_engine {

// ---------------------------------------------------------------------------
// Pure logic (no FFmpeg dependency; always compiled)
// ---------------------------------------------------------------------------

TranscodeTarget resolveTranscodeTarget(const std::string& format) {
    std::string f;
    f.reserve(format.size());
    for (char c : format) f.push_back((char)std::tolower((unsigned char)c));

    TranscodeTarget t;
    if (f == "mp3") {
        t.container = "mp3";
        t.encoders = {"libmp3lame"};
    } else if (f == "m4a" || f == "mp4") {
        t.container = "mp4";
        t.encoders = {"aac"};
    } else if (f == "aac") {
        t.container = "adts";
        t.encoders = {"aac"};
    } else if (f == "alac") {
        t.container = "mp4";
        t.encoders = {"alac"};
    } else if (f == "flac") {
        t.container = "flac";
        t.encoders = {"flac"};
    } else if (f == "ogg" || f == "oga" || f == "vorbis") {
        t.container = "ogg";
        t.encoders = {"libvorbis", "vorbis"};
    } else if (f == "opus") {
        t.container = "ogg";
        t.encoders = {"libopus", "opus"};
    } else if (f == "wav") {
        t.container = "wav";
        t.encoders = {"pcm_s16le"};
    } else if (f == "wma") {
        t.container = "asf";
        t.encoders = {"wmav2"};
    } else {
        t.error = "unsupported target format: '" + format + "'";
        return t;
    }
    t.valid = true;
    return t;
}

int parseBitrate(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    size_t j = s.size();
    while (j > i && std::isspace((unsigned char)s[j - 1])) --j;
    if (i >= j) return 0;

    double value = 0.0;
    size_t k = i;
    bool dot = false;
    while (k < j && (std::isdigit((unsigned char)s[k]) || (!dot && s[k] == '.'))) {
        if (s[k] == '.') dot = true;
        ++k;
    }
    if (k == i) return 0;
    try {
        value = std::stod(s.substr(i, k - i));
    } catch (...) {
        return 0;
    }
    long mult = 1;
    if (k < j) {
        char suffix = (char)std::tolower((unsigned char)s[k]);
        if (suffix == 'k') mult = 1000;
        else if (suffix == 'm') mult = 1000000;
        else return 0; // trailing garbage
        if (k + 1 != j) return 0;
    }
    double bps = value * mult;
    if (bps <= 0.0 || bps > 100000000.0) return 0;
    return (int)bps;
}

#ifdef HAVE_LIBAV

namespace {

// RAII helpers ---------------------------------------------------------------

struct FormatInputDeleter {
    void operator()(AVFormatContext* c) const {
        if (c) avformat_close_input(&c);
    }
};
struct FormatOutputDeleter {
    void operator()(AVFormatContext* c) const {
        if (c) {
            if (c->pb) avio_closep(&c->pb);
            avformat_free_context(c);
        }
    }
};
struct CodecCtxDeleter {
    void operator()(AVCodecContext* c) const {
        if (c) avcodec_free_context(&c);
    }
};
struct FrameDeleter {
    void operator()(AVFrame* f) const {
        if (f) av_frame_free(&f);
    }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const {
        if (p) av_packet_free(&p);
    }
};
struct SwrDeleter {
    void operator()(SwrContext* s) const {
        if (s) swr_free(&s);
    }
};
struct FifoDeleter {
    void operator()(AVAudioFifo* f) const {
        if (f) av_audio_fifo_free(f);
    }
};
struct ChannelLayoutHolder {
    AVChannelLayout layout;
    ChannelLayoutHolder() { std::memset(&layout, 0, sizeof(layout)); }
    ~ChannelLayoutHolder() { av_channel_layout_uninit(&layout); }
    ChannelLayoutHolder(const ChannelLayoutHolder&) = delete;
    ChannelLayoutHolder& operator=(const ChannelLayoutHolder&) = delete;
};

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) out += ", ";
        out += names[i];
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Transcoder
// ---------------------------------------------------------------------------

bool Transcoder::isSupported() {
    return true;
}

TranscodeResult Transcoder::transcode(const TranscodeOptions& opts,
                                      TranscodeProgressCb progressCb,
                                      const std::atomic<bool>* cancelFlag) {
    TranscodeResult result;
    auto fail = [&](const std::string& msg) {
        result.ok = false;
        result.error = msg;
        return result;
    };
    auto cancelled = [&]() {
        result.ok = false;
        result.cancelled = true;
        result.error = "cancelled";
        return result;
    };
    auto isCancelled = [&]() {
        return cancelFlag && cancelFlag->load(std::memory_order_relaxed);
    };

    if (opts.inputPath.empty()) return fail("empty input path");
    if (opts.outputPath.empty()) return fail("empty output path");

    TranscodeTarget target = resolveTranscodeTarget(opts.format);
    if (!target.valid) return fail(target.error);

    const AVCodec* encoder = nullptr;
    for (const auto& name : target.encoders) {
        encoder = avcodec_find_encoder_by_name(name.c_str());
        if (encoder) break;
    }
    if (!encoder) {
        return fail("no encoder available for format '" + opts.format +
                    "' (tried: " + joinNames(target.encoders) +
                    "); rebuild FFmpeg with the encoder enabled");
    }

    // --- Open input ---
    AVFormatContext* ifmtRaw = nullptr;
    if (avformat_open_input(&ifmtRaw, opts.inputPath.c_str(), nullptr, nullptr) < 0)
        return fail("cannot open input: " + opts.inputPath);
    std::unique_ptr<AVFormatContext, FormatInputDeleter> ifmt(ifmtRaw);

    if (avformat_find_stream_info(ifmt.get(), nullptr) < 0)
        return fail("cannot read stream info: " + opts.inputPath);

    const AVCodec* decoder = nullptr;
    int audioIdx = av_find_best_stream(ifmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (audioIdx < 0 || !decoder)
        return fail("no audio stream in: " + opts.inputPath);
    AVStream* inStream = ifmt->streams[audioIdx];

    std::unique_ptr<AVCodecContext, CodecCtxDeleter> decCtx(avcodec_alloc_context3(decoder));
    if (!decCtx) return fail("cannot allocate decoder context");
    if (avcodec_parameters_to_context(decCtx.get(), inStream->codecpar) < 0)
        return fail("cannot copy decoder parameters");
    if (avcodec_open2(decCtx.get(), decoder, nullptr) < 0)
        return fail(std::string("cannot open decoder: ") + (decoder->name ? decoder->name : "?"));

    const double inTimeBase = av_q2d(inStream->time_base);
    double durationSec = 0.0;
    if (ifmt->duration != AV_NOPTS_VALUE && ifmt->duration > 0)
        durationSec = (double)ifmt->duration / AV_TIME_BASE;
    if (opts.endTimeSec > 0.0 && (durationSec <= 0.0 || opts.endTimeSec < durationSec))
        durationSec = opts.endTimeSec - opts.startTimeSec;

    // --- Configure encoder ---
    std::unique_ptr<AVCodecContext, CodecCtxDeleter> encCtx(avcodec_alloc_context3(encoder));
    if (!encCtx) return fail("cannot allocate encoder context");

    const int outSampleRate = opts.sampleRate > 0 ? opts.sampleRate : decCtx->sample_rate;
    encCtx->sample_rate = outSampleRate > 0 ? outSampleRate : 44100;

    // Normalize channel layouts: demuxers may report an UNSPEC layout
    // ("N channels" with no defined positions), which encoders like aac
    // reject. Fall back to the default layout for the channel count.
    const int inNbChannels = decCtx->ch_layout.nb_channels > 0 ? decCtx->ch_layout.nb_channels : 2;
    const int outNbChannels = opts.channels > 0 ? opts.channels : inNbChannels;
    ChannelLayoutHolder inLayoutHolder;
    AVChannelLayout& inLayout = inLayoutHolder.layout;
    if (decCtx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC && decCtx->ch_layout.nb_channels > 0) {
        if (av_channel_layout_copy(&inLayout, &decCtx->ch_layout) < 0)
            return fail("cannot copy channel layout");
    } else {
        av_channel_layout_default(&inLayout, inNbChannels);
    }
    av_channel_layout_default(&encCtx->ch_layout, outNbChannels);
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const enum AVSampleFormat *sampleFormats = nullptr;
    int nbSampleFormats = 0;
    avcodec_get_supported_config(encCtx.get(), encoder, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                 0, (const void **)&sampleFormats, &nbSampleFormats);
    encCtx->sample_fmt = (sampleFormats && nbSampleFormats > 0) ? sampleFormats[0] : AV_SAMPLE_FMT_FLTP;
#else
    encCtx->sample_fmt = encoder->sample_fmts ? encoder->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
#endif
    encCtx->time_base = AVRational{1, encCtx->sample_rate};
    const int bitrate = parseBitrate(opts.bitrate);
    if (bitrate > 0) encCtx->bit_rate = bitrate;

    if (avcodec_open2(encCtx.get(), encoder, nullptr) < 0) {
        av_channel_layout_uninit(&encCtx->ch_layout);
        return fail(std::string("cannot open encoder: ") + (encoder->name ? encoder->name : "?"));
    }

    // --- Resampler (always via swr; passthrough when formats match) ---
    std::unique_ptr<SwrContext, SwrDeleter> swr(swr_alloc());
    if (!swr) return fail("cannot allocate resampler");
    {
        SwrContext* s = swr.get();
        int r = 0;
        r |= av_opt_set_chlayout(s, "in_chlayout", &inLayout, 0);
        r |= av_opt_set_int(s, "in_sample_rate", decCtx->sample_rate, 0);
        r |= av_opt_set_sample_fmt(s, "in_sample_fmt", decCtx->sample_fmt, 0);
        r |= av_opt_set_chlayout(s, "out_chlayout", &encCtx->ch_layout, 0);
        r |= av_opt_set_int(s, "out_sample_rate", encCtx->sample_rate, 0);
        r |= av_opt_set_sample_fmt(s, "out_sample_fmt", encCtx->sample_fmt, 0);
        if (r < 0 || swr_init(s) < 0) {
            return fail("cannot initialize resampler");
        }
    }

    // --- Output muxer ---
    AVFormatContext* ofmtRaw = nullptr;
    if (avformat_alloc_output_context2(&ofmtRaw, nullptr, target.container.c_str(),
                                        opts.outputPath.c_str()) < 0 ||
        !ofmtRaw)
        return fail("cannot allocate output context for: " + target.container);
    std::unique_ptr<AVFormatContext, FormatOutputDeleter> ofmt(ofmtRaw);

    AVStream* outStream = avformat_new_stream(ofmt.get(), nullptr);
    if (!outStream) return fail("cannot create output stream");
    if (avcodec_parameters_from_context(outStream->codecpar, encCtx.get()) < 0)
        return fail("cannot copy encoder parameters");
    outStream->time_base = encCtx->time_base;

    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt->pb, opts.outputPath.c_str(), AVIO_FLAG_WRITE) < 0)
            return fail("cannot open output: " + opts.outputPath);
    }
    if (avformat_write_header(ofmt.get(), nullptr) < 0)
        return fail("cannot write output header: " + opts.outputPath);

    // --- Seek for trim start ---
    if (opts.startTimeSec > 0.0) {
        const int64_t ts = (int64_t)(opts.startTimeSec * AV_TIME_BASE);
        if (avformat_seek_file(ifmt.get(), -1, INT64_MIN, ts, INT64_MAX, 0) >= 0)
            avcodec_flush_buffers(decCtx.get());
    }

    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> filtFrame(av_frame_alloc());
    std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
    std::unique_ptr<AVPacket, PacketDeleter> encPkt(av_packet_alloc());
    if (!frame || !filtFrame || !pkt || !encPkt) return fail("cannot allocate frame/packet");

    std::unique_ptr<AVAudioFifo, FifoDeleter> fifo(
        av_audio_fifo_alloc(encCtx->sample_fmt, encCtx->ch_layout.nb_channels, 1024));
    if (!fifo) return fail("cannot allocate audio fifo");

    const int encFrameSize = encCtx->frame_size > 0 ? encCtx->frame_size : 1024;
    int64_t nextPts = 0;
    auto lastProgress = std::chrono::steady_clock::now();
    double lastReported = -1.0;

    auto reportProgress = [&](double currentSec) {
        if (!progressCb) return;
        double p = -1.0;
        if (durationSec > 0.0 && currentSec >= 0.0)
            p = std::min(1.0, std::max(0.0, currentSec / durationSec));
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastProgress).count();
        if (p >= 1.0 || std::fabs(p - lastReported) >= 0.02 || dt >= 0.5) {
            lastProgress = now;
            lastReported = p;
            progressCb(p, currentSec, durationSec);
        }
    };

    // Encode + write everything currently in the fifo.
    auto drainFifo = [&](bool& okFlag) {
        okFlag = true;
        while (av_audio_fifo_size(fifo.get()) >= encFrameSize) {
            if (isCancelled()) return;
            av_frame_unref(filtFrame.get());
            filtFrame->nb_samples = encFrameSize;
            filtFrame->format = encCtx->sample_fmt;
            filtFrame->sample_rate = encCtx->sample_rate;
            if (av_channel_layout_copy(&filtFrame->ch_layout, &encCtx->ch_layout) < 0) { okFlag = false; return; }
            if (av_frame_get_buffer(filtFrame.get(), 0) < 0) { okFlag = false; return; }
            if (av_audio_fifo_read(fifo.get(), (void**)filtFrame->data, encFrameSize) < encFrameSize) { okFlag = false; return; }
            filtFrame->pts = nextPts;
            nextPts += filtFrame->nb_samples;
            if (avcodec_send_frame(encCtx.get(), filtFrame.get()) < 0) { okFlag = false; return; }
            while (avcodec_receive_packet(encCtx.get(), encPkt.get()) == 0) {
                av_packet_rescale_ts(encPkt.get(), encCtx->time_base, outStream->time_base);
                encPkt->stream_index = outStream->index;
                if (av_interleaved_write_frame(ofmt.get(), encPkt.get()) < 0) { okFlag = false; return; }
                av_packet_unref(encPkt.get());
            }
        }
    };

    // Convert one decoded frame through swr into the fifo.
    auto pushFrame = [&](bool& okFlag) {
        okFlag = true;
        const int dstNb = (int)av_rescale_rnd(
            swr_get_delay(swr.get(), decCtx->sample_rate) + frame->nb_samples,
            encCtx->sample_rate, decCtx->sample_rate, AV_ROUND_UP);
        uint8_t** conv = nullptr;
        int convLinesize = 0;
        if (av_samples_alloc_array_and_samples(&conv, &convLinesize, encCtx->ch_layout.nb_channels,
                                               dstNb, encCtx->sample_fmt, 0) < 0) { okFlag = false; return; }
        const int got = swr_convert(swr.get(), conv, dstNb,
                                    (const uint8_t**)frame->data, frame->nb_samples);
        if (got < 0) { av_freep(&conv[0]); av_freep(&conv); okFlag = false; return; }
        if (av_audio_fifo_write(fifo.get(), (void**)conv, got) < got) { okFlag = false; }
        av_freep(&conv[0]);
        av_freep(&conv);
    };

    bool okFlag = true;

    // --- Main demux/decode loop ---
    for (;;) {
        if (isCancelled()) return cancelled();
        int r = av_read_frame(ifmt.get(), pkt.get());
        if (r < 0) break; // EOF or error; flush below
        if (pkt->stream_index != audioIdx) { av_packet_unref(pkt.get()); continue; }

        const double ptsSec = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * inTimeBase : -1.0;
        if (opts.endTimeSec > 0.0 && ptsSec >= 0.0 && ptsSec >= opts.endTimeSec) {
            av_packet_unref(pkt.get());
            break;
        }
        reportProgress(ptsSec);

        if (avcodec_send_packet(decCtx.get(), pkt.get()) < 0) { av_packet_unref(pkt.get()); continue; }
        av_packet_unref(pkt.get());
        while (avcodec_receive_frame(decCtx.get(), frame.get()) == 0) {
            if (isCancelled()) return cancelled();
            pushFrame(okFlag);
            if (!okFlag) return fail("resample/convert failed");
            drainFifo(okFlag);
            if (!okFlag) return fail("encode failed");
            av_frame_unref(frame.get());
        }
    }

    // --- Flush decoder ---
    avcodec_send_packet(decCtx.get(), nullptr);
    while (avcodec_receive_frame(decCtx.get(), frame.get()) == 0) {
        pushFrame(okFlag);
        if (!okFlag) return fail("resample/convert failed (flush)");
        drainFifo(okFlag);
        if (!okFlag) return fail("encode failed (flush)");
        av_frame_unref(frame.get());
    }

    // --- Flush resampler ---
    for (;;) {
        uint8_t** conv = nullptr;
        int convLinesize = 0;
        const int dstNb = 1024;
        if (av_samples_alloc_array_and_samples(&conv, &convLinesize, encCtx->ch_layout.nb_channels,
                                               dstNb, encCtx->sample_fmt, 0) < 0)
            return fail("cannot allocate flush buffer");
        const int got = swr_convert(swr.get(), conv, dstNb, nullptr, 0);
        if (got > 0) {
            if (av_audio_fifo_write(fifo.get(), (void**)conv, got) < got) {
                av_freep(&conv[0]); av_freep(&conv);
                return fail("fifo write failed (flush)");
            }
        }
        av_freep(&conv[0]);
        av_freep(&conv);
        if (got <= 0) break;
        drainFifo(okFlag);
        if (!okFlag) return fail("encode failed (resample flush)");
    }

    // --- Drain remaining fifo (partial frame, zero-padded) ---
    {
        const int remaining = av_audio_fifo_size(fifo.get());
        if (remaining > 0) {
            av_frame_unref(filtFrame.get());
            filtFrame->nb_samples = encFrameSize; // allocate full, pad the tail
            filtFrame->format = encCtx->sample_fmt;
            filtFrame->sample_rate = encCtx->sample_rate;
            if (av_channel_layout_copy(&filtFrame->ch_layout, &encCtx->ch_layout) < 0)
                return fail("cannot set frame channel layout");
            if (av_frame_get_buffer(filtFrame.get(), 0) < 0) return fail("cannot allocate frame buffer");
            if (av_audio_fifo_read(fifo.get(), (void**)filtFrame->data, remaining) < remaining)
                return fail("fifo read failed");
            if (remaining < encFrameSize &&
                av_samples_set_silence(filtFrame->data, remaining, encFrameSize - remaining,
                                       encCtx->ch_layout.nb_channels, encCtx->sample_fmt) < 0)
                return fail("cannot pad final frame");
            filtFrame->pts = nextPts;
            nextPts += encFrameSize;
            if (avcodec_send_frame(encCtx.get(), filtFrame.get()) < 0) return fail("encode failed (final)");
            while (avcodec_receive_packet(encCtx.get(), encPkt.get()) == 0) {
                av_packet_rescale_ts(encPkt.get(), encCtx->time_base, outStream->time_base);
                encPkt->stream_index = outStream->index;
                if (av_interleaved_write_frame(ofmt.get(), encPkt.get()) < 0) return fail("mux failed (final)");
                av_packet_unref(encPkt.get());
            }
        }
    }

    // --- Flush encoder ---
    avcodec_send_frame(encCtx.get(), nullptr);
    while (avcodec_receive_packet(encCtx.get(), encPkt.get()) == 0) {
        av_packet_rescale_ts(encPkt.get(), encCtx->time_base, outStream->time_base);
        encPkt->stream_index = outStream->index;
        if (av_interleaved_write_frame(ofmt.get(), encPkt.get()) < 0) return fail("mux failed (encoder flush)");
        av_packet_unref(encPkt.get());
    }

    if (av_write_trailer(ofmt.get()) < 0) return fail("cannot write trailer");

    reportProgress(durationSec > 0.0 ? durationSec : -1.0);
    result.ok = true;
    return result;
}

#else // !HAVE_LIBAV

bool Transcoder::isSupported() {
    return false;
}

TranscodeResult Transcoder::transcode(const TranscodeOptions&,
                                      TranscodeProgressCb,
                                      const std::atomic<bool>*) {
    TranscodeResult r;
    r.ok = false;
    r.error = "transcode not supported in this build (FFmpeg unavailable)";
    return r;
}

#endif // HAVE_LIBAV

} // namespace audio_engine
