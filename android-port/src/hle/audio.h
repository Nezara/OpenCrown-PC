// Host sink for SceAudio output.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace hle {

// Writes everything played on 48 kHz ports to a 16-bit stereo WAV file (empty path = discard).
void audio_set_wav(const std::string& path);
// Host device sink: called from the game's audio thread with each 48 kHz grain as interleaved s16 stereo, port
// volume applied (the SDL3 platform layer plays it). `queued` returns the frames the device has not played yet:
// with a device, sceAudioOutOutput paces the game against it (keeps ~40 ms queued) instead of the wall clock, so
// the device never runs dry between grains.
void audio_set_sink(std::function<void(const int16_t* lr, uint32_t frames)> sink, std::function<uint32_t()> queued);

}  // namespace hle
