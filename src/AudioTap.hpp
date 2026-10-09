#pragma once
#include <cstdint>
#include <optional>
namespace separate_song::audio_tap {
void install();
void shutdown();
void enable(bool enabled);
// Wall time of a DSP clock value of the group the effects tap is on (GD's m_globalChannel), on the
// same timeline as the effects packets. Other groups' clocks drift from it while either is paused.
// Empty until the effects tap has processed a block.
std::optional<uint64_t> wallAtDspClock(unsigned long long clock);
} // namespace separate_song::audio_tap
