#pragma once
#ifdef _WIN32
#include "WindowsMediaDecoder.hpp"
#endif
#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define STB_VORBIS_HEADER_ONLY
#include "../vendor/miniaudio/miniaudio.h"
#include "../vendor/miniaudio/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
#include <array>
#ifdef OBS_JUKEBOX_QA
#include <atomic>
#include <chrono>
#include <thread>
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#ifdef _WIN32
#include <memory>
#endif
class Decoder {
    ma_decoder decoder{};
    bool opened = false, seekValid = false;
    ma_uint64 lengthFrames = 0;
#ifdef _WIN32
    std::unique_ptr<WindowsMediaDecoder> windowsDecoder;
#endif
    std::array<float, 16384> cache{};
    size_t count = 0;
    double phase = 0;
    bool unexpectedStarvation = false;
#ifdef OBS_JUKEBOX_QA
    bool testStarved = false;
    bool testFailedSeek = false;
#endif

  public:
#ifdef OBS_JUKEBOX_QA
    inline static std::atomic<int> testOpenDelayMs{0};
    inline static std::atomic<int> testOpenHandles{0};
    inline static std::atomic<bool> testStarveOnNextSeek{false};
    inline static std::atomic<bool> testFailOnNextSeek{false};
    inline static std::atomic<bool> testStarveAllReads{false};
    inline static std::atomic<unsigned> testOpenCount{0};
#endif
    double position = 0;
    std::string error;
    ~Decoder() { close(); }
    void close() {
#ifdef OBS_JUKEBOX_QA
        if (opened)
            --testOpenHandles;
#endif
#ifdef _WIN32
        if (windowsDecoder)
            windowsDecoder.reset();
        else
#endif
            if (opened)
            ma_decoder_uninit(&decoder);
        lengthFrames = 0;
        opened = false;
        seekValid = false;
        count = 0;
        phase = 0;
        position = 0;
        unexpectedStarvation = false;
#ifdef OBS_JUKEBOX_QA
        testStarved = false;
        testFailedSeek = false;
#endif
    }
    bool open(const std::string &path) {
        close();
        error.clear();
#ifdef OBS_JUKEBOX_QA
        ++testOpenCount;
        std::this_thread::sleep_for(std::chrono::milliseconds(testOpenDelayMs.load()));
#endif
        if (path.empty()) {
            error = "Choose an OBS song in Jukebox.";
            return false;
        }
        auto config = ma_decoder_config_init(ma_format_f32, 2, 48000);
        config.seekPointCount = 2048;
#ifdef _WIN32
        auto wide = std::filesystem::path(std::u8string(path.begin(), path.end())).wstring();
        auto result = ma_decoder_init_file_w(wide.c_str(), &config, &decoder);
#else
        auto result = ma_decoder_init_file(path.c_str(), &config, &decoder);
#endif
        opened = result == MA_SUCCESS;
#ifdef _WIN32
        if (!opened) {
            auto fallback = std::make_unique<WindowsMediaDecoder>();
            if (fallback->open(wide)) {
                windowsDecoder = std::move(fallback);
                opened = true;
            }
        }
#endif
        if (opened) {
#ifdef _WIN32
            if (windowsDecoder)
                lengthFrames = windowsDecoder->lengthInFrames();
            else
#endif
                ma_decoder_get_length_in_pcm_frames(&decoder, &lengthFrames);
        }
        seekValid = opened;
#ifdef OBS_JUKEBOX_QA
        if (opened)
            ++testOpenHandles;
#endif
        if (!opened) {
#ifdef _WIN32
            error = "Cannot decode this song. Use MP3, WAV, FLAC, Ogg Vorbis, AAC, M4A or uncompressed AIFF.";
#else
            error = "Cannot decode this song. Use MP3, WAV, FLAC, Ogg Vorbis or uncompressed AIFF.";
#endif
        }
        return opened;
    }
    bool ready() const { return opened; }
    bool starvedBeforeEnd() const { return unexpectedStarvation; }
    bool hasAudioAt(double seconds) const {
        return opened && lengthFrames && std::isfinite(seconds) && seconds >= 0 &&
               seconds * 48000 + 4800 < double(lengthFrames);
    }
    bool seek(double seconds) {
        if (!opened)
            return false;
#ifdef OBS_JUKEBOX_QA
        testFailedSeek |= testFailOnNextSeek.exchange(false);
        if (testFailedSeek) {
            count = 0;
            seekValid = false;
            return false;
        }
        if (testStarveOnNextSeek.exchange(false)) {
            testStarved = true;
            count = 0;
            phase = 0;
            seekValid = false;
        }
#endif
        // position describes phase within the retained PCM window, not the backend's read-ahead cursor.
        double cachedStart = position - phase / 48000.;
        double cachedPhase = (seconds - cachedStart) * 48000.;
        if (seekValid && std::isfinite(cachedPhase) && cachedPhase >= 0 && cachedPhase < double(count)) {
            phase = cachedPhase;
            position = seconds;
            error.clear();
            return true;
        }
        count = 0;
        phase = 0;
        seekValid = false;
        // Reject known EOF positions before touching the backend. An indexed MP3
        // seek beyond EOF can fail with its cursor at zero but its stream exhausted;
        // miniaudio then optimizes away a subsequent seek(0), leaving silence.
        bool valid = std::isfinite(seconds) && seconds >= 0 && seconds <= 31536000.0 &&
                     (!lengthFrames || seconds * 48000 < double(lengthFrames));
        if (valid) {
#ifdef _WIN32
            if (windowsDecoder)
                valid = windowsDecoder->seek(seconds);
            else
#endif
                valid = ma_decoder_seek_to_pcm_frame(&decoder, ma_uint64(seconds * 48000)) == MA_SUCCESS;
        }
        if (!valid) {
            error = "Cannot seek this song. Music is silent until a valid position is available.";
            return false;
        }
        position = seconds;
        seekValid = true;
        error.clear();
        return true;
    }
    void render(float *output, size_t frames, double rate) {
        unexpectedStarvation = false;
        if (!opened || !seekValid) {
            std::fill_n(output, frames * 2, 0.f);
            return;
        }
        size_t consumed = std::min(count, size_t(phase));
        if (consumed) {
            std::memmove(cache.data(), cache.data() + consumed * 2, (count - consumed) * 2 * sizeof(float));
            count -= consumed;
            phase -= consumed;
        }
        size_t needed = size_t(std::ceil(phase + frames * rate)) + 2;
        if (count < needed) {
            ma_uint64 got = 0;
#ifdef OBS_JUKEBOX_QA
            if (!testStarved && !testStarveAllReads.load()) {
#endif
#ifdef _WIN32
            if (windowsDecoder)
                got = windowsDecoder->read(cache.data() + count * 2, cache.size() / 2 - count);
            else
#endif
                ma_decoder_read_pcm_frames(&decoder, cache.data() + count * 2, cache.size() / 2 - count,
                                           &got);
#ifdef OBS_JUKEBOX_QA
            }
#endif
            count += got;
        }
        for (size_t i = 0; i < frames; ++i) {
            auto at = size_t(phase);
            // Digital silence is valid PCM. Only a missing frame before the
            // known end of the file is evidence of a stuck decoder.
            if (at >= count && hasAudioAt(position + i * rate / 48000.))
                unexpectedStarvation = true;
            double blend = phase - at;
            for (size_t ch = 0; ch < 2; ++ch) {
                float a = at < count ? cache[at * 2 + ch] : 0,
                      b = at + 1 < count ? cache[(at + 1) * 2 + ch] : a;
                output[i * 2 + ch] = float(a + (b - a) * blend);
            }
            phase += rate;
        }
        position += frames * rate / 48000.;
    }
};
