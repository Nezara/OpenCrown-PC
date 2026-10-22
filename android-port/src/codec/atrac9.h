// Our ATRAC9 decoder (Sony's Vita audio codec, used by the game's NGS ATRAC9 voices and CRI Atom).
//
// Stream layout: a 4-byte config word (0xFE, then sample-rate index 4 bits, channel config 3, a zero bit, frame
// bytes - 1 in 11 bits, superframe index 2) fixes everything: constant-size superframes of 2^index frames, each
// frame 64/128/256 samples per channel. Frames are decoded in order (each depends on the previous one's state).
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace codec {

class Atrac9 {
 public:
  Atrac9();
  ~Atrac9();

  // `config` = the 4 config bytes in stream order (first byte 0xFE). Resets all decoding state.
  bool init(const uint8_t config[4]);
  // Clears the inter-frame state (overlap, previous scale factors, noise generator) but keeps the configuration.
  void reset();

  uint32_t channels() const { return channels_; }
  uint32_t sample_rate() const { return sample_rate_; }
  uint32_t frame_samples() const { return frame_samples_; }
  uint32_t superframe_bytes() const { return superframe_bytes_; }
  uint32_t superframe_samples() const { return frame_samples_ * frames_per_superframe_; }

  // Decodes one superframe (exactly superframe_bytes()) into superframe_samples() x channels() interleaved samples
  // in 16-bit scale (±32768, not clamped). Returns false on a bitstream error (the output is then silence).
  bool decode_superframe(const uint8_t* data, float* out);

 private:
  struct State;
  std::unique_ptr<State> s_;
  uint32_t channels_ = 0, sample_rate_ = 0, frame_samples_ = 0, frames_per_superframe_ = 0, superframe_bytes_ = 0;
};

}  // namespace codec
