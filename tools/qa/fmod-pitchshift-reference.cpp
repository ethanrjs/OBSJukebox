// Measures FMOD's own pitch shifter (FMOD_DSP_TYPE_PITCHSHIFT) in Geometry Dash's fmod.dll, so the
// plugin's port of the same algorithm can be checked against it. Manual QA, Windows only; it ships
// nothing and runs the FMOD library from the local GD install.
//
//   fmod-pitchshift-reference <folder holding GD's fmod.dll> <output folder>
//
// FMOD mixes in non-realtime mode: each System::update renders one DSP block, and a custom DSP at
// the head of the master group records the result. Prints a report and writes each case's input
// and output as 32-bit float WAV files.
#include <windows.h>
#include <fmod.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace {
void check(FMOD_RESULT result, const char *what) {
    if (result == FMOD_OK)
        return;
    std::fprintf(stderr, "%s failed: FMOD error %d\n", what, int(result));
    std::exit(1);
}

using Stereo = std::vector<float>; // interleaved left, right
size_t framesOf(const Stereo &s) { return s.size() / 2; }

struct Stage {
    float pitch;
    int fft;
};
struct Event {
    size_t frame;
    enum Kind { Pause, Resume, Stop } kind;
};
struct Case {
    std::string name;
    Stereo input;
    std::vector<Stage> channelStages, groupStages;
    std::vector<Event> events;
    size_t frames; // how much output to record
};

FMOD_RESULT F_CALL record(FMOD_DSP_STATE *state, float *in, float *out, unsigned frames, int channels,
                          int *outChannels) {
    *outChannels = channels;
    void *user = nullptr;
    state->functions->getuserdata(state, &user);
    auto &recorded = *static_cast<Stereo *>(user);
    for (unsigned i = 0; i < frames; ++i) {
        float left = in ? in[i * channels] : 0.f;
        recorded.push_back(left);
        recorded.push_back(in && channels > 1 ? in[i * channels + 1] : left);
    }
    if (in && out && in != out)
        std::memcpy(out, in, frames * channels * sizeof(float));
    return FMOD_OK;
}

// Stages are given input side first; each is added at the head, so the last one ends nearest the output.
void addStages(FMOD::System *system, FMOD::ChannelControl *target, const std::vector<Stage> &stages) {
    for (const auto &stage : stages) {
        FMOD::DSP *dsp = nullptr;
        check(system->createDSPByType(FMOD_DSP_TYPE_PITCHSHIFT, &dsp), "createDSPByType");
        check(dsp->setParameterFloat(FMOD_DSP_PITCHSHIFT_FFTSIZE, float(stage.fft)), "set FFT size");
        check(dsp->setParameterFloat(FMOD_DSP_PITCHSHIFT_PITCH, stage.pitch), "set pitch");
        check(target->addDSP(FMOD_CHANNELCONTROL_DSP_HEAD, dsp), "addDSP");
    }
}

Stereo run(int rate, const Case &c) {
    FMOD::System *system = nullptr;
    check(FMOD::System_Create(&system), "System_Create");
    check(system->setOutput(FMOD_OUTPUTTYPE_NOSOUND_NRT), "setOutput");
    check(system->setSoftwareFormat(rate, FMOD_SPEAKERMODE_STEREO, 0), "setSoftwareFormat");
    check(system->setDSPBufferSize(256, 4), "setDSPBufferSize");
    check(system->init(32, FMOD_INIT_NORMAL, nullptr), "init");

    FMOD_CREATESOUNDEXINFO info{};
    info.cbsize = sizeof(info);
    info.numchannels = 2;
    info.defaultfrequency = rate;
    info.format = FMOD_SOUND_FORMAT_PCMFLOAT;
    info.length = unsigned(c.input.size() * sizeof(float));
    FMOD::Sound *sound = nullptr;
    check(system->createSound(nullptr, FMOD_OPENUSER | FMOD_LOOP_OFF | FMOD_CREATESAMPLE, &info, &sound),
          "createSound");
    void *first = nullptr, *second = nullptr;
    unsigned firstLength = 0, secondLength = 0;
    check(sound->lock(0, info.length, &first, &second, &firstLength, &secondLength), "Sound::lock");
    std::memcpy(first, c.input.data(), firstLength);
    if (second)
        std::memcpy(second, reinterpret_cast<const char *>(c.input.data()) + firstLength, secondLength);
    check(sound->unlock(first, second, firstLength, secondLength), "Sound::unlock");

    FMOD::ChannelGroup *master = nullptr, *music = nullptr;
    check(system->getMasterChannelGroup(&master), "getMasterChannelGroup");
    check(system->createChannelGroup("music", &music), "createChannelGroup");
    addStages(system, music, c.groupStages);

    Stereo recorded;
    FMOD_DSP_DESCRIPTION description{};
    std::strcpy(description.name, "record");
    description.version = 0x10000;
    description.numinputbuffers = 1;
    description.numoutputbuffers = 1;
    description.read = record;
    description.userdata = &recorded;
    FMOD::DSP *tap = nullptr;
    check(system->createDSP(&description, &tap), "createDSP");
    check(master->addDSP(FMOD_CHANNELCONTROL_DSP_HEAD, tap), "addDSP record");

    FMOD::Channel *channel = nullptr;
    check(system->playSound(sound, music, true, &channel), "playSound");
    addStages(system, channel, c.channelStages);
    check(channel->setPaused(false), "start");
    size_t next = 0;
    while (framesOf(recorded) < c.frames) {
        for (; next < c.events.size() && framesOf(recorded) >= c.events[next].frame; ++next) {
            auto kind = c.events[next].kind;
            if (kind == Event::Stop)
                channel->stop();
            else
                channel->setPaused(kind == Event::Pause);
        }
        check(system->update(), "update");
    }
    recorded.resize(c.frames * 2);
    master->removeDSP(tap);
    tap->release();
    sound->release();
    music->release();
    system->release();
    return recorded;
}

Stereo silence(size_t frames) { return Stereo(frames * 2, 0.f); }
Stereo impulses(size_t frames, size_t firstAt, size_t every) {
    auto s = silence(frames);
    for (size_t f = firstAt; f < frames; f += every)
        s[f * 2] = s[f * 2 + 1] = .5f;
    return s;
}
Stereo sine(size_t frames, int rate, double hz, float level = .3f) {
    Stereo s(frames * 2);
    for (size_t f = 0; f < frames; ++f)
        s[f * 2] = s[f * 2 + 1] = level * float(std::sin(2 * std::numbers::pi * hz * double(f) / rate));
    return s;
}
Stereo noise(size_t frames) {
    std::mt19937 random(19);
    std::uniform_real_distribution<float> value(-.2f, .2f);
    Stereo s(frames * 2);
    for (size_t f = 0; f < frames; ++f)
        s[f * 2] = s[f * 2 + 1] = value(random);
    return s;
}
Stereo padded(Stereo s, size_t frames) {
    s.resize(std::max(s.size(), frames * 2), 0.f);
    return s;
}

size_t peak(const Stereo &s, size_t from, size_t to) {
    size_t best = from;
    for (size_t f = from; f < std::min(to, framesOf(s)); ++f)
        if (std::abs(s[f * 2]) > std::abs(s[best * 2]))
            best = f;
    return best;
}
double rms(const Stereo &s, size_t from, size_t to) {
    double sum = 0;
    size_t count = 0;
    for (size_t f = from; f < std::min(to, framesOf(s)); ++f, ++count)
        sum += double(s[f * 2]) * s[f * 2];
    return count ? std::sqrt(sum / count) : 0;
}
double db(double ratio) { return ratio > 0 ? 20 * std::log10(ratio) : -INFINITY; }
// Last frame at or after `from` whose level exceeds -80 dBFS, or `from` if none does.
size_t lastAudible(const Stereo &s, size_t from) {
    size_t last = from;
    for (size_t f = from; f < framesOf(s); ++f)
        if (std::abs(s[f * 2]) > 1e-4f)
            last = f;
    return last;
}

// Left channel from `from` for `frames`, 16-bit PCM unless `asFloat`. Outputs are small enough to
// keep next to the tests. A noise input is kept exact: the vocoder's phase estimates are chaotic, so
// even 16-bit rounding moves its output away from FMOD's over a long window.
void writeMono(const std::filesystem::path &path, const Stereo &s, size_t from, size_t frames, int rate,
               bool asFloat = false) {
    from = std::min(from, framesOf(s));
    frames = std::min(frames, framesOf(s) - from);
    std::ofstream out(path, std::ios::binary);
    auto word = [&](uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i)
            out.put(char(value >> (i * 8)));
    };
    const uint32_t bytes = uint32_t(frames * (asFloat ? 4 : 2));
    out.write("RIFF", 4);
    word(36 + bytes, 4);
    out.write("WAVEfmt ", 8);
    word(16, 4);
    word(asFloat ? 3 : 1, 2); // IEEE float or PCM
    word(1, 2);
    word(uint32_t(rate), 4);
    word(uint32_t(rate) * (asFloat ? 4u : 2u), 4);
    word(asFloat ? 4 : 2, 2);
    word(asFloat ? 32 : 16, 2);
    out.write("data", 4);
    word(bytes, 4);
    for (size_t i = 0; i < frames; ++i) {
        const float sample = std::clamp(s[(from + i) * 2], -1.f, 1.f);
        if (asFloat) {
            word(uint32_t(std::bit_cast<uint32_t>(sample)), 4);
        } else {
            word(uint16_t(int16_t(std::lround(sample * 32767.f))), 2);
        }
    }
}
void writeWav(const std::filesystem::path &path, const Stereo &s, int rate) {
    std::ofstream out(path, std::ios::binary);
    auto word = [&](uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i)
            out.put(char(value >> (i * 8)));
    };
    uint32_t bytes = uint32_t(s.size() * sizeof(float));
    out.write("RIFF", 4);
    word(36 + bytes, 4);
    out.write("WAVEfmt ", 8);
    word(16, 4);
    word(3, 2); // IEEE float
    word(2, 2);
    word(uint32_t(rate), 4);
    word(uint32_t(rate) * 8, 4);
    word(8, 2);
    word(32, 2);
    out.write("data", 4);
    word(bytes, 4);
    out.write(reinterpret_cast<const char *>(s.data()), bytes);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: fmod-pitchshift-reference <folder holding GD's fmod.dll> <output folder>\n");
        return 2;
    }
    // fmod.dll is delay-loaded, so it comes from GD's folder, not from beside this tool.
    SetDllDirectoryW(std::filesystem::path(argv[1]).wstring().c_str());
    std::filesystem::path outDir = argv[2];
    std::filesystem::create_directories(outDir);
    FILE *report = std::fopen((outDir / "report.txt").string().c_str(), "w");
    auto line = [&](const char *format, auto... values) {
        std::printf(format, values...);
        std::fprintf(report, format, values...);
    };
    {
        FMOD::System *probe = nullptr;
        check(FMOD::System_Create(&probe), "System_Create");
        unsigned version = 0;
        probe->getVersion(&version);
        probe->release();
        line("FMOD %x.%02x.%02x from %s\n", version >> 16, (version >> 8) & 0xff, version & 0xff, argv[1]);
    }
    const int ffts[] = {256, 512, 1024, 2048, 4096};
    for (int rate : {48000, 44100}) {
        line("\n== mixer %d Hz ==\n", rate);
        const size_t firstImpulse = 8192, every = 16384, impulseFrames = firstImpulse + 3 * every;
        auto pulse = impulses(impulseFrames, firstImpulse, every);
        auto dry = run(rate, {"dry", pulse, {}, {}, {}, impulseFrames});
        size_t dryPeak = peak(dry, firstImpulse - 2048, firstImpulse + 4096);
        line("dry impulse at %zu (input %zu): pipeline delay %lld frames\n", dryPeak, firstImpulse,
             (long long)dryPeak - (long long)firstImpulse);
        auto save = [&](const std::string &name, const Stereo &s) {
            writeWav(outDir / (std::to_string(rate) + "-" + name + ".wav"), s, rate);
        };
        save("impulse-in", pulse);
        for (int fft : ffts)
            for (float pitch : {1.f, .75f}) {
                auto out = run(rate, {"latency", pulse, {{pitch, fft}}, {}, {}, impulseFrames});
                size_t at = peak(out, firstImpulse - 2048, firstImpulse + 2 * fft + 4096);
                line("channel stage fft %4d pitch %.2f: impulse peak %+lld frames after dry\n", fft, pitch,
                     (long long)at - (long long)dryPeak);
                save("impulse-fft" + std::to_string(fft) + "-p" + std::to_string(int(pitch * 100)), out);
            }
        {
            auto out = run(rate, {"chain", pulse, {{1.f, 2048}, {1.f, 2048}}, {}, {}, impulseFrames});
            line("two chained stages fft 2048 pitch 1: impulse peak %+lld frames after dry\n",
                 (long long)peak(out, firstImpulse - 2048, firstImpulse + 12288) - (long long)dryPeak);
            auto group = run(rate, {"group", pulse, {}, {{1.f, 2048}}, {}, impulseFrames});
            line("group stage fft 2048 pitch 1: impulse peak %+lld frames after dry\n",
                 (long long)peak(group, firstImpulse - 2048, firstImpulse + 8192) - (long long)dryPeak);
        }
        // Pitch 1: identical to the dry signal (bypassed) or processed?
        const size_t noiseFrames = size_t(rate) * 2;
        auto hiss = padded(noise(noiseFrames), noiseFrames + 16384);
        auto dryNoise = run(rate, {"dry noise", hiss, {}, {}, {}, noiseFrames + 16384});
        save("noise-in", hiss);
        save("noise-dry", dryNoise);
        if (rate == 48000)
            writeMono(outDir / "noise-in.wav", hiss, 0, size_t(rate) * 2, rate, true);
        for (int fft : {1024, 4096}) {
            for (float pitch : {1.f, .75f}) {
                auto out = run(rate, {"noise", hiss, {{pitch, fft}}, {}, {}, noiseFrames + 16384});
                save("noise-fft" + std::to_string(fft) + "-p" + std::to_string(int(pitch * 100)), out);
                if (rate == 48000)
                    writeMono(outDir / ("noise-fft" + std::to_string(fft) + "-p" +
                                          std::to_string(int(pitch * 100)) + ".wav"),
                                out, size_t(rate) / 2, size_t(rate), rate);
                if (pitch != 1.f)
                    continue;
                long long bestShift = 0;
                double bestResidual = INFINITY;
                for (long long shift = 0; shift <= fft * 2; ++shift) {
                    double residual = 0;
                    for (size_t f = size_t(rate) / 2; f < size_t(rate); ++f) {
                        double d = double(out[(f + shift) * 2]) - dryNoise[f * 2];
                        residual += d * d;
                    }
                    if (residual < bestResidual)
                        bestResidual = residual, bestShift = shift;
                }
                double reference = rms(dryNoise, size_t(rate) / 2, size_t(rate));
                line("pitch 1 fft %4d on noise: best alignment %lld frames, residual %.1f dB below the signal\n", fft,
                     bestShift, -db(std::sqrt(bestResidual / (rate / 2.0)) / reference));
            }
        }
        // Gain.
        const size_t toneFrames = size_t(rate) * 2;
        auto tone = padded(sine(toneFrames, rate, 440), toneFrames + 16384);
        auto dryTone = run(rate, {"dry tone", tone, {}, {}, {}, toneFrames + 16384});
        double dryLevel = rms(dryTone, size_t(rate) / 2, size_t(rate) * 3 / 2);
        for (float pitch : {1.f, .75f}) {
            auto out = run(rate, {"tone", tone, {{pitch, 4096}}, {}, {}, toneFrames + 16384});
            line("sine 440 Hz fft 4096 pitch %.2f: level %+.2f dB against dry\n", pitch,
                 db(rms(out, size_t(rate) / 2, size_t(rate) * 3 / 2) / dryLevel));
            save("sine-fft4096-p" + std::to_string(int(pitch * 100)), out);
            if (rate == 48000) {
                writeMono(outDir / "sine-in.wav", tone, 0, size_t(rate) * 2, rate);
                if (pitch == .75f)
                    writeMono(outDir / "sine-fft4096-p75.wav", out, size_t(rate) / 2, size_t(rate), rate);
            }
        }
        // A stage on a shared bus processes music and effects as one sum. OBS has to process the two
        // separately, so record how far the linear sum of separate results is from FMOD's bus result.
        {
            Stereo mixed = hiss;
            for (size_t i = 0; i < mixed.size(); ++i)
                mixed[i] += tone[i];
            auto mixedOut = run(rate, {"mixed", mixed, {{.75f, 4096}}, {}, {}, framesOf(mixed)});
            auto noiseOut = run(rate, {"noise only", hiss, {{.75f, 4096}}, {}, {}, framesOf(hiss)});
            auto toneOut = run(rate, {"tone only", tone, {{.75f, 4096}}, {}, {}, framesOf(tone)});
            double xy = 0, xx = 0, yy = 0;
            for (size_t f = size_t(rate) / 2; f < size_t(rate) * 3 / 2; ++f) {
                double sum = double(noiseOut[f * 2]) + toneOut[f * 2], bus = mixedOut[f * 2];
                xy += sum * bus, xx += sum * sum, yy += bus * bus;
            }
            line("bus stage against separate stages over frames %zu to %zu: correlation %.3f, level %+.2f dB\n",
                 size_t(rate) / 2, size_t(rate) * 3 / 2, xy / std::sqrt(xx * yy), 10 * std::log10(yy / xx));
        }
        // What a stage does once its input ends: channel stage (channel ends with its sound) and group stage.
        const size_t shortFrames = size_t(rate) / 2, tail = size_t(rate);
        auto burst = sine(shortFrames, rate, 440);
        auto dryBurst = run(rate, {"dry burst", burst, {}, {}, {}, shortFrames + tail});
        size_t dryEnd = lastAudible(dryBurst, 0);
        for (bool onGroup : {false, true}) {
            Case c{"burst", burst, {}, {}, {}, shortFrames + tail};
            (onGroup ? c.groupStages : c.channelStages).push_back({.75f, 4096});
            auto out = run(rate, c);
            line("%s stage fft 4096 after its sound ends: last audible frame %+lld after dry's\n",
                 onGroup ? "group" : "channel", (long long)lastAudible(out, 0) - (long long)dryEnd);
            save(std::string(onGroup ? "burst-group" : "burst-channel"), out);
        }
        // A sound, a true digital gap, then another sound: FMOD's shifter gates a silent block, so it
        // holds the first sound's pending output across the gap and plays it when the second starts.
        {
            const size_t part = size_t(rate) / 4, tail = size_t(rate) / 2;
            Stereo gaps((part * 3 + tail) * 2, 0.f);
            auto first = sine(part, rate, 440), second = sine(part, rate, 660);
            std::copy(first.begin(), first.end(), gaps.begin());
            std::copy(second.begin(), second.end(), gaps.begin() + (part * 2) * 2);
            auto out = run(rate, {"gap", gaps, {{.75f, 4096}}, {}, {}, framesOf(gaps)});
            line("gap stage fft 4096: %zu frames, peak after the gap %zu\n", framesOf(gaps),
                 peak(out, part * 2, part * 3 + tail));
            if (rate == 48000) {
                writeMono(outDir / "burst-gap-in.wav", gaps, 0, framesOf(gaps), rate);
                writeMono(outDir / "burst-gap-out.wav", out, 0, framesOf(gaps), rate);
            }
        }
        // Pause: does a channel stage keep running (its tail plays during the pause) or freeze?
        {
            size_t pauseAt = size_t(rate), resumeAt = size_t(rate) * 3 / 2;
            auto longTone = sine(size_t(rate) * 2, rate, 440);
            auto out = run(rate, {"pause", longTone, {{.75f, 4096}}, {},
                                  {{pauseAt, Event::Pause}, {resumeAt, Event::Resume}}, size_t(rate) * 3});
            size_t window = 4096;
            line("channel stage fft 4096 paused at %zu: level just after pause %.1f dBFS, late in pause %.1f dBFS, "
                 "just after resume %.1f dBFS\n",
                 pauseAt, db(rms(out, pauseAt + 512, pauseAt + window)),
                 db(rms(out, resumeAt - window, resumeAt - 512)), db(rms(out, resumeAt + 256, resumeAt + window)));
            save("pause-channel", out);
        }
    }
    std::fclose(report);
    return 0;
}
