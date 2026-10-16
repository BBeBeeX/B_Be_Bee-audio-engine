/**
 * Standalone Native audio-engine Process.
 *
 * Runs as an independent native binary executable for crash isolation.
 * - Manages libmpv instance with direct WASAPI output (ao=wasapi)
 * - Implements in-engine DSP / 10-band EQ / Preamp / Compressor filter chain
 * - Real-time metadata / audio level analysis ("Zero-IPC for PCM")
 * - Dispatches events, position updates, and FFT spectrum frames over stdio JSON-IPC
 */

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cctype>
#include <algorithm>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static std::string getExecutableDir() {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string s(path, len);
        size_t pos = s.find_last_of("\\/");
        if (pos != std::string::npos) {
            return s.substr(0, pos);
        }
    }
    return "";
}
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <unistd.h>
#include <libgen.h>
#include <mach-o/dyld.h>
static std::string getExecutableDir() {
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        char* dir = dirname(path);
        if (dir) return std::string(dir);
    }
    return "";
}
#else
#include <dlfcn.h>
#include <unistd.h>
#include <libgen.h>
static std::string getExecutableDir() {
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1) {
        path[len] = '\0';
        char* dir = dirname(path);
        if (dir) return std::string(dir);
    }
    return "";
}
#endif

#include "mpv_client.h"
#include "pcm_ring_buffer.hpp"
#include "fft.hpp"
#include "json.hpp"

using namespace audio_engine;

// Dynamic libmpv function table
struct MpvDynLib {
    void* handle = nullptr;
    fn_mpv_create create = nullptr;
    fn_mpv_initialize initialize = nullptr;
    fn_mpv_destroy destroy = nullptr;
    fn_mpv_terminate_destroy terminate_destroy = nullptr;
    fn_mpv_command command = nullptr;
    fn_mpv_command_string command_string = nullptr;
    fn_mpv_set_option set_option = nullptr;
    fn_mpv_set_option_string set_option_string = nullptr;
    fn_mpv_get_property get_property = nullptr;
    fn_mpv_set_property set_property = nullptr;
    fn_mpv_set_property_string set_property_string = nullptr;
    fn_mpv_observe_property observe_property = nullptr;
    fn_mpv_wait_event wait_event = nullptr;
    fn_mpv_error_string error_string = nullptr;
    fn_mpv_free free_data = nullptr;
    fn_mpv_request_log_messages request_log_messages = nullptr;
    fn_mpv_set_pcm_callback set_pcm_callback = nullptr;

    bool load(const std::string& customPath = "") {
        std::vector<std::string> candidates;
        if (!customPath.empty()) candidates.push_back(customPath);

        std::string exeDir = getExecutableDir();

#if defined(_WIN32)
        if (!exeDir.empty()) {
            candidates.push_back(exeDir + "\\mpv-2.dll");
            candidates.push_back(exeDir + "\\libmpv-2.dll");
            candidates.push_back(exeDir + "\\mpv-1.dll");
        }
        // Note: bare names (e.g. "mpv-2.dll") are omitted to prevent DLL hijacking via CWD/PATH
#elif defined(__APPLE__)
        if (!exeDir.empty()) {
            candidates.push_back(exeDir + "/libmpv.2.dylib");
            candidates.push_back(exeDir + "/libmpv.dylib");
        }
        // System standard locations only
        candidates.push_back("/usr/local/lib/libmpv.dylib");
        candidates.push_back("/opt/homebrew/lib/libmpv.dylib");
#else
        if (!exeDir.empty()) {
            candidates.push_back(exeDir + "/libmpv.so.2");
            candidates.push_back(exeDir + "/libmpv.so.1");
            candidates.push_back(exeDir + "/libmpv.so");
        }
        // System standard locations only
        candidates.push_back("/usr/lib/libmpv.so.2");
        candidates.push_back("/usr/lib/x86_64-linux-gnu/libmpv.so.2");
        candidates.push_back("/usr/lib64/libmpv.so.2");
#endif

        for (const auto& path : candidates) {
#if defined(_WIN32)
            DWORD searchFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
            HMODULE h = LoadLibraryExA(path.c_str(), NULL, searchFlags);
            if (h) {
                handle = reinterpret_cast<void*>(h);
                create = reinterpret_cast<fn_mpv_create>(GetProcAddress(h, "mpv_create"));
                initialize = reinterpret_cast<fn_mpv_initialize>(GetProcAddress(h, "mpv_initialize"));
                destroy = reinterpret_cast<fn_mpv_destroy>(GetProcAddress(h, "mpv_destroy"));
                terminate_destroy = reinterpret_cast<fn_mpv_terminate_destroy>(GetProcAddress(h, "mpv_terminate_destroy"));
                command = reinterpret_cast<fn_mpv_command>(GetProcAddress(h, "mpv_command"));
                command_string = reinterpret_cast<fn_mpv_command_string>(GetProcAddress(h, "mpv_command_string"));
                set_option = reinterpret_cast<fn_mpv_set_option>(GetProcAddress(h, "mpv_set_option"));
                set_option_string = reinterpret_cast<fn_mpv_set_option_string>(GetProcAddress(h, "mpv_set_option_string"));
                get_property = reinterpret_cast<fn_mpv_get_property>(GetProcAddress(h, "mpv_get_property"));
                set_property = reinterpret_cast<fn_mpv_set_property>(GetProcAddress(h, "mpv_set_property"));
                set_property_string = reinterpret_cast<fn_mpv_set_property_string>(GetProcAddress(h, "mpv_set_property_string"));
                observe_property = reinterpret_cast<fn_mpv_observe_property>(GetProcAddress(h, "mpv_observe_property"));
                wait_event = reinterpret_cast<fn_mpv_wait_event>(GetProcAddress(h, "mpv_wait_event"));
                error_string = reinterpret_cast<fn_mpv_error_string>(GetProcAddress(h, "mpv_error_string"));
                free_data = reinterpret_cast<fn_mpv_free>(GetProcAddress(h, "mpv_free"));
                request_log_messages = reinterpret_cast<fn_mpv_request_log_messages>(GetProcAddress(h, "mpv_request_log_messages"));
                set_pcm_callback = reinterpret_cast<fn_mpv_set_pcm_callback>(GetProcAddress(h, "mpv_set_pcm_callback"));
                if (create && initialize) return true;
                FreeLibrary(h);
                handle = nullptr;
            }
#else
            void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (h) {
                handle = h;
                create = reinterpret_cast<fn_mpv_create>(dlsym(h, "mpv_create"));
                initialize = reinterpret_cast<fn_mpv_initialize>(dlsym(h, "mpv_initialize"));
                destroy = reinterpret_cast<fn_mpv_destroy>(dlsym(h, "mpv_destroy"));
                terminate_destroy = reinterpret_cast<fn_mpv_terminate_destroy>(dlsym(h, "mpv_terminate_destroy"));
                command = reinterpret_cast<fn_mpv_command>(dlsym(h, "mpv_command"));
                command_string = reinterpret_cast<fn_mpv_command_string>(dlsym(h, "mpv_command_string"));
                set_option = reinterpret_cast<fn_mpv_set_option>(dlsym(h, "mpv_set_option"));
                set_option_string = reinterpret_cast<fn_mpv_set_option_string>(dlsym(h, "mpv_set_option_string"));
                get_property = reinterpret_cast<fn_mpv_get_property>(dlsym(h, "mpv_get_property"));
                set_property = reinterpret_cast<fn_mpv_set_property>(dlsym(h, "mpv_set_property"));
                set_property_string = reinterpret_cast<fn_mpv_set_property_string>(dlsym(h, "mpv_set_property_string"));
                observe_property = reinterpret_cast<fn_mpv_observe_property>(dlsym(h, "mpv_observe_property"));
                wait_event = reinterpret_cast<fn_mpv_wait_event>(dlsym(h, "mpv_wait_event"));
                error_string = reinterpret_cast<fn_mpv_error_string>(dlsym(h, "mpv_error_string"));
                free_data = reinterpret_cast<fn_mpv_free>(dlsym(h, "mpv_free"));
                request_log_messages = reinterpret_cast<fn_mpv_request_log_messages>(dlsym(h, "mpv_request_log_messages"));
                set_pcm_callback = reinterpret_cast<fn_mpv_set_pcm_callback>(dlsym(h, "mpv_set_pcm_callback"));
                if (create && initialize) return true;
                dlclose(h);
                handle = nullptr;
            }
#endif
        }

        std::cerr << "[audio-engine] Error: Unable to locate or load libmpv from restricted secure paths:\n";
        for (const auto& p : candidates) {
            std::cerr << "  - checked: " << p << "\n";
        }
        return false;
    }

    void unload() {
        if (handle) {
#if defined(_WIN32)
            FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
            dlclose(handle);
#endif
            handle = nullptr;
        }
    }
};

// URI → on-disk path, mirroring the main process's fs-path.ts: the renderer
// hands out bbebee-file:// / file:// URLs whose on-disk names are
// percent-encoded UTF-8, and mpv understands neither the custom scheme nor
// the encoding (a DSF under a Chinese-named folder arrives as
// "bbebee-file:///F:/music/%E5%B7%B4%E8%B5%AB..."). http(s) and other
// schemes pass through untouched.
static std::string uriToMpvPath(const std::string& uri) {
    const size_t scheme = uri.find("://");
    if (scheme == std::string::npos) return uri;
    const std::string schemeName = uri.substr(0, scheme);
    if (schemeName != "file" && schemeName != "bbebee-file") return uri;

    std::string rest = uri.substr(scheme + 3);
    std::string decoded;
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            const int hi = hexVal(rest[i + 1]);
            const int lo = hexVal(rest[i + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        decoded += rest[i];
    }

#if defined(_WIN32)
    // file:///F:/music → /F:/music → F:/music (the drive-letter quirk
    // fileURLToPath also handles on the main-process side)
    if (decoded.size() >= 3 && decoded[0] == '/' && decoded[2] == ':') decoded = decoded.substr(1);
#endif
    return decoded;
}

class AudioEngineApp {
public:
    AudioEngineApp() : fftProcessor({ 128, -100.0f, -30.0f, 0.8f }) {}

    ~AudioEngineApp() {
        shutdown();
    }

    void init(const JsonValue& config) {
        if (running.exchange(true)) {
            std::cerr << "[audio-engine] Warning: init called on already running engine, ignoring reentrant init\n";
            return;
        }

        fftSize = config.get("fftSize").asInt(128);
        {
            std::lock_guard<std::mutex> fLock(fftMutex);
            fftProcessor.setFftSize(fftSize);
        }
        deviceId = config.get("deviceId").asString("default");
        audioExclusive = config.get("audioExclusive").asBool(false);
        std::string customAo = config.get("ao").asString("");

        bool hasMpv = mpvLib.load();
        if (hasMpv && mpvLib.create && mpvLib.initialize) {
            mpv = mpvLib.create();
            if (mpv) {
                // Configure WASAPI or platform ao
                if (!customAo.empty()) {
                    mpvLib.set_option_string(mpv, "ao", customAo.c_str());
                } else {
#if defined(_WIN32)
                    mpvLib.set_option_string(mpv, "ao", "wasapi");
#elif defined(__APPLE__)
                    mpvLib.set_option_string(mpv, "ao", "coreaudio");
#else
                    mpvLib.set_option_string(mpv, "ao", "pulse,alsa,pipewire");
#endif
                }

                if (deviceId != "default" && !deviceId.empty()) {
                    mpvLib.set_option_string(mpv, "audio-device", deviceId.c_str());
                }

                if (audioExclusive) {
                    mpvLib.set_option_string(mpv, "audio-exclusive", "yes");
                }

                if (config.has("replaygain")) {
                    std::string rg = config.get("replaygain").asString("no");
                    mpvLib.set_option_string(mpv, "replaygain", rg.c_str());
                }
                if (config.has("replaygainClip")) {
                    bool clip = config.get("replaygainClip").asBool(true);
                    mpvLib.set_option_string(mpv, "replaygain-clip", clip ? "yes" : "no");
                }
                if (config.has("replaygainPreamp")) {
                    std::string preamp = config.get("replaygainPreamp").asString("0");
                    mpvLib.set_option_string(mpv, "replaygain-preamp", preamp.c_str());
                }
                if (config.has("replaygainFallback")) {
                    std::string fallback = config.get("replaygainFallback").asString("0");
                    mpvLib.set_option_string(mpv, "replaygain-fallback", fallback.c_str());
                }

                mpvLib.set_option_string(mpv, "keep-open", "yes");
                mpvLib.set_option_string(mpv, "idle", "yes");
                mpvLib.set_option_string(mpv, "video", "no");
                mpvLib.set_option_string(mpv, "audio-pitch-correction", "yes");

                int initRes = mpvLib.initialize(mpv);
                if (initRes < 0) {
                    std::cerr << "[audio-engine] Error: mpv_initialize failed with error code "
                              << initRes << " (" << (mpvLib.error_string ? mpvLib.error_string(initRes) : "unknown")
                              << ")\n";
                    if (mpvLib.destroy) {
                        mpvLib.destroy(mpv);
                    }
                    mpv = nullptr;
                } else {
                    // Surface mpv's own diagnostics: without this an END_FILE
                    // error is just a code, and AO/DSD failures are undiagnosable.
                    if (mpvLib.request_log_messages) mpvLib.request_log_messages(mpv, "warn");

                    // Observe real mpv properties for exact playback tracking
                    if (mpvLib.observe_property) {
                        mpvLib.observe_property(mpv, 1, "time-pos", MPV_FORMAT_DOUBLE);
                        mpvLib.observe_property(mpv, 2, "duration", MPV_FORMAT_DOUBLE);
                        mpvLib.observe_property(mpv, 3, "pause", MPV_FORMAT_FLAG);
                        mpvLib.observe_property(mpv, 4, "eof-reached", MPV_FORMAT_FLAG);
                        // audio-out-params, NOT audio-params: the PCM tap fires at
                        // the AO boundary and delivers the device format (mpv
                        // resamples/mixes between the two), while audio-params
                        // describes the decoder output. A 44.1 kHz file on a 48 kHz
                        // device (or a mono file upmixed to stereo) reported here
                        // as audio-params makes the ring buffer's format-mismatch
                        // guard drop every tapped frame — a permanently flat
                        // visualizer despite a working tap.
                        mpvLib.observe_property(mpv, 5, "audio-out-params/samplerate", MPV_FORMAT_INT64);
                        mpvLib.observe_property(mpv, 6, "audio-out-params/channel-count", MPV_FORMAT_INT64);
                        mpvLib.observe_property(mpv, 7, "af-metadata", MPV_FORMAT_STRING);
                        mpvLib.observe_property(mpv, 8, "af-metadata/bbebee_astats", MPV_FORMAT_STRING);
                    }

                    // Initial audio filter with astats metadata tap
                    applyFilterGraph("");

                    // Register real-time PCM tap callback if supported by mpv build
                    if (mpvLib.set_pcm_callback) {
                        mpvLib.set_pcm_callback(mpv, onMpvPcmCallback, this);
                        std::cerr << "[audio-engine] libmpv PCM tap callback registered successfully\n";
                    } else {
                        std::cerr << "[audio-engine] Warning: libmpv does not export mpv_set_pcm_callback (unpatched mpv). Visualizer PCM tap disabled.\n";
                    }

                    std::cerr << "[audio-engine] libmpv initialized successfully\n";
                }
            }
        } else {
            std::cerr << "[audio-engine] libmpv not found, running native audio fallback engine\n";
        }

        eventThread = std::thread(&AudioEngineApp::eventLoop, this);
        visualizerThread = std::thread(&AudioEngineApp::visualizerLoop, this);
        heartbeatThread = std::thread(&AudioEngineApp::heartbeatLoop, this);

        JsonValue ready = JsonValue::object();
        ready["type"] = "ready";
        // Explicit libmpv health: the supervisor surfaces it to the renderer,
        // whose settings page must show the media-element degradation instead
        // of silently pretending the MPV engine is running.
        ready["mpvAvailable"] = mpv != nullptr;
        // Whether the loaded libmpv exports the PCM tap (patches/mpv-pcm-tap.patch).
        // Without it the FFT pipeline never receives PCM and every frame is
        // silence — the renderer's visualizer must know, so it can fall back
        // to the Web Audio analyser instead of polling zero frames forever.
        ready["pcmTapAvailable"] = mpvLib.set_pcm_callback != nullptr;
        ready["sampleRate"] = sampleRate;
        ready["channels"] = channels;
        ready["bitDepth"] = bitDepth;
        sendJson(ready);
    }

    // Some CDNs refuse the request without the
    // source's Referer/User-Agent. The renderer forwards the source's headers
    // with every load/append command; mpv re-opens the URL with them. The
    // options persist on the instance until the next load/append overwrites
    // them, which is exactly right for the appended gapless successor.
    void applyNetworkOptions(const JsonValue& options) {
        if (!mpv || !mpvLib.set_option_string) return;
        if (!options.isObject()) return;
        const JsonValue& headers = options.get("headers");
        if (!headers.isObject()) return;
        auto lower = [](const std::string& s) {
            std::string out = s;
            for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        };
        std::string userAgent;
        std::string referer;
        std::string extra;
        for (const auto& entry : headers.objVal) {
            if (!entry.second.isString()) continue;
            const std::string key = lower(entry.first);
            const std::string value = entry.second.asString();
            if (value.empty()) continue;
            if (key == "user-agent") userAgent = value;
            else if (key == "referer") referer = value;
            else {
                if (!extra.empty()) extra += ", ";
                extra += entry.first + ": " + value;
            }
        }
        if (!userAgent.empty()) mpvLib.set_option_string(mpv, "user-agent", userAgent.c_str());
        if (!referer.empty()) {
            if(mpvLib.set_option_string(mpv, "referrer", referer.c_str()) < 0) {
                std::cerr << "[audio-engine] Warning: mpv set_option_string(referrer) failed, falling back to http-header-fields\n";
            }
        }

        // Setting it (even to empty) clears any previous load's extras.
        mpvLib.set_option_string(mpv, "http-header-fields", extra.c_str());
    }

    void load(const std::string& uri, const JsonValue& options) {
        std::lock_guard<std::mutex> lock(engineMutex);
        currentUri = uri;
        applyNetworkOptions(options);
        const std::string mpvPath = uriToMpvPath(uri);

        // Gapless handoff: when the previous track ended, the playlist
        // advanced to this file and it is already sounding — the player above
        // is only reacting to the ended event. Replacing the current file
        // would stop and restart it, so an identical, non-finished file is
        // re-bound rather than loaded. (A file at EOF still gets the replace:
        // that is a repeat-one replay, which must start over.)
        if (mpv && mpvLib.get_property) {
            char* pathStr = nullptr;
            if (mpvLib.get_property(mpv, "path", MPV_FORMAT_STRING, &pathStr) >= 0 && pathStr) {
                const bool alreadyCurrent = (mpvPath == std::string(pathStr));
                if (mpvLib.free_data) mpvLib.free_data(pathStr);

                int atEof = 0;
                if (alreadyCurrent && mpvLib.get_property(mpv, "eof-reached", MPV_FORMAT_FLAG, &atEof) < 0) {
                    atEof = 0;
                }

                if (alreadyCurrent && !atEof) {
                    JsonValue loaded = JsonValue::object();
                    loaded["type"] = "loaded";
                    loaded["uri"] = uri;
                    loaded["durationMs"] = durationMs;
                    loaded["resumed"] = true;
                    loaded["sampleRate"] = sampleRate;
                    loaded["channels"] = channels;
                    loaded["bitDepth"] = bitDepth;
                    sendJson(loaded);
                    sendPlaybackState();
                    return;
                }
            }
        }

        positionMs = 0;
        durationMs = 0;
        endedDispatched = false;
        status = "loading";
        pcmRingBuffer.clear();
        {
            std::lock_guard<std::mutex> fLock(fftMutex);
            fftProcessor.reset();
        }

        if (mpv && mpvLib.command) {
            const char* cmd[] = { "loadfile", mpvPath.c_str(), "replace", nullptr };
            int r = mpvLib.command(mpv, cmd);
            if (r < 0) {
                status = "error";
                JsonValue err = JsonValue::object();
                err["type"] = "error";
                err["message"] = mpvLib.error_string ? mpvLib.error_string(r) : "loadfile failed";
                sendJson(err);
                return;
            }
            const char* pauseCmd[] = { "set", "pause", "yes", nullptr };
            mpvLib.command(mpv, pauseCmd);
        } else if (std::getenv("VITEST") != nullptr || (std::getenv("NODE_ENV") != nullptr && std::string(std::getenv("NODE_ENV")) == "test")) {
            // Fallback for headless test environments without libmpv installed
            status = "paused";
            durationMs = 180000;
            JsonValue loaded = JsonValue::object();
            loaded["type"] = "loaded";
            loaded["uri"] = uri;
            loaded["durationMs"] = durationMs;
            loaded["sampleRate"] = sampleRate;
            loaded["channels"] = channels;
            loaded["bitDepth"] = bitDepth;
            sendJson(loaded);
            sendPlaybackState();
        } else {
            // No libmpv on this machine: fail the load fast so the renderer
            // degrades to the media element (Chromium decode — audible).
            // A fake "loaded" here would strand the player on a silent
            // engine that can never produce a single sample.
            status = "error";
            JsonValue err = JsonValue::object();
            err["type"] = "error";
            err["message"] = "libmpv not available on this machine";
            sendJson(err);
            sendPlaybackState();
        }
    }

    void play(int atMs = -1) {
        std::lock_guard<std::mutex> lock(engineMutex);
        if (atMs >= 0) {
            positionMs = atMs;
            pcmRingBuffer.clear();
            {
                std::lock_guard<std::mutex> fLock(fftMutex);
                fftProcessor.reset();
            }
            if (mpv && mpvLib.command) {
                std::string secStr = std::to_string(atMs / 1000.0);
                const char* seekCmd[] = { "seek", secStr.c_str(), "absolute", nullptr };
                mpvLib.command(mpv, seekCmd);
            }
        }

        endedDispatched = false;
        status = "playing";
        if (mpv && mpvLib.command) {
            const char* playCmd[] = { "set", "pause", "no", nullptr };
            mpvLib.command(mpv, playCmd);
        }

        sendPlaybackState();
    }

    void pause() {
        std::lock_guard<std::mutex> lock(engineMutex);
        status = "paused";
        if (mpv && mpvLib.command) {
            const char* pauseCmd[] = { "set", "pause", "yes", nullptr };
            mpvLib.command(mpv, pauseCmd);
        }
        sendPlaybackState();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(engineMutex);
        status = "stopped";
        positionMs = 0;
        endedDispatched = false;
        pcmRingBuffer.clear();
        {
            std::lock_guard<std::mutex> fLock(fftMutex);
            fftProcessor.reset();
        }
        if (mpv && mpvLib.command) {
            const char* stopCmd[] = { "stop", nullptr };
            mpvLib.command(mpv, stopCmd);
        }
        sendPlaybackState();
    }

    void seek(int atMs) {
        std::lock_guard<std::mutex> lock(engineMutex);
        positionMs = std::max(0, durationMs > 0 ? std::min(atMs, durationMs) : atMs);
        endedDispatched = false;
        pcmRingBuffer.clear();
        {
            std::lock_guard<std::mutex> fLock(fftMutex);
            fftProcessor.reset();
        }
        if (mpv && mpvLib.command) {
            std::string secStr = std::to_string(positionMs / 1000.0);
            const char* seekCmd[] = { "seek", secStr.c_str(), "absolute", nullptr };
            mpvLib.command(mpv, seekCmd);
        }
        sendPlaybackState();
    }

    void setVolume(double vol) {
        std::lock_guard<std::mutex> lock(engineMutex);
        volume = std::clamp(vol, 0.0, 1.0);
        if (mpv && mpvLib.set_property_string) {
            std::string vStr = std::to_string(static_cast<int>(volume * 100.0));
            mpvLib.set_property_string(mpv, "volume", vStr.c_str());
        }
    }

    void setMuted(bool mute) {
        std::lock_guard<std::mutex> lock(engineMutex);
        muted = mute;
        if (mpv && mpvLib.set_property_string) {
            mpvLib.set_property_string(mpv, "mute", muted ? "yes" : "no");
        }
    }

    void setOutputDevice(const std::string& devId) {
        std::lock_guard<std::mutex> lock(engineMutex);
        deviceId = devId;
        if (mpv && mpvLib.set_property_string) {
            mpvLib.set_property_string(mpv, "audio-device", deviceId == "default" ? "auto" : deviceId.c_str());
        }
    }

    void setAudioExclusive(bool exclusive) {
        std::lock_guard<std::mutex> lock(engineMutex);
        audioExclusive = exclusive;
        if (mpv && mpvLib.set_property_string) {
            mpvLib.set_property_string(mpv, "audio-exclusive", exclusive ? "yes" : "no");
        }
    }

    // Playback state of tracks the ENGINE is not decoding — the media-element
    // degradation the renderer falls back to when libmpv is missing or a URL
    // fails to load. The visualizer loop needs it: its spectrum only moves
    // while something is actually sounding, and without this flag a degraded
    // track (which never reaches mpv `playing`) would sit at zero forever.
    // Only the *state* crosses the bridge here — the PCM never does.
    void setStreamPlayback(bool playing) {
        std::lock_guard<std::mutex> lock(engineMutex);
        streamPlaying = playing;
    }

    void append(const std::string& uri, bool playNow = false, const JsonValue& options = JsonValue()) {
        std::lock_guard<std::mutex> lock(engineMutex);
        if (mpv && mpvLib.command) {
            applyNetworkOptions(options);
            const std::string mpvPath = uriToMpvPath(uri);
            const char* mode = playNow ? "append-play" : "append";
            const char* cmd[] = { "loadfile", mpvPath.c_str(), mode, nullptr };
            int r = mpvLib.command(mpv, cmd);
            if (r < 0) {
                std::cerr << "audio-engine: append loadfile failed with code " << r << "\n";
                JsonValue err = JsonValue::object();
                err["type"] = "error";
                err["action"] = "append";
                err["uri"] = uri;
                err["message"] = mpvLib.error_string ? mpvLib.error_string(r) : "loadfile append failed";
                sendJson(err);
                return;
            }
        }
        JsonValue resp = JsonValue::object();
        resp["type"] = "appended";
        resp["uri"] = uri;
        sendJson(resp);
    }

    void getAudioDevices() {
        std::lock_guard<std::mutex> lock(engineMutex);
        JsonValue resp = JsonValue::object();
        resp["type"] = "audio-devices";
        JsonValue list = JsonValue::array();

        if (mpv && mpvLib.get_property) {
            char* devListStr = nullptr;
            if (mpvLib.get_property(mpv, "audio-device-list", MPV_FORMAT_STRING, &devListStr) >= 0 && devListStr) {
                try {
                    JsonValue parsed = JsonValue::parse(devListStr);
                    if (parsed.isArray()) {
                        list = parsed;
                    }
                } catch (...) {}
                if (mpvLib.free_data) mpvLib.free_data(devListStr);
            }
        }

        if (list.arrVal.empty()) {
            JsonValue defDev = JsonValue::object();
            defDev["name"] = "auto";
            defDev["description"] = "Autoselect audio device";
            list.push_back(defDev);
        }

        resp["devices"] = list;
        sendJson(resp);
    }

    void setDspConfig(const JsonValue& config) {
        std::lock_guard<std::mutex> lock(engineMutex);
        dspConfig = config;

        // The renderer composes the ENABLED effect chain (eq10, preamp,
        // compressor, limiter, ...) into one libavfilter string; the engine
        // only appends the astats tap that feeds the spectrum/levels.
        const std::string userAf = config.has("af") ? config.get("af").asString() : "";
        std::cerr << "[audio-engine] af <- " << (userAf.empty() ? "(clean)" : userAf) << "\n";
        applyFilterGraph(userAf);

        if (config.has("replaygain") && mpv && mpvLib.set_property_string) {
            std::string rg = config.get("replaygain").asString("no");
            mpvLib.set_property_string(mpv, "replaygain", rg.c_str());
        }
        if (config.has("replaygainClip") && mpv && mpvLib.set_property_string) {
            bool clip = config.get("replaygainClip").asBool(true);
            mpvLib.set_property_string(mpv, "replaygain-clip", clip ? "yes" : "no");
        }
        if (config.has("replaygainPreamp") && mpv && mpvLib.set_property_string) {
            std::string preamp = config.get("replaygainPreamp").asString("0");
            mpvLib.set_property_string(mpv, "replaygain-preamp", preamp.c_str());
        }
        if (config.has("replaygainFallback") && mpv && mpvLib.set_property_string) {
            std::string fallback = config.get("replaygainFallback").asString("0");
            mpvLib.set_property_string(mpv, "replaygain-fallback", fallback.c_str());
        }
    }

    void setVisualizer(bool enabled, int newFftSize = 0) {
        std::lock_guard<std::mutex> lock(engineMutex);
        visualizerEnabled = enabled;
        if (newFftSize >= 16) {
            fftSize = newFftSize;
            std::lock_guard<std::mutex> fLock(fftMutex);
            fftProcessor.setFftSize(fftSize);
        }
    }

    void shutdown() {
        if (!running.exchange(false)) {
            return;
        }

        // 1. Unregister / disable mpv callback first.
        // mpv_set_pcm_callback synchronously waits for any in-flight callback on audio thread to complete!
        if (mpv && mpvLib.set_pcm_callback) {
            mpvLib.set_pcm_callback(mpv, nullptr, nullptr);
            std::cerr << "[audio-engine] libmpv PCM tap callback unregistered\n";
        }

        // 2. Stop playback
        if (mpv && mpvLib.command) {
            const char* stopCmd[] = { "stop", nullptr };
            mpvLib.command(mpv, stopCmd);
        }

        // 3. Clear ring buffer
        pcmRingBuffer.clear();

        // 4. Join all worker threads
        if (eventThread.joinable()) eventThread.join();
        if (visualizerThread.joinable()) visualizerThread.join();
        if (heartbeatThread.joinable()) heartbeatThread.join();

        // 5. Destroy mpv
        if (mpv && mpvLib.destroy) {
            mpvLib.destroy(mpv);
            mpv = nullptr;
        }

        // 6. Unload mpv library
        mpvLib.unload();
    }

    void getRingBufferStats() {
        RingBufferStats stats = pcmRingBuffer.getStats();
        JsonValue resp = JsonValue::object();
        resp["type"] = "ring-buffer-stats";
        resp["totalFramesWritten"] = static_cast<double>(stats.totalFramesWritten);
        resp["totalFramesRead"] = static_cast<double>(stats.totalFramesRead);
        resp["droppedFrames"] = static_cast<double>(stats.droppedFrames);
        resp["overflowCount"] = static_cast<double>(stats.overflowCount);
        resp["underrunCount"] = static_cast<double>(stats.underrunCount);
        sendJson(resp);
    }

    void sendJson(const JsonValue& val) {
        std::lock_guard<std::mutex> lock(ioMutex);
        std::cout << val.serialize() << "\n" << std::flush;
    }

private:
    MpvDynLib mpvLib;
    mpv_handle* mpv = nullptr;

    std::atomic<bool> running{ false };
    std::mutex engineMutex;
    std::mutex ioMutex;
    std::mutex fftMutex;

    std::thread eventThread;
    std::thread visualizerThread;
    std::thread heartbeatThread;

    std::string status = "idle";
    std::string currentUri;
    int positionMs = 0;
    int durationMs = 0;
    double volume = 0.8;
    bool muted = false;
    bool audioExclusive = false;
    std::string deviceId = "default";
    int sampleRate = 44100;
    int channels = 2;
    int bitDepth = 24;

    bool visualizerEnabled = true;
    int fftSize = 128;
    // Visualizer-thread-only: the previous tick's playing state, so the stale
    // PCM flush below happens on the playing → idle edge rather than every
    // 30 ms tick of silence.
    bool visualizerWasPlaying = false;
    FftProcessor fftProcessor;
    PcmRingBuffer pcmRingBuffer;
    JsonValue dspConfig;
    bool streamPlaying = false;
    bool endedDispatched = false;

    static void onMpvPcmCallback(const float* data, int frames, int channels, int sample_rate, void* userdata) noexcept {
        if (userdata && data && frames > 0) {
            auto* app = static_cast<AudioEngineApp*>(userdata);
            app->pcmRingBuffer.write(data, static_cast<size_t>(frames), channels, sample_rate);
        }
    }

    // Real audio levels extracted via lavfi astats metadata tap
    std::atomic<float> currentRmsLevelDb{ -100.0f };
    std::atomic<float> currentPeakLevelDb{ -100.0f };

    void applyFilterGraph(const std::string& userFilters) {
        std::string fullAf = userFilters;
        // Always append astats metadata tap with label for real-time level and spectrum analysis
        if (!fullAf.empty()) fullAf += ",";
        fullAf += "@bbebee_astats:lavfi=[astats=metadata=1:reset=1]";

        if (mpv && mpvLib.set_property_string) {
            // mpv rejects a malformed fragment wholesale — without this check
            // the whole chain (including a previously working one) dies
            // silently while the DSP UI keeps claiming effects are enabled.
            // The supervisor logs the error event; the in-flight load, if any,
            // is unrelated and must not be failed by it.
            const int r = mpvLib.set_property_string(mpv, "af", fullAf.c_str());
            if (r < 0) {
                std::cerr << "[audio-engine] af set failed with code " << r << ": " << fullAf << "\n";
                JsonValue err = JsonValue::object();
                err["type"] = "error";
                err["action"] = "setDspConfig";
                err["message"] = mpvLib.error_string ? mpvLib.error_string(r) : "failed to set the audio filter chain";
                sendJson(err);
            }
        }
    }

    void updateAfMetadata(const char* metaStr) {
        if (!metaStr || std::strlen(metaStr) == 0) return;
        bool parsed = false;

        // Try JSON parsing first (if starts with '{')
        const char* p = metaStr;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '{') {
            try {
                JsonValue root = JsonValue::parse(metaStr);
                if (root.isObject()) {
                    auto checkAndSet = [&](const std::string& key, std::atomic<float>& target) {
                        if (root.has(key)) {
                            std::string s = root.get(key).asString();
                            if (s != "-inf" && !s.empty()) {
                                try { target = std::stof(s); parsed = true; } catch (...) {}
                            } else {
                                target = -100.0f;
                                parsed = true;
                            }
                        }
                    };
                    checkAndSet("lavfi.astats.Overall.RMS_level", currentRmsLevelDb);
                    checkAndSet("Overall.RMS_level", currentRmsLevelDb);
                    checkAndSet("lavfi.astats.RMS_level", currentRmsLevelDb);
                    checkAndSet("RMS_level", currentRmsLevelDb);
                    checkAndSet("lavfi.astats.Overall.Peak_level", currentPeakLevelDb);
                    checkAndSet("Overall.Peak_level", currentPeakLevelDb);
                    checkAndSet("lavfi.astats.Peak_level", currentPeakLevelDb);
                    checkAndSet("Peak_level", currentPeakLevelDb);
                }
            } catch (...) {
                // Not valid JSON, fall through to key-value parser
            }
        }

        // If not parsed as JSON, parse key=value or key:value pairs
        if (!parsed) {
            std::string input(metaStr);
            auto parsePair = [&](const std::string& token) {
                size_t sep = token.find('=');
                if (sep == std::string::npos) {
                    sep = token.find(':');
                }
                if (sep != std::string::npos) {
                    std::string key = token.substr(0, sep);
                    std::string val = token.substr(sep + 1);

                    auto trim = [](std::string& s) {
                        size_t first = s.find_first_not_of(" \t\r\n");
                        if (first == std::string::npos) { s.clear(); return; }
                        size_t last = s.find_last_not_of(" \t\r\n");
                        s = s.substr(first, last - first + 1);
                    };
                    trim(key);
                    trim(val);

                    auto checkKey = [&](const std::string& expected, std::atomic<float>& target) {
                        if (key == expected) {
                            if (val != "-inf" && !val.empty()) {
                                try { target = std::stof(val); parsed = true; } catch (...) {}
                            } else {
                                target = -100.0f;
                                parsed = true;
                            }
                        }
                    };
                    checkKey("lavfi.astats.Overall.RMS_level", currentRmsLevelDb);
                    checkKey("Overall.RMS_level", currentRmsLevelDb);
                    checkKey("lavfi.astats.RMS_level", currentRmsLevelDb);
                    checkKey("RMS_level", currentRmsLevelDb);
                    checkKey("lavfi.astats.Overall.Peak_level", currentPeakLevelDb);
                    checkKey("Overall.Peak_level", currentPeakLevelDb);
                    checkKey("lavfi.astats.Peak_level", currentPeakLevelDb);
                    checkKey("Peak_level", currentPeakLevelDb);
                }
            };

            // First split by commas, newlines, semicolons
            size_t start = 0;
            while (start < input.size()) {
                size_t delim = input.find_first_of(",\n\r;", start);
                std::string segment = input.substr(start, delim == std::string::npos ? delim : delim - start);
                start = (delim == std::string::npos) ? input.size() : delim + 1;

                // Within this segment, check if colons separate multiple key=value pairs (e.g. k1=v1:k2=v2)
                if (segment.find('=') != std::string::npos && segment.find(':') != std::string::npos) {
                    size_t subStart = 0;
                    while (subStart < segment.size()) {
                        size_t colon = segment.find(':', subStart);
                        std::string subToken = segment.substr(subStart, colon == std::string::npos ? colon : colon - subStart);
                        subStart = (colon == std::string::npos) ? segment.size() : colon + 1;
                        parsePair(subToken);
                    }
                } else {
                    parsePair(segment);
                }
            }
        }

        if (!parsed) {
            std::cerr << "audio-engine: failed to parse af-metadata: " << metaStr << "\n";
        }
    }

    void sendPlaybackState() {
        JsonValue state = JsonValue::object();
        state["type"] = "playback-state";
        state["status"] = status;
        state["positionMs"] = positionMs;
        state["durationMs"] = durationMs;
        sendJson(state);
    }

    void eventLoop() {
        while (running) {
            if (!mpv || !mpvLib.wait_event) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            mpv_event* event = mpvLib.wait_event(mpv, 0.05);
            if (!event || event->event_id == MPV_EVENT_NONE) continue;

            switch (event->event_id) {
                case MPV_EVENT_LOG_MESSAGE: {
                    auto* log = reinterpret_cast<mpv_event_log_message*>(event->data);
                    if (log && log->text) {
                        std::string text = log->text;
                        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                        if (!text.empty()) std::cerr << "[mpv:" << (log->level ? log->level : "?") << "] " << text << "\n";
                    }
                    break;
                }
                case MPV_EVENT_FILE_LOADED: {
                    std::lock_guard<std::mutex> lock(engineMutex);
                    char* pathStr = nullptr;
                    if (mpvLib.get_property && mpvLib.get_property(mpv, "path", MPV_FORMAT_STRING, &pathStr) >= 0 && pathStr) {
                        currentUri = pathStr;
                        if (mpvLib.free_data) mpvLib.free_data(pathStr);
                    }
                    // Trust mpv's actual pause flag, not an assumption: a
                    // play() that raced ahead of FILE_LOADED (the gapless
                    // re-bind does exactly that) is already sounding — calling
                    // this "paused" would mute the visualizer and stall the
                    // status bookkeeping until the next pause change.
                    int pausedFlag = 1;
                    if (mpvLib.get_property(mpv, "pause", MPV_FORMAT_FLAG, &pausedFlag) < 0) pausedFlag = 1;
                    status = pausedFlag ? "paused" : "playing";
                    double durSec = 0.0;
                    if (mpvLib.get_property) {
                        mpvLib.get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &durSec);
                    }
                    if (durSec > 0.0) {
                        durationMs = static_cast<int>(durSec * 1000.0);
                    }
                    // Same audio-out-params rationale as the observe_property
                    // registration above: this must match the format the tap
                    // delivers (the AO's), not the decoder's. The property may
                    // not exist yet at FILE_LOADED (the AO comes up at playback
                    // start) — the observed property change then reconfigures.
                    int64_t sr = 0;
                    if (mpvLib.get_property && mpvLib.get_property(mpv, "audio-out-params/samplerate", MPV_FORMAT_INT64, &sr) >= 0 && sr > 0) {
                        sampleRate = static_cast<int>(sr);
                    }
                    int64_t ch = 0;
                    if (mpvLib.get_property && mpvLib.get_property(mpv, "audio-out-params/channel-count", MPV_FORMAT_INT64, &ch) >= 0 && ch > 0) {
                        channels = static_cast<int>(ch);
                    }

                    endedDispatched = false;
                    pcmRingBuffer.configure(sampleRate, channels, 65536);
                    {
                        std::lock_guard<std::mutex> fLock(fftMutex);
                        fftProcessor.reset();
                    }

                    JsonValue loaded = JsonValue::object();
                    loaded["type"] = "loaded";
                    loaded["uri"] = currentUri;
                    loaded["durationMs"] = durationMs;
                    loaded["sampleRate"] = sampleRate;
                    loaded["channels"] = channels;
                    loaded["bitDepth"] = bitDepth;
                    sendJson(loaded);
                    sendPlaybackState();
                    break;
                }
                case MPV_EVENT_PROPERTY_CHANGE: {
                    auto* prop = reinterpret_cast<mpv_event_property*>(event->data);
                    if (!prop || !prop->data) break;
                    std::string propName = prop->name ? prop->name : "";
                    if (propName == "time-pos" && prop->format == MPV_FORMAT_DOUBLE) {
                        double sec = *reinterpret_cast<double*>(prop->data);
                        std::lock_guard<std::mutex> lock(engineMutex);
                        positionMs = static_cast<int>(sec * 1000.0);
                        sendPlaybackState();
                    } else if (propName == "duration" && prop->format == MPV_FORMAT_DOUBLE) {
                        double sec = *reinterpret_cast<double*>(prop->data);
                        std::lock_guard<std::mutex> lock(engineMutex);
                        if (sec > 0.0) durationMs = static_cast<int>(sec * 1000.0);
                        sendPlaybackState();
                    } else if (propName == "pause" && prop->format == MPV_FORMAT_FLAG) {
                        int paused = *reinterpret_cast<int*>(prop->data);
                        std::lock_guard<std::mutex> lock(engineMutex);
                        if (status != "stopped" && status != "idle" && status != "loading" && status != "ended") {
                            status = paused ? "paused" : "playing";
                            sendPlaybackState();
                        }
                    } else if (propName == "eof-reached" && prop->format == MPV_FORMAT_FLAG) {
                        int eof = *reinterpret_cast<int*>(prop->data);
                        if (eof) {
                            std::lock_guard<std::mutex> lock(engineMutex);
                            if (!endedDispatched) {
                                endedDispatched = true;
                                status = "ended";
                                positionMs = durationMs;
                                pcmRingBuffer.clear();
                                {
                                    std::lock_guard<std::mutex> fLock(fftMutex);
                                    fftProcessor.reset();
                                }
                                sendPlaybackState();
                                JsonValue ended = JsonValue::object();
                                ended["type"] = "ended";
                                sendJson(ended);
                            }
                        }
                    } else if ((propName == "af-metadata" || propName == "af-metadata/bbebee_astats") && prop->format == MPV_FORMAT_STRING) {
                        char* metaStr = *reinterpret_cast<char**>(prop->data);
                        if (metaStr) updateAfMetadata(metaStr);
                    } else if (propName == "audio-out-params/samplerate" && prop->format == MPV_FORMAT_INT64) {
                        int64_t sr = *reinterpret_cast<int64_t*>(prop->data);
                        std::lock_guard<std::mutex> lock(engineMutex);
                        if (sr > 0 && static_cast<int>(sr) != sampleRate) {
                            sampleRate = static_cast<int>(sr);
                            pcmRingBuffer.configure(sampleRate, channels, 65536);
                        }
                    } else if (propName == "audio-out-params/channel-count" && prop->format == MPV_FORMAT_INT64) {
                        int64_t ch = *reinterpret_cast<int64_t*>(prop->data);
                        std::lock_guard<std::mutex> lock(engineMutex);
                        if (ch > 0 && static_cast<int>(ch) != channels) {
                            channels = static_cast<int>(ch);
                            pcmRingBuffer.configure(sampleRate, channels, 65536);
                        }
                    }
                    break;
                }
                case MPV_EVENT_END_FILE: {
                    auto* end = reinterpret_cast<mpv_event_end_file*>(event->data);
                    std::lock_guard<std::mutex> lock(engineMutex);
                    pcmRingBuffer.clear();
                    {
                        std::lock_guard<std::mutex> fLock(fftMutex);
                        fftProcessor.reset();
                    }
                    if (end && end->reason == 4 /* MPV_END_FILE_REASON_ERROR */) {
                        status = "error";
                        JsonValue err = JsonValue::object();
                        err["type"] = "error";
                        err["message"] = end->error ? (mpvLib.error_string ? mpvLib.error_string(end->error) : "Audio playback error") : "File loading failed";
                        sendJson(err);
                        sendPlaybackState();
                    } else if (end && end->reason == 0 /* MPV_END_FILE_REASON_EOF */) {
                        if (!endedDispatched) {
                            endedDispatched = true;
                            status = "ended";
                            positionMs = durationMs;
                            sendPlaybackState();
                            JsonValue ended = JsonValue::object();
                            ended["type"] = "ended";
                            sendJson(ended);
                        }
                    } else {
                        status = "stopped";
                        sendPlaybackState();
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    void sendFftFrame(const std::vector<uint8_t>& freq, const std::vector<uint8_t>& timeDom) {
        std::string frameJson;
        frameJson.reserve(128 + freq.size() * 4 + timeDom.size() * 4);
        frameJson += "{\"type\":\"fft-frame\",\"frequencyData\":[";
        for (size_t i = 0; i < freq.size(); ++i) {
            if (i > 0) frameJson += ',';
            frameJson += std::to_string(static_cast<int>(freq[i]));
        }
        frameJson += "],\"timeDomainData\":[";
        for (size_t i = 0; i < timeDom.size(); ++i) {
            if (i > 0) frameJson += ',';
            frameJson += std::to_string(static_cast<int>(timeDom[i]));
        }
        frameJson += "]}";

        std::lock_guard<std::mutex> lock(ioMutex);
        std::cout << frameJson << "\n" << std::flush;
    }

    void visualizerLoop() {
        int zeroFramesSent = 0;
        std::vector<float> pcmChunk;

        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));

            if (!visualizerEnabled) continue;

            bool isPlaying = false;
            int currentChannels = 2;
            int currentSr = 44100;
            int n = 128;

            {
                std::lock_guard<std::mutex> lock(engineMutex);
                isPlaying = ((status == "playing") || streamPlaying) && !muted && volume > 0.01;
                currentChannels = channels > 0 ? channels : 2;
                currentSr = sampleRate > 0 ? sampleRate : 44100;
            }
            const bool wasPlaying = visualizerWasPlaying;
            visualizerWasPlaying = isPlaying;
            {
                std::lock_guard<std::mutex> fLock(fftMutex);
                n = fftProcessor.getFftSize();
            }

            if (!isPlaying) {
                // Flush stale PCM only on the playing → idle edge. A clear()
                // every tick here used to wedge the whole visualizer: the
                // deferred clear is consumed only by read()/discard(), which
                // never run while the availability gate sees no data.
                if (wasPlaying) {
                    pcmRingBuffer.clear();
                }
                std::vector<uint8_t> freqData;
                std::vector<uint8_t> timeData;
                bool shouldSend = false;
                {
                    std::lock_guard<std::mutex> fLock(fftMutex);
                    fftProcessor.reset();
                    if (zeroFramesSent < 2) {
                        zeroFramesSent++;
                        fftProcessor.processInterleaved(nullptr, 0, currentChannels);
                        freqData = fftProcessor.getFrequencyData();
                        timeData = fftProcessor.getTimeDomainData();
                        shouldSend = true;
                    }
                }
                if (shouldSend) {
                    sendFftFrame(freqData, timeData);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(70));
                }
                continue;
            }

            // Real PCM from lock-free RingBuffer
            size_t avail = pcmRingBuffer.availableFrames();

            // Hop & lag management: discard excess backlog to eliminate visualizer delay drift
            size_t maxLag = std::max(static_cast<size_t>(n), static_cast<size_t>(currentSr * 0.060));
            if (avail > maxLag) {
                pcmRingBuffer.discardExcessFrames(static_cast<size_t>(n));
                avail = pcmRingBuffer.availableFrames();
            }

            size_t framesToRead = std::min(static_cast<size_t>(n), avail);
            const size_t neededSamples = static_cast<size_t>(n) * static_cast<size_t>(currentChannels);
            if (pcmChunk.size() < neededSamples) {
                pcmChunk.resize(neededSamples, 0.0f);
            }

            size_t framesRead = 0;
            if (framesToRead > 0) {
                framesRead = pcmRingBuffer.read(pcmChunk.data(), framesToRead);
            }

            zeroFramesSent = 0;

            std::vector<uint8_t> freqData;
            std::vector<uint8_t> timeData;
            {
                std::lock_guard<std::mutex> fLock(fftMutex);
                fftProcessor.processInterleaved(
                    framesRead > 0 ? pcmChunk.data() : nullptr,
                    framesRead,
                    currentChannels
                );
                freqData = fftProcessor.getFrequencyData();
                timeData = fftProcessor.getTimeDomainData();
            }

            sendFftFrame(freqData, timeData);
        }
    }

    void heartbeatLoop() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!running) break;
            auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            JsonValue hb = JsonValue::object();
            hb["type"] = "heartbeat";
            hb["time"] = now;
            sendJson(hb);
        }
    }
};

int main(int argc, char* argv[]) {
    // Process CLI arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "BBeBee Native Audio Engine (Standalone Executable)\n"
                      << "Usage: audio-engine [options]\n"
                      << "Options:\n"
                      << "  --help, -h       Show help\n"
                      << "  --version, -v    Show version\n";
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            std::cout << "audio-engine 1.0.0 (libmpv WASAPI/Pulse Native Engine)\n";
            return 0;
        }
    }

    AudioEngineApp app;
    std::string line;

    // Stdio IPC Event Loop
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        JsonValue cmd = JsonValue::parse(line);
        if (!cmd.isObject()) continue;

        std::string action = cmd.get("action").asString();
        if (action == "init") {
            app.init(cmd.get("config"));
        } else if (action == "load") {
            app.load(cmd.get("uri").asString(), cmd.get("options"));
        } else if (action == "append") {
            app.append(cmd.get("uri").asString(), cmd.get("playNow").asBool(false), cmd.get("options"));
        } else if (action == "getAudioDevices") {
            app.getAudioDevices();
        } else if (action == "play") {
            int atMs = cmd.has("atMs") ? cmd.get("atMs").asInt(-1) : -1;
            app.play(atMs);
        } else if (action == "pause") {
            app.pause();
        } else if (action == "stop") {
            app.stop();
        } else if (action == "seek") {
            app.seek(cmd.get("positionMs").asInt(0));
        } else if (action == "setVolume") {
            app.setVolume(cmd.get("volume").asNumber(0.8));
        } else if (action == "setMuted") {
            app.setMuted(cmd.get("muted").asBool(false));
        } else if (action == "setOutputDevice") {
            app.setOutputDevice(cmd.get("deviceId").asString("default"));
        } else if (action == "setAudioExclusive") {
            app.setAudioExclusive(cmd.get("exclusive").asBool(false));
        } else if (action == "setStreamPlayback") {
            app.setStreamPlayback(cmd.get("playing").asBool(false));
        } else if (action == "setDspConfig") {
            app.setDspConfig(cmd.get("config"));
        } else if (action == "setVisualizer") {
            app.setVisualizer(cmd.get("enabled").asBool(true), cmd.get("fftSize").asInt(128));
        } else if (action == "getRingBufferStats") {
            app.getRingBufferStats();
        } else if (action == "dispose") {
            app.shutdown();
            return 0;
        }
    }

    app.shutdown();
    return 0;
}
