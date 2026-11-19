#pragma once
#include <cstdint>
#include <optional>

namespace separate_song::audio_tap {
void install();
void shutdown();
void enable(bool enabled);
// Whether OBS Jukebox takes effects (the mod's "enabled" setting).
bool enabled();
// Wall time of a DSP clock value on the effects timeline (GD's m_globalChannel). Other groups'
// clocks drift from it while either is paused. Empty until the tap has processed a block.
std::optional<uint64_t> wallAtDspClock(unsigned long long clock);
// The effects tap's index in GD's m_globalChannel DSP chain: DSPs at lower indices come after
// it, so it doesn't hear them. Empty while the tap isn't attached there.
std::optional<int> dspIndex();
}
