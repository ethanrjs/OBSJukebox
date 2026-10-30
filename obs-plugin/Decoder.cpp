// Keep third-party implementations in exactly one translation unit.
#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define STB_VORBIS_HEADER_ONLY
#include "../vendor/miniaudio/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "../vendor/miniaudio/miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "../vendor/miniaudio/stb_vorbis.c"
#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

// Declared in Decoder.hpp. ma_dr_mp3_calculate_seek_points reads every frame header twice, once to
// count the frames and once to place the points, and without a Xing frame count the length takes a
// third pass. This takes one pass and repeats the placement on what it recorded, so the table and
// length are the ones miniaudio would give. Out of memory for its frame list, the file falls back to
// miniaudio's own passes, as any other failure does. It reads the MP3 backend's internals and repeats
// miniaudio 0.11.23's placement, so a miniaudio update must re-check it against decoderSeekPointsTest.
static_assert(MA_VERSION_MAJOR == 0 && MA_VERSION_MINOR == 11 && MA_VERSION_REVISION == 23,
              "indexMp3 mirrors miniaudio 0.11.23's ma_dr_mp3_calculate_seek_points; re-check it");
bool indexMp3(ma_decoder &decoder, ma_uint32 wanted, ma_uint64 &lengthFrames) try {
    lengthFrames = 0;
    if (decoder.pBackendVTable != &g_ma_decoding_backend_vtable_mp3)
        return true;
    auto &backend = *static_cast<ma_mp3 *>(decoder.pBackend);
    auto &mp3 = backend.dr;
    if (mp3.currentPCMFrame != 0 || !ma_dr_mp3_seek_to_start_of_stream(&mp3))
        return false;
    struct Frame {
        ma_uint64 bytePos;
        ma_uint32 pcmFrames, sampleRate;
    };
    std::vector<Frame> frames;
    ma_uint64 totalPcm = 0;
    for (;;) {
        const ma_uint64 bytePos = mp3.streamCursor - mp3.dataSize;
        const ma_uint32 pcmFrames = ma_dr_mp3_decode_next_frame_ex(&mp3, NULL, NULL, NULL);
        if (!pcmFrames)
            break;
        frames.push_back({bytePos, pcmFrames, mp3.mp3FrameSampleRate});
        totalPcm += pcmFrames;
    }
    if (!ma_dr_mp3_seek_to_start_of_stream(&mp3))
        return false;
    std::vector<ma_dr_mp3_seek_point> points;
    constexpr size_t leading = MA_DR_MP3_SEEK_LEADING_MP3_FRAMES;
    if (frames.size() < leading + 1) {
        points.push_back({0, 0, 0, 0});
    } else {
        const ma_uint32 count = ma_uint32(std::min<ma_uint64>(wanted, frames.size() - 1));
        const ma_uint64 between = totalPcm / (count + 1);
        // As ma_dr_mp3__accumulate_running_pcm_frame_count, in the same float arithmetic.
        ma_uint64 running = 0;
        float fraction = 0;
        size_t next = 0;
        auto accumulate = [&] {
            const float ratio = (float)frames[next].sampleRate / (float)mp3.sampleRate;
            const float out = fraction + (frames[next].pcmFrames / ratio);
            const ma_uint32 whole = (ma_uint32)out;
            fraction = out - whole;
            running += whole;
            ++next;
        };
        struct Info {
            ma_uint64 bytePos, pcmFrameIndex;
        } window[leading + 1];
        for (auto &info : window) {
            info = {frames[next].bytePos, running};
            accumulate();
        }
        ma_uint64 target = 0;
        for (ma_uint32 point = 0; point < count; ++point) {
            target += between;
            while (target >= running) {
                // miniaudio places a point at the end of the stream here; that case is left to it.
                if (next == frames.size())
                    return false;
                std::copy(window + 1, window + leading + 1, window);
                window[leading] = {frames[next].bytePos, running};
                accumulate();
            }
            points.push_back({window[0].bytePos, target, leading,
                              (ma_uint16)(target - window[leading - 1].pcmFrameIndex)});
        }
    }
    // The MP3 backend owns the table and frees it when the decoder closes, as with its own.
    const size_t bytes = points.size() * sizeof(points[0]);
    auto *table = static_cast<ma_dr_mp3_seek_point *>(ma_malloc(bytes, &decoder.allocationCallbacks));
    if (!table)
        return false;
    std::memcpy(table, points.data(), bytes);
    if (!ma_dr_mp3_bind_seek_table(&mp3, ma_uint32(points.size()), table)) {
        ma_free(table, &decoder.allocationCallbacks);
        return false;
    }
    backend.seekPointCount = ma_uint32(points.size());
    backend.pSeekPoints = table;
    if (mp3.totalPCMFrameCount == MA_UINT64_MAX) // as ma_decoder_get_length_in_pcm_frames
        lengthFrames = mp3.sampleRate == decoder.outputSampleRate
                           ? totalPcm
                           : ma_calculate_frame_count_after_resampling(decoder.outputSampleRate, mp3.sampleRate, totalPcm);
    return true;
} catch (const std::bad_alloc &) {
    return false;
}
