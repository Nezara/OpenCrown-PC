// Output backends for the libSceAudioOut shim. The only client is CRI's NcVoice backend ("CRI Audio Output" thread,
// notes/map/audio_pad.md): 48 kHz float ports of 256 frames, one batched sceAudioOutOutputs per mixer pass, paced
// purely by Outputs blocking. A sink receives interleaved float frames per port and must block for pacing.
#pragma once
#include <cstdint>
#include <memory>

namespace dcp::hle {

class AudioSink {
public:
    virtual ~AudioSink() = default;
    // Port opened/closed. channels = 1, 2 or 8; rate = 48000.
    virtual void open(int port, int channels, int rate) = 0;
    virtual void close(int port) = 0;
    // One mixer pass: ports[i] gets frames x channels interleaved floats (nullptr = silence). Called once per batch;
    // returns once the host can take the next batch (the game's only pacing).
    virtual void submit(int count, const int* ports, const float* const* data, int frames) = 0;
};

// Replace the active sink (default: wall-clock paced null sink, with DCP_AUDIO_WAV=path capturing the mix).
void set_audio_sink(std::unique_ptr<AudioSink> sink);

}  // namespace dcp::hle
