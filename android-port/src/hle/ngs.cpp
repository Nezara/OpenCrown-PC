// SceNgs: our own voice/rack/patch synthesiser. All bookkeeping is host-side; the game's system and rack buffers
// hold only what the game reads itself: each voice's module parameter blocks (returned by sceNgsVoiceLockParams and
// read live every update). sceNgsSystemUpdate renders one grain through the patch graph; the master buss output
// module's state data is the grain as s16 stereo, which the game copies to sceAudioOutOutput.
//
// API semantics: vitasdk psp2/ngs.h (MIT). Module ids, parameter layouts and the module lists of the voice
// definitions are interface facts cross-checked against Vita3K's NGS headers (read as reference only, no code
// taken) and against the game's own use (asm notes below).
//
// Modelled: PCM s16, VAG ADPCM and ATRAC9 (our decoder, codec/atrac9) players with buffers, loops, swaps, end of
// data and their callbacks, ATRAC9 per-buffer sample discards, input mixing, patches with 2x2 gain matrices, output
// to s16. Effects (EQ, envelope, distortion, filters, reverb, delay, generator, mixer gains) pass the signal through
// unchanged for now; key-off keeps the voice ACTIVE | KEY_OFF until the envelope's release time has run out.
//
// How the game drives the players (1.09 asm, CRI's NGS wrapper):
//   - voice setup 0x81440a5a: LockParams through 0x81459c62 / 0x81459cc8, which memsets the whole block (0x54 player,
//     0x60 ATRAC9) and rewrites the descriptor; then buffer 0 {next 1}, buffer 1 {next 0}, loop counts 0, ptr/bytes
//     still 0. The game never uses NGS loop counts (always 0); its loops are buffer chains.
//   - refill 0x81459d2e fills buffer [obj+0xf] (alternating 0/1) and links it to the other one; the last chunk of a
//     sound gets next = -1. ATRAC9 loop points split a chunk into buffer idx -> buffer 2 -> other, with
//     samples_discard_start/end at the seam (0x8145a294) and a 256-sample discard on the first buffer (0x81459ee0).
//     The voice server prefills both buffers (0x814415d2), or one when the source is complete, then VoicePlay.
//   - module callback 0x8145ab9c: reason 0 (end of data) ignored; otherwise reason2 = index of the finished buffer,
//     and the game refills the next buffer from inside the callback.
//   - status poll 0x8145a868: a voice counts as finished when (voice_state & 7) == 0; a stopped voice (KeyOff at
//     0x8145a9e2) is only reused after that, so key-off must report ACTIVE | KEY_OFF, not KEY_OFF alone.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "codec/atrac9.h"
#include "hle/common.h"
#include "hle/kobj.h"

namespace hle {
namespace {

enum : uint32_t {
  NGS_OK = 0,
  NGS_ERROR_INVALID_PARAM = 0x804A0002,
  NGS_ERROR_INVALID_ALIGNMENT = 0x804A0003,
  NGS_ERROR_OUT_OF_ASSETS = 0x804A0006,
  NGS_ERROR_RESOURCE_LOCKED = 0x804A0008,
  NGS_ERROR_INVALID_HANDLE = 0x804A000C,
  NGS_ERROR_SIZE_MISMATCH = 0x804A000D,
  NGS_ERROR_PATCH_NOT_AVAIL = 0x804A000E,
  NGS_ERROR_PARAM_TYPE_MISMATCH = 0x804A000F,
  NGS_ERROR_INVALID_STATE = 0x804A0010,
};

enum VoiceState : uint32_t {
  STATE_AVAILABLE = 0,
  STATE_ACTIVE = 1,
  STATE_PAUSED = 0x20,
  STATE_KEY_OFF = 0x40,
};

// Module type ids (low 16 bits of their parameter-struct ids 0x0101xxxx).
enum ModId : uint32_t {
  MOD_AT9 = 0x5CAA,
  MOD_INPUT = 0x5CE0,  // input mixer of a buss: no parameters
  MOD_COMPRESSOR = 0x5CE1,
  MOD_DISTORTION = 0x5CE2,
  MOD_ENVELOPE = 0x5CE3,
  MOD_FILTER = 0x5CE4,
  MOD_PAUSER = 0x5CE5,
  MOD_PLAYER = 0x5CE6,
  MOD_REVERB = 0x5CE7,
  MOD_GENERATOR = 0x5CE8,
  MOD_MIXER = 0x5CE9,
  MOD_DELAY = 0x5CEB,
  MOD_EQ = 0x5CEC,
  MOD_PITCHSHIFT = 0x5CED,  // unverified
  MOD_OUTPUT = 0x5CEF,  // master buss output: id unverified (only reported in callbacks)
};

// Parameter block sizes (descriptor included) and fixed state-data sizes (0 = any size, zero-filled).
uint32_t param_size(uint32_t m) {
  switch (m) {
    case MOD_AT9: return 0x60;
    case MOD_PLAYER: return 0x54;
    case MOD_GENERATOR: return 0x20;
    case MOD_MIXER: return 0x18;
    case MOD_EQ: return 0x58;
    case MOD_ENVELOPE: return 0x48;
    case MOD_DISTORTION: return 0x20;
    case MOD_FILTER: return 0x1c;
    case MOD_PAUSER: return 0x10;
    case MOD_REVERB: return 0x4c;
    case MOD_DELAY: return 0x80;
    case MOD_COMPRESSOR: return 0x28;
    default: return 0;
  }
}
uint32_t fixed_state_size(uint32_t m) {
  switch (m) {
    case MOD_AT9:
    case MOD_PLAYER: return 0x18;
    case MOD_ENVELOPE: return 0x14;
    case MOD_COMPRESSOR: return 0x10;
    default: return 0;
  }
}

struct Def {
  const char* name;
  std::vector<uint32_t> mods;
  uint32_t outputs;
  bool buss;  // has one input
};
const Def DEF_TEMPLATE1{"Template1", {MOD_PLAYER, MOD_GENERATOR, MOD_MIXER, MOD_EQ, MOD_ENVELOPE, MOD_DISTORTION,
                                      MOD_EQ, MOD_EQ, MOD_EQ, MOD_EQ}, 4, false};
const Def DEF_ATRAC9{"Atrac9Voice", {MOD_AT9, MOD_GENERATOR, MOD_MIXER, MOD_EQ, MOD_ENVELOPE, MOD_DISTORTION, MOD_EQ,
                                     MOD_EQ, MOD_EQ, MOD_EQ}, 4, false};
const Def DEF_MASTER{"MasterBuss", {MOD_INPUT, MOD_OUTPUT}, 1, true};
const Def DEF_REVERB{"ReverbBuss", {MOD_INPUT, MOD_REVERB}, 1, true};
const Def DEF_DELAY{"DelayBuss", {MOD_INPUT, MOD_DELAY}, 1, true};
const Def DEF_DISTORTION{"DistortionBuss", {MOD_INPUT, MOD_DISTORTION}, 1, true};
const Def DEF_COMPRESSOR{"CompressorBuss", {MOD_INPUT, MOD_COMPRESSOR}, 1, true};
const Def DEF_EQ{"EqBuss", {MOD_INPUT, MOD_EQ}, 1, true};
const Def DEF_MIXER{"MixerBuss", {MOD_INPUT}, 1, true};
// Pitch shift module: id unverified and no parameter block modelled (effects pass through).
const Def DEF_PITCHSHIFT{"PitchShiftBuss", {MOD_INPUT, MOD_PITCHSHIFT}, 1, true};
const Def* const DEFS[] = {&DEF_TEMPLATE1, &DEF_ATRAC9,     &DEF_MASTER, &DEF_REVERB, &DEF_DELAY,
                           &DEF_DISTORTION, &DEF_COMPRESSOR, &DEF_EQ,     &DEF_MIXER,  &DEF_PITCHSHIFT};

// The game only stores a definition pointer into SceNgsRackDescription, so each definition is a 16-byte tag in
// guest memory (allocated on first use) that we map back.
uint32_t def_tags = 0;
const Def* def_from_va(uint32_t va) {
  if (!def_tags || va < def_tags || (va - def_tags) % 16 || (va - def_tags) / 16 >= std::size(DEFS)) return nullptr;
  return DEFS[(va - def_tags) / 16];
}

struct System;
struct Rack;
struct Voice;

struct Patch {
  uint32_t handle;
  Voice* src;
  int32_t out_index, sub_index;
  Voice* dst;
  int32_t in_index;
  float m[2][2];  // [source channel][destination channel]
};

struct Source {  // player / ATRAC9 module runtime
  int32_t buffer = 0;          // current buffer index
  uint32_t pos = 0;            // byte position in it
  int32_t loops = 0;           // loops done on the current buffer
  int32_t samples_since_key_on = 0, bytes_since_key_on = 0, samples_total = 0, bytes_total = 0;
  bool ended = false;
  std::deque<float> pcm;       // decoded, interleaved by source channel count
  double frac = 0;             // resampler phase
  float prev[2] = {0, 0}, cur[2] = {0, 0};
  float vag_hist[2][2] = {};   // VAG ADPCM s1, s2 per channel
  std::unique_ptr<codec::Atrac9> at9;
  uint32_t at9_config = 0;
  std::vector<float> at9_out;
  int32_t discard_left = 0;    // ATRAC9 samples still to drop at the start of the current buffer
  uint32_t produced = 0;       // samples decoded since the last buffer transition (key-on / loop / swap)
  uint32_t left_mask = 0;      // buffers swapped away from during this grain (refill callback not run yet)
};

struct ModCallback {
  uint32_t fn = 0, user = 0;
};

struct Voice {
  uint32_t handle;
  Rack* rack;
  uint32_t state = STATE_AVAILABLE;
  std::vector<uint32_t> params;  // guest address of each module's parameter block
  std::vector<bool> locked, bypassed;
  std::vector<ModCallback> callbacks;
  std::vector<std::vector<Patch*>> out_patches;  // [output][slot]
  std::vector<Patch*> in_patches;
  uint32_t update_passed = 0;
  int64_t release_left = -1;  // samples until a keyed-off voice ends
  Source src;
  std::vector<float> in, out;  // grain buffers, stereo interleaved
  std::vector<int16_t> pcm_out;  // output module state (master buss)
};

struct Rack {
  uint32_t handle;
  System* sys;
  const Def* def;
  uint32_t channels, patches_per_output, user_data;
  std::vector<std::unique_ptr<Voice>> voices;
};

struct System {
  uint32_t handle;
  uint32_t granularity, sample_rate, max_racks, max_voices;
  uint32_t scratch;  // guest block for callback info records
  std::vector<std::unique_ptr<Rack>> racks;
};

// Held by every NGS call and across the grain render (SystemUpdate). DCP_LOCK_STATS=1 times contended waits for the
// [timing] ngs_lock numbers (sync.cpp: lock_stats_report).
struct NgsLock {
  std::recursive_mutex m;
  void lock() {
    if (!lock_stats_on()) return m.lock();
    if (m.try_lock()) return;
    auto t0 = std::chrono::steady_clock::now();
    m.lock();
    note_ngs_lock_wait(uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
  }
  void unlock() { m.unlock(); }
};
NgsLock ngs_lock;
std::map<uint32_t, std::unique_ptr<System>> systems;
std::map<uint32_t, Rack*> racks;
std::map<uint32_t, Voice*> voices;
std::map<uint32_t, std::unique_ptr<Patch>> patches;
uint32_t next_patch = 0x50000001;

Voice* find_voice(uint32_t h) {
  auto it = voices.find(h);
  return it == voices.end() ? nullptr : it->second;
}

// --- parameter defaults ------------------------------------------------------------------------------------------
void write_defaults(Cpu& c, uint32_t va, uint32_t mod) {
  uint32_t size = param_size(mod);
  if (!size) return;
  Memory& m = c.mem();
  std::memset(m.ptr(va), 0, size);
  m.write<uint32_t>(va, 0x01010000u | mod);
  m.write<uint32_t>(va + 4, size);
  auto f = [&](uint32_t off, float v) { m.write<float>(va + off, v); };
  switch (mod) {
    case MOD_PLAYER:
    case MOD_AT9:
      for (uint32_t i = 0; i < 4; i++) {
        uint32_t b = va + 8 + i * (mod == MOD_AT9 ? 16 : 12);
        m.write<int16_t>(b + 10, -1);  // next_buffer_index
      }
      f(mod == MOD_AT9 ? 0x48 : 0x38, 48000.0f);  // playback_frequency
      f(mod == MOD_AT9 ? 0x4c : 0x3c, 1.0f);      // playback_scalar
      m.write<int32_t>(va + (mod == MOD_AT9 ? 0x54 : 0x44), 0);
      m.write<int8_t>(va + (mod == MOD_AT9 ? 0x58 : 0x4c), 1);  // channels
      m.write<int8_t>(va + (mod == MOD_AT9 ? 0x5a : 0x4e), 1);  // channel_map[1]
      break;
    case MOD_MIXER: f(8, 1.0f); f(12, 1.0f); break;
    case MOD_ENVELOPE: m.write<uint32_t>(va + 0x3c, 0); break;
    case MOD_DISTORTION: f(0x18, 0.0f); f(0x1c, 1.0f); break;  // wet 0, dry 1
    default: break;
  }
}

// --- sources -----------------------------------------------------------------------------------------------------
// Layouts (offsets in the parameter block):
//   player: buffers at 8, 12 bytes each {ptr, bytes, s16 loop_count, s16 next}; frequency 0x38, scalar 0x3c,
//           start_bytes 0x48, channels 0x4c, map 0x4d, type 0x4f (0 PCM s16, 1 VAG ADPCM), start_buffer 0x51
//   ATRAC9: buffers at 8, 16 bytes each {ptr, bytes, s16 loop_count, s16 next, s16 skip start, s16 skip end};
//           frequency 0x48, scalar 0x4c, channels 0x58, map 0x59, config_data 0x5c
struct BufferRef {
  uint32_t ptr, bytes;
  int16_t loop_count, next;
  int16_t discard_start, discard_end;  // ATRAC9 only: samples dropped from the buffer's start / end
};
BufferRef buffer_at(Memory& m, uint32_t params, bool at9, int32_t i) {
  uint32_t b = params + 8 + uint32_t(i) * (at9 ? 16 : 12);
  return {m.read<uint32_t>(b),     m.read<uint32_t>(b + 4),
          m.read<int16_t>(b + 8),  m.read<int16_t>(b + 10),
          at9 ? m.read<int16_t>(b + 12) : int16_t(0), at9 ? m.read<int16_t>(b + 14) : int16_t(0)};
}

struct PendingCallback {
  uint32_t fn, voice, rack, module_id, reason, reason2, ptr, user;
};

// Rate-limited notes for the safety nets below (none of them should trigger in normal play).
void ngs_note(const char* what, const Voice& v, const BufferRef& b, int32_t idx, bool at9) {
  static int noted = 0;
  if (noted >= 16) return;
  noted++;
  HLE_NOTE("[ngs] voice 0x%08x (%s) buffer %d (0x%08x, %u bytes, loops %d, next %d): %s\n", v.handle,
           at9 ? "ATRAC9" : "player", idx, b.ptr, b.bytes, b.loop_count, b.next, what);
}

enum class Fetch { DATA, WAIT, END };

// Decodes the next chunk of the voice's current buffer into src.pcm.
//   DATA: bytes were consumed (samples may still be zero: a cut-short superframe, discarded samples);
//   WAIT: nothing to read this grain, the voice stays where it is (silent) and tries again next grain;
//   END:  the data has ended (end-of-data callback queued).
// Buffer switches queue module callbacks: reason 0 end of data, 1 swapped buffer, loop = 2 (player) / 4 (ATRAC9);
// reason2 = index of the buffer just finished (the game's callback 0x8145ab9c compares it with buffer indices),
// ptr = its address.
Fetch decode_more(Cpu& c, Voice& v, uint32_t mod, std::vector<PendingCallback>& cbs) {
  Memory& m = c.mem();
  Source& s = v.src;
  bool at9 = mod == MOD_AT9;
  uint32_t p = v.params[0];
  int chans = std::clamp<int>(m.read<int8_t>(p + (at9 ? 0x58 : 0x4c)), 1, 2);
  auto queue = [&](uint32_t reason, int32_t idx, uint32_t ptr) {
    if (v.callbacks[0].fn)
      cbs.push_back({v.callbacks[0].fn, v.handle, v.rack->handle, mod, reason, uint32_t(idx), ptr, v.callbacks[0].user});
  };
  // At most 4 swaps (each target must not have been left this grain) plus one loop can happen per call.
  for (int guard = 0; guard < 8; guard++) {
    if (s.buffer < 0 || s.buffer > 3) return Fetch::END;
    BufferRef b = buffer_at(m, p, at9, s.buffer);
    // A buffer without address or bytes is not filled yet (setup links buffer 0 <-> 1 before the game fills them,
    // from its voice server and its swap callbacks): the voice waits on it, silent, and reads it once it is set.
    if (!b.ptr || !b.bytes) return Fetch::WAIT;
    if (s.pos < b.bytes) {
      uint32_t n = 0, got = 0;
      if (at9) {
        // ATRAC9: one superframe per step through our decoder (codec/atrac9), re-initialised when the voice's
        // config word changes. A superframe cut short by the buffer end is skipped. The buffer's discard counts
        // drop samples from the start of its first superframe(s) and the end of its last whole superframe.
        uint32_t cfg = m.read<uint32_t>(p + 0x5c);
        if (!s.at9 || cfg != s.at9_config) {
          uint8_t bytes[4] = {uint8_t(cfg), uint8_t(cfg >> 8), uint8_t(cfg >> 16), uint8_t(cfg >> 24)};
          s.at9 = std::make_unique<codec::Atrac9>();
          s.at9_config = cfg;
          if (!s.at9->init(bytes)) {
            s.at9.reset();
            queue(3, s.buffer, b.ptr);  // SCE_NGS_AT9_DECODE_ERROR (config rejected)
            return Fetch::END;
          }
        }
        codec::Atrac9& dec = *s.at9;
        uint32_t sfb = dec.superframe_bytes(), sfs = dec.superframe_samples(), dch = dec.channels();
        if (s.pos == 0) s.discard_left = std::max<int32_t>(0, b.discard_start);
        n = std::min(sfb, b.bytes - s.pos);
        if (n == sfb) {
          s.at9_out.resize(size_t(sfs) * dch);
          if (!dec.decode_superframe(m.ptr(b.ptr + s.pos), s.at9_out.data())) {
            s.pos += n;
            queue(3, s.buffer, b.ptr);  // SCE_NGS_AT9_DECODE_ERROR
            return Fetch::END;
          }
          uint32_t count = sfs;
          if (b.bytes - s.pos - n < sfb)  // last whole superframe of the buffer
            count -= std::min<uint32_t>(sfs, uint32_t(std::max<int32_t>(0, b.discard_end)));
          uint32_t first = std::min<uint32_t>(count, uint32_t(s.discard_left));
          s.discard_left -= int32_t(first);
          for (uint32_t i = first; i < count; i++)
            for (int ch = 0; ch < chans; ch++)
              s.pcm.push_back(s.at9_out[size_t(i) * dch + std::min<uint32_t>(uint32_t(ch), dch - 1)] / 32768.0f);
          got = count - first;
        }
      } else if (m.read<int8_t>(p + 0x4f) == 1) {  // VAG ADPCM: 16-byte frames of 28 samples, channels interleaved
        static const float coef[5][2] = {{0, 0}, {60 / 64.f, 0}, {115 / 64.f, -52 / 64.f}, {98 / 64.f, -55 / 64.f},
                                         {122 / 64.f, -60 / 64.f}};
        n = std::min<uint32_t>(16u * chans, b.bytes - s.pos);
        if (n < 16u * chans) n = b.bytes - s.pos;  // trailing partial frame: skip
        else {
          float frame[2][28] = {};
          for (int ch = 0; ch < chans; ch++) {
            const uint8_t* f = m.ptr(b.ptr + s.pos + 16u * ch);
            int shift = f[0] & 15, filt = std::min(f[0] >> 4, 4);
            for (int i = 0; i < 28; i++) {
              int nib = (f[2 + i / 2] >> ((i & 1) * 4)) & 15;
              int32_t raw = int32_t(int16_t(uint16_t(nib << 12))) >> shift;
              float out = float(raw) + s.vag_hist[ch][0] * coef[filt][0] + s.vag_hist[ch][1] * coef[filt][1];
              s.vag_hist[ch][1] = s.vag_hist[ch][0];
              s.vag_hist[ch][0] = out;
              frame[ch][i] = std::clamp(out, -32768.f, 32767.f) / 32768.f;
            }
          }
          for (int i = 0; i < 28; i++)
            for (int ch = 0; ch < chans; ch++) s.pcm.push_back(frame[ch][i]);
          got = 28;
        }
      } else {  // PCM s16
        uint32_t frame = 2u * chans;
        n = std::min<uint32_t>(1024u * frame, (b.bytes - s.pos) / frame * frame);
        if (!n) n = b.bytes - s.pos;  // trailing partial frame: skip
        const int16_t* src = reinterpret_cast<const int16_t*>(m.ptr(b.ptr + s.pos));
        for (uint32_t i = 0; i < n / frame * chans; i++) s.pcm.push_back(src[i] / 32768.f);
        got = n / frame;
      }
      s.pos += n;
      s.produced += got;
      s.samples_since_key_on += int32_t(got);
      s.samples_total += int32_t(got);
      s.bytes_since_key_on += int32_t(n);
      s.bytes_total += int32_t(n);
      return Fetch::DATA;
    }
    // Current buffer done: loop, swap, or end.
    if (b.loop_count < 0 || s.loops < b.loop_count) {
      if (!s.produced) {  // a loop yielding no samples would spin forever
        ngs_note("loops without yielding samples, silent until it changes", v, b, s.buffer, at9);
        return Fetch::WAIT;
      }
      s.loops++;
      s.pos = 0;
      s.produced = 0;
      queue(at9 ? 4 : 2, s.buffer, b.ptr);
      continue;
    }
    int32_t done = s.buffer;
    if (b.next < 0 || b.next > 3) {
      queue(0, done, b.ptr);
      return Fetch::END;
    }
    // The game refills a finished buffer from the swap callback, which runs after this grain: a buffer left during
    // this grain still holds the data just played, so it is read next grain at the earliest.
    if (v.callbacks[0].fn && (s.left_mask >> b.next & 1)) return Fetch::WAIT;
    s.left_mask |= 1u << done;
    s.buffer = b.next;
    s.pos = 0;
    s.loops = 0;
    s.produced = 0;
    queue(1, done, b.ptr);
  }
  return Fetch::WAIT;
}

// Fills v.out (stereo, granularity frames) from the voice's source, resampled to the system rate.
void render_source(Cpu& c, Voice& v, uint32_t mod, uint32_t gran, uint32_t rate, std::vector<PendingCallback>& cbs) {
  Memory& m = c.mem();
  Source& s = v.src;
  bool at9 = mod == MOD_AT9;
  uint32_t p = v.params[0];
  int chans = std::clamp<int>(m.read<int8_t>(p + (at9 ? 0x58 : 0x4c)), 1, 2);
  float freq = m.read<float>(p + (at9 ? 0x48 : 0x38)) * m.read<float>(p + (at9 ? 0x4c : 0x3c));
  double step = (std::isfinite(freq) && freq > 0) ? freq / double(rate) : 1.0;
  step = std::clamp(step, 1.0 / 64, 16.0);
  s.left_mask = 0;
  // Safety net: decode_more's rules bound the work per grain; a voice needing more attempts than this is silent
  // for the rest of the grain and noted (the old unbounded loop ran the host out of memory).
  int budget = 64 + int(gran * step);
  bool wait = false;
  for (uint32_t i = 0; i < gran; i++) {
    while (s.frac >= 1.0) {
      while (!s.ended && !wait && s.pcm.size() < size_t(chans)) {
        if (--budget < 0) {
          ngs_note("decode budget exhausted, silent for the rest of the grain", v,
                   buffer_at(m, p, at9, std::clamp(s.buffer, 0, 3)), s.buffer, at9);
          wait = true;
          break;
        }
        Fetch f = decode_more(c, v, mod, cbs);
        if (f == Fetch::END) s.ended = true;
        else if (f == Fetch::WAIT) wait = true;
      }
      for (int ch = 0; ch < chans; ch++) {
        s.prev[ch] = s.cur[ch];
        s.cur[ch] = s.pcm.empty() ? 0.0f : s.pcm.front();
        if (!s.pcm.empty()) s.pcm.pop_front();
      }
      s.frac -= 1.0;
    }
    float t = float(s.frac);
    float l = s.prev[0] + (s.cur[0] - s.prev[0]) * t;
    float r = chans == 2 ? s.prev[1] + (s.cur[1] - s.prev[1]) * t : l;
    v.out[2 * i] = l;
    v.out[2 * i + 1] = r;
    s.frac += step;
  }
}

void reset_source(Voice& v) {
  v.src = Source{};
  v.src.frac = 1.0;
}

// --- graph -------------------------------------------------------------------------------------------------------
void update_system(Cpu& c, System& sys, std::vector<PendingCallback>& cbs) {
  uint32_t gran = sys.granularity;
  std::vector<Voice*> all;
  for (auto& r : sys.racks)
    for (auto& v : r->voices) {
      std::fill(v->in.begin(), v->in.end(), 0.0f);
      all.push_back(v.get());
    }
  // Topological order over patches (sources before the busses they feed).
  std::map<Voice*, int> indeg;
  for (Voice* v : all) indeg[v] = 0;
  for (auto& [h, p] : patches)
    if (indeg.count(p->src) && indeg.count(p->dst)) indeg[p->dst]++;
  std::vector<Voice*> order, ready;
  for (Voice* v : all)
    if (!indeg[v]) ready.push_back(v);
  while (!ready.empty()) {
    Voice* v = ready.back();
    ready.pop_back();
    order.push_back(v);
    for (auto& outs : v->out_patches)
      for (Patch* p : outs)
        if (p && --indeg[p->dst] == 0) ready.push_back(p->dst);
  }

  for (Voice* v : order) {
    std::fill(v->out.begin(), v->out.end(), 0.0f);
    bool running = (v->state & (STATE_ACTIVE | STATE_KEY_OFF)) && !(v->state & STATE_PAUSED);
    if (!running) continue;
    const Def* d = v->rack->def;
    uint32_t mod0 = d->mods[0];
    if (mod0 == MOD_PLAYER || mod0 == MOD_AT9) {
      if (!v->bypassed[0]) render_source(c, *v, mod0, gran, sys.sample_rate, cbs);
      if (v->src.ended && v->src.pcm.empty()) v->state = STATE_AVAILABLE;  // played to the end
    } else {
      v->out = v->in;
    }
    if (v->state & STATE_KEY_OFF) {
      v->release_left -= gran;
      if (v->release_left <= 0) v->state = STATE_AVAILABLE;
    }
    v->update_passed++;
    // Output module (master buss): s16 stereo grain.
    for (size_t mi = 0; mi < d->mods.size(); mi++)
      if (d->mods[mi] == MOD_OUTPUT)
        for (uint32_t i = 0; i < gran * 2; i++)
          v->pcm_out[i] = int16_t(std::lround(std::clamp(v->out[i], -1.0f, 1.0f) * 32767.0f));
    // Patches into the destination inputs.
    uint32_t sc = v->rack->channels;
    for (auto& outs : v->out_patches)
      for (Patch* p : outs) {
        if (!p) continue;
        uint32_t dc = p->dst->rack->channels;
        for (uint32_t i = 0; i < gran; i++) {
          float s0 = v->out[2 * i], s1 = sc == 2 ? v->out[2 * i + 1] : 0.0f;
          float d0 = s0 * p->m[0][0] + s1 * p->m[1][0];
          float d1 = dc == 2 ? s0 * p->m[0][1] + s1 * p->m[1][1] : 0.0f;
          p->dst->in[2 * i] += d0;
          p->dst->in[2 * i + 1] += dc == 2 ? d1 : d0;
        }
      }
  }
}

// Runs queued callbacks with the NGS lock released (callbacks call back into NGS, and may block on game locks).
void run_callbacks(Cpu& c, System& sys, std::vector<PendingCallback>& cbs) {
  for (const PendingCallback& cb : cbs) {
    uint32_t info = sys.scratch;
    Memory& m = c.mem();
    m.write<uint32_t>(info, cb.voice);
    m.write<uint32_t>(info + 4, cb.rack);
    m.write<uint32_t>(info + 8, cb.module_id);
    m.write<uint32_t>(info + 12, cb.reason);
    m.write<uint32_t>(info + 16, cb.reason2);
    m.write<uint32_t>(info + 20, cb.ptr);
    m.write<uint32_t>(info + 24, cb.user);
    c.call(cb.fn, {info});
  }
}

// --- system ------------------------------------------------------------------------------------------------------
constexpr uint32_t SYSTEM_MEMORY = 0x100;  // the game requires < 0x2000

bool valid_init_params(Cpu& c, uint32_t p) {
  if (!p) return false;
  int32_t racks = int32_t(c.mem().read<uint32_t>(p)), voices = int32_t(c.mem().read<uint32_t>(p + 4));
  int32_t gran = int32_t(c.mem().read<uint32_t>(p + 8)), rate = int32_t(c.mem().read<uint32_t>(p + 12));
  return racks >= 1 && racks <= 64 && voices >= 1 && voices <= 256 && gran >= 64 && gran <= 512 && gran % 64 == 0 &&
         rate == 48000;
}

void SystemGetRequiredMemorySize(Cpu& c) {
  if (!valid_init_params(c, arg(c, 0)) || !arg(c, 1)) return ret(c, NGS_ERROR_INVALID_PARAM);
  c.mem().write<uint32_t>(arg(c, 1), SYSTEM_MEMORY);
  ret(c, NGS_OK);
}

// sceNgsSystemInit(memory, size, params*, handle*)
void SystemInit(Cpu& c) {
  uint32_t mem = arg(c, 0), size = arg(c, 1), p = arg(c, 2), out = arg(c, 3);
  if (!valid_init_params(c, p) || !out || !mem) return ret(c, NGS_ERROR_INVALID_PARAM);
  if (mem & 15) return ret(c, NGS_ERROR_INVALID_ALIGNMENT);
  if (size < SYSTEM_MEMORY) return ret(c, NGS_ERROR_INVALID_PARAM);
  std::lock_guard lk(ngs_lock);
  if (!def_tags) def_tags = c.mem().alloc(0x1000, "NGS voice definitions");
  auto sys = std::make_unique<System>();
  sys->handle = mem;
  sys->max_racks = c.mem().read<uint32_t>(p);
  sys->max_voices = c.mem().read<uint32_t>(p + 4);
  sys->granularity = c.mem().read<uint32_t>(p + 8);
  sys->sample_rate = c.mem().read<uint32_t>(p + 12);
  sys->scratch = mem + 0x40;  // callback info record lives in the system's own memory
  std::memset(c.mem().ptr(mem), 0, SYSTEM_MEMORY);
  HLE_NOTE("sceNgsSystemInit: %u racks, %u voices, grain %u, %u Hz\n", sys->max_racks, sys->max_voices,
           sys->granularity, sys->sample_rate);
  systems[mem] = std::move(sys);
  c.mem().write<uint32_t>(out, mem);
  ret(c, NGS_OK);
}

void SystemRelease(Cpu& c) {
  std::lock_guard lk(ngs_lock);
  auto it = systems.find(arg(c, 0));
  if (it == systems.end()) return ret(c, NGS_ERROR_INVALID_HANDLE);
  if (!it->second->racks.empty()) return ret(c, NGS_ERROR_INVALID_STATE);
  systems.erase(it);
  ret(c, NGS_OK);
}

void SystemUpdate(Cpu& c) {
  std::vector<PendingCallback> cbs;
  System* sys;
  {
    std::lock_guard lk(ngs_lock);
    auto it = systems.find(arg(c, 0));
    if (it == systems.end()) return ret(c, NGS_ERROR_INVALID_HANDLE);
    sys = it->second.get();
    update_system(c, *sys, cbs);
  }
  run_callbacks(c, *sys, cbs);
  ret(c, NGS_OK);
}

// --- racks -------------------------------------------------------------------------------------------------------
struct RackDesc {
  const Def* def;
  int32_t voices, channels, patches_per_input, patches_per_output;
  uint32_t user;
};
bool read_desc(Cpu& c, uint32_t p, RackDesc& d) {
  if (!p) return false;
  Memory& m = c.mem();
  d = {def_from_va(m.read<uint32_t>(p)), int32_t(m.read<uint32_t>(p + 4)), int32_t(m.read<uint32_t>(p + 8)),
       int32_t(m.read<uint32_t>(p + 12)), int32_t(m.read<uint32_t>(p + 16)), m.read<uint32_t>(p + 20)};
  return d.def && d.voices > 0 && (d.channels == 1 || d.channels == 2) && d.patches_per_output >= 0 &&
         (!d.def->buss || d.patches_per_input > 0);
}
uint32_t voice_bytes(const Def& d) {
  uint32_t n = 0x10;  // voice header (the handle points here)
  for (uint32_t mod : d.mods) n += (param_size(mod) + 15) & ~15u;
  return n;
}
uint32_t rack_bytes(const RackDesc& d) { return 0x10 + uint32_t(d.voices) * voice_bytes(*d.def); }

void RackGetRequiredMemorySize(Cpu& c) {
  RackDesc d;
  std::lock_guard lk(ngs_lock);
  if (!systems.count(arg(c, 0))) return ret(c, NGS_ERROR_INVALID_HANDLE);
  if (!read_desc(c, arg(c, 1), d) || !arg(c, 2)) return ret(c, NGS_ERROR_INVALID_PARAM);
  c.mem().write<uint32_t>(arg(c, 2), rack_bytes(d));
  ret(c, NGS_OK);
}

// sceNgsRackInit(system, SceNgsBufferInfo* buffer, desc*, rack*)
void RackInit(Cpu& c) {
  std::lock_guard lk(ngs_lock);
  auto sit = systems.find(arg(c, 0));
  if (sit == systems.end()) return ret(c, NGS_ERROR_INVALID_HANDLE);
  System& sys = *sit->second;
  RackDesc d;
  uint32_t bi = arg(c, 1), out = arg(c, 3);
  if (!bi || !out || !read_desc(c, arg(c, 2), d)) return ret(c, NGS_ERROR_INVALID_PARAM);
  uint32_t base = c.mem().read<uint32_t>(bi), size = c.mem().read<uint32_t>(bi + 4);
  if (base & 15) return ret(c, NGS_ERROR_INVALID_ALIGNMENT);
  if (size < rack_bytes(d) || !c.mem().valid(base, rack_bytes(d))) return ret(c, NGS_ERROR_INVALID_PARAM);
  uint32_t used = 0;
  for (auto& r : sys.racks) used += uint32_t(r->voices.size());
  if (sys.racks.size() >= sys.max_racks || used + uint32_t(d.voices) > sys.max_voices)
    return ret(c, NGS_ERROR_OUT_OF_ASSETS);

  auto rack = std::make_unique<Rack>();
  rack->handle = base;
  rack->sys = &sys;
  rack->def = d.def;
  rack->channels = uint32_t(d.channels);
  rack->patches_per_output = uint32_t(d.patches_per_output);
  rack->user_data = d.user;
  std::memset(c.mem().ptr(base), 0, rack_bytes(d));
  uint32_t va = base + 0x10;
  for (int32_t i = 0; i < d.voices; i++) {
    auto v = std::make_unique<Voice>();
    v->handle = va;
    v->rack = rack.get();
    uint32_t pa = va + 0x10;
    for (uint32_t mod : d.def->mods) {
      v->params.push_back(param_size(mod) ? pa : 0);
      write_defaults(c, pa, mod);
      pa += (param_size(mod) + 15) & ~15u;
    }
    size_t nm = d.def->mods.size();
    v->locked.assign(nm, false);
    v->bypassed.assign(nm, false);
    v->callbacks.assign(nm, {});
    v->out_patches.assign(d.def->outputs, std::vector<Patch*>(rack->patches_per_output, nullptr));
    v->in.assign(sys.granularity * 2, 0.0f);
    v->out.assign(sys.granularity * 2, 0.0f);
    v->pcm_out.assign(sys.granularity * 2, 0);
    reset_source(*v);
    voices[va] = v.get();
    rack->voices.push_back(std::move(v));
    va += voice_bytes(*d.def);
  }
  HLE_NOTE("sceNgsRackInit: %s x%d, %d ch -> 0x%08x\n", d.def->name, d.voices, d.channels, base);
  racks[base] = rack.get();
  sys.racks.push_back(std::move(rack));
  c.mem().write<uint32_t>(out, base);
  ret(c, NGS_OK);
}

void remove_patch(Patch* p) {
  for (auto& outs : p->src->out_patches)
    for (Patch*& s : outs)
      if (s == p) s = nullptr;
  auto& ins = p->dst->in_patches;
  ins.erase(std::remove(ins.begin(), ins.end(), p), ins.end());
  patches.erase(p->handle);
}

// sceNgsRackRelease(rack, callback): we release at once; a callback still runs (from here, not a later update).
void RackRelease(Cpu& c) {
  uint32_t cb = arg(c, 1), user = 0, handle = arg(c, 0);
  System* sys = nullptr;
  {
    std::lock_guard lk(ngs_lock);
    auto it = racks.find(handle);
    if (it == racks.end()) return ret(c, NGS_ERROR_INVALID_HANDLE);
    Rack* r = it->second;
    sys = r->sys;
    user = r->user_data;
    for (auto& v : r->voices) {
      std::vector<Patch*> ps(v->in_patches);
      for (auto& outs : v->out_patches)
        for (Patch* p : outs)
          if (p) ps.push_back(p);
      for (Patch* p : ps)
        if (patches.count(p->handle)) remove_patch(p);
      voices.erase(v->handle);
    }
    racks.erase(it);
    auto& list = sys->racks;
    list.erase(std::remove_if(list.begin(), list.end(), [&](auto& x) { return x.get() == r; }), list.end());
  }
  if (cb) {
    std::vector<PendingCallback> cbs{{cb, 0, handle, 0, 0, 0, 0, user}};
    run_callbacks(c, *sys, cbs);
  }
  ret(c, NGS_OK);
}

void RackGetVoiceHandle(Cpu& c) {
  std::lock_guard lk(ngs_lock);
  auto it = racks.find(arg(c, 0));
  if (it == racks.end()) return ret(c, NGS_ERROR_INVALID_HANDLE);
  uint32_t i = arg(c, 1);
  if (i >= it->second->voices.size() || !arg(c, 2)) return ret(c, NGS_ERROR_INVALID_PARAM);
  c.mem().write<uint32_t>(arg(c, 2), it->second->voices[i]->handle);
  ret(c, NGS_OK);
}

// --- patches -----------------------------------------------------------------------------------------------------
// sceNgsPatchCreateRouting(SceNgsPatchSetupInfo* {src, out, sub, dst, in}, patch*)
void PatchCreateRouting(Cpu& c) {
  uint32_t info = arg(c, 0), out = arg(c, 1);
  if (!info || !out) return ret(c, NGS_ERROR_INVALID_PARAM);
  Memory& m = c.mem();
  std::lock_guard lk(ngs_lock);
  Voice *src = find_voice(m.read<uint32_t>(info)), *dst = find_voice(m.read<uint32_t>(info + 12));
  int32_t oi = int32_t(m.read<uint32_t>(info + 4)), sub = int32_t(m.read<uint32_t>(info + 8));
  int32_t ii = int32_t(m.read<uint32_t>(info + 16));
  if (!src || !dst) return ret(c, NGS_ERROR_INVALID_HANDLE);
  if (src->rack->sys != dst->rack->sys || !dst->rack->def->buss || ii != 0 || oi < 0 ||
      oi >= int32_t(src->out_patches.size()))
    return ret(c, NGS_ERROR_INVALID_PARAM);
  auto& slots = src->out_patches[oi];
  if (sub < 0) sub = int32_t(std::find(slots.begin(), slots.end(), nullptr) - slots.begin());
  if (sub >= int32_t(slots.size()) || slots[sub]) return ret(c, NGS_ERROR_PATCH_NOT_AVAIL);
  auto p = std::make_unique<Patch>(Patch{next_patch, src, oi, sub, dst, ii, {{0, 0}, {0, 0}}});
  next_patch += 2;
  slots[sub] = p.get();
  dst->in_patches.push_back(p.get());
  m.write<uint32_t>(out, p->handle);
  patches[p->handle] = std::move(p);
  ret(c, NGS_OK);
}

void PatchRemoveRouting(Cpu& c) {
  std::lock_guard lk(ngs_lock);
  auto it = patches.find(arg(c, 0));
  if (it == patches.end()) return ret(c, NGS_ERROR_INVALID_HANDLE);
  remove_patch(it->second.get());
  ret(c, NGS_OK);
}

// sceNgsPatchGetInfo(patch, SceNgsPatchRouteInfo* {out ch, in ch, matrix}, SceNgsPatchSetupInfo* setup|NULL)
void PatchGetInfo(Cpu& c) {
  std::lock_guard lk(ngs_lock);
  auto it = patches.find(arg(c, 0));
  if (it == patches.end()) return ret(c, NGS_ERROR_INVALID_HANDLE);
  Patch& p = *it->second;
  Memory& m = c.mem();
  if (uint32_t ri = arg(c, 1)) {
    m.write<uint32_t>(ri, p.src->rack->channels);
    m.write<uint32_t>(ri + 4, p.dst->rack->channels);
    for (int o = 0; o < 2; o++)
      for (int i = 0; i < 2; i++) m.write<float>(ri + 8 + 8 * o + 4 * i, p.m[o][i]);
  }
  if (uint32_t si = arg(c, 2)) {
    m.write<uint32_t>(si, p.src->handle);
    m.write<uint32_t>(si + 4, uint32_t(p.out_index));
    m.write<uint32_t>(si + 8, uint32_t(p.sub_index));
    m.write<uint32_t>(si + 12, p.dst->handle);
    m.write<uint32_t>(si + 16, uint32_t(p.in_index));
  }
  ret(c, NGS_OK);
}

void PatchSetVolumesMatrix(Cpu& c) {
  std::lock_guard lk(ngs_lock);
  auto it = patches.find(arg(c, 0));
  if (it == patches.end()) return ret(c, NGS_ERROR_INVALID_HANDLE);
  uint32_t mx = arg(c, 1);
  if (!mx) return ret(c, NGS_ERROR_INVALID_PARAM);
  for (int o = 0; o < 2; o++)
    for (int i = 0; i < 2; i++) it->second->m[o][i] = c.mem().read<float>(mx + 8 * o + 4 * i);
  ret(c, NGS_OK);
}

// --- voices ------------------------------------------------------------------------------------------------------
#define VOICE_OR_FAIL(v)                                   \
  std::lock_guard lk(ngs_lock);                            \
  Voice* v = find_voice(arg(c, 0));                        \
  if (!v) return ret(c, NGS_ERROR_INVALID_HANDLE)

void VoicePlay(Cpu& c) {
  VOICE_OR_FAIL(v);
  if (v->state != STATE_AVAILABLE) return ret(c, NGS_ERROR_INVALID_STATE);
  reset_source(*v);
  uint32_t mod0 = v->rack->def->mods[0];
  if (mod0 == MOD_PLAYER) {  // start_buffer / start_bytes
    int8_t sb = c.mem().read<int8_t>(v->params[0] + 0x51);
    v->src.buffer = sb >= 0 && sb < 4 ? sb : 0;
    v->src.pos = c.mem().read<uint32_t>(v->params[0] + 0x48);
  }
  v->state = STATE_ACTIVE;
  v->update_passed = 0;
  v->release_left = -1;
  ret(c, NGS_OK);
}

void VoiceKeyOff(Cpu& c) {
  VOICE_OR_FAIL(v);
  if (!(v->state & STATE_ACTIVE)) return ret(c, NGS_ERROR_INVALID_STATE);
  if (v->state & STATE_KEY_OFF) return ret(c, NGS_OK);  // already releasing
  uint32_t release_ms = 0;
  const Def* d = v->rack->def;
  for (size_t i = 0; i < d->mods.size(); i++)
    if (d->mods[i] == MOD_ENVELOPE && !v->bypassed[i]) release_ms = c.mem().read<uint32_t>(v->params[i] + 0x38);
  // The voice stays ACTIVE while the release runs: the game reuses a stopped voice only once (state & 7) == 0
  // (status poll 0x8145a868), and rewrites its parameter blocks at that point.
  v->state |= STATE_KEY_OFF;
  v->release_left = int64_t(release_ms) * v->rack->sys->sample_rate / 1000;
  if (v->release_left <= 0) v->state = STATE_AVAILABLE;
  ret(c, NGS_OK);
}

void VoiceKill(Cpu& c) {
  VOICE_OR_FAIL(v);
  v->state = STATE_AVAILABLE;
  reset_source(*v);
  ret(c, NGS_OK);
}

void VoicePause(Cpu& c) {
  VOICE_OR_FAIL(v);
  if (!(v->state & (STATE_ACTIVE | STATE_KEY_OFF))) return ret(c, NGS_ERROR_INVALID_STATE);
  v->state |= STATE_PAUSED;
  ret(c, NGS_OK);
}

void VoiceResume(Cpu& c) {
  VOICE_OR_FAIL(v);
  v->state &= ~STATE_PAUSED;
  ret(c, NGS_OK);
}

// sceNgsVoiceLockParams(voice, module, params_id, SceNgsBufferInfo* out)
void VoiceLockParams(Cpu& c) {
  VOICE_OR_FAIL(v);
  uint32_t mi = arg(c, 1), id = arg(c, 2), out = arg(c, 3);
  const Def* d = v->rack->def;
  if (mi >= d->mods.size() || !out) return ret(c, NGS_ERROR_INVALID_PARAM);
  if ((id & 0xffff) != (d->mods[mi] & 0xffff) || !v->params[mi]) {
    HLE_NOTE("sceNgsVoiceLockParams(%s module %u = 0x%04x): asked for id 0x%08x\n", d->name, mi, d->mods[mi], id);
    return ret(c, NGS_ERROR_PARAM_TYPE_MISMATCH);
  }
  if (v->locked[mi]) return ret(c, NGS_ERROR_RESOURCE_LOCKED);
  v->locked[mi] = true;
  c.mem().write<uint32_t>(out, v->params[mi]);
  c.mem().write<uint32_t>(out + 4, param_size(d->mods[mi]));
  ret(c, NGS_OK);
}

void VoiceUnlockParams(Cpu& c) {
  VOICE_OR_FAIL(v);
  uint32_t mi = arg(c, 1);
  if (mi >= v->locked.size()) return ret(c, NGS_ERROR_INVALID_PARAM);
  if (!v->locked[mi]) return ret(c, NGS_ERROR_INVALID_STATE);
  v->locked[mi] = false;
  ret(c, NGS_OK);
}

// sceNgsVoiceBypassModule(voice, module, flag): 0 = not bypassed, 2 = bypassed.
void VoiceBypassModule(Cpu& c) {
  VOICE_OR_FAIL(v);
  uint32_t mi = arg(c, 1), flag = arg(c, 2);
  if (mi >= v->bypassed.size() || (flag != 0 && flag != 2)) return ret(c, NGS_ERROR_INVALID_PARAM);
  v->bypassed[mi] = flag == 2;
  ret(c, NGS_OK);
}

void VoiceSetModuleCallback(Cpu& c) {
  VOICE_OR_FAIL(v);
  uint32_t mi = arg(c, 1);
  if (mi >= v->callbacks.size()) return ret(c, NGS_ERROR_INVALID_PARAM);
  v->callbacks[mi] = {arg(c, 2), arg(c, 3)};
  ret(c, NGS_OK);
}

// SceNgsVoiceInfo: state, modules, inputs, outputs, patches per output, update_passed.
void VoiceGetInfo(Cpu& c) {
  VOICE_OR_FAIL(v);
  uint32_t o = arg(c, 1);
  if (!o) return ret(c, NGS_ERROR_INVALID_PARAM);
  const Def* d = v->rack->def;
  uint32_t vals[6] = {v->state, uint32_t(d->mods.size()), d->buss ? 1u : 0u, d->outputs, v->rack->patches_per_output,
                      v->update_passed};
  for (int i = 0; i < 6; i++) c.mem().write<uint32_t>(o + 4 * i, vals[i]);
  ret(c, NGS_OK);
}

// sceNgsVoiceGetStateData(voice, module, mem, size): master output = the grain (s16 stereo); players/ATRAC9 =
// {byte position, buffer, samples since key-on, bytes since key-on, samples total, bytes total}.
void VoiceGetStateData(Cpu& c) {
  VOICE_OR_FAIL(v);
  uint32_t mi = arg(c, 1), dst = arg(c, 2), size = arg(c, 3);
  const Def* d = v->rack->def;
  if (mi >= d->mods.size() || !c.mem().valid(dst, size)) return ret(c, NGS_ERROR_INVALID_PARAM);
  uint32_t mod = d->mods[mi];
  uint8_t* p = c.mem().ptr(dst);
  if (mod == MOD_OUTPUT) {
    uint32_t bytes = uint32_t(v->pcm_out.size() * 2);
    if (size != bytes) return ret(c, NGS_ERROR_SIZE_MISMATCH);
    std::memcpy(p, v->pcm_out.data(), bytes);
    return ret(c, NGS_OK);
  }
  uint32_t fixed = fixed_state_size(mod);
  if (fixed && size != fixed) return ret(c, NGS_ERROR_SIZE_MISMATCH);
  std::memset(p, 0, size);
  if (mod == MOD_PLAYER || mod == MOD_AT9) {
    const Source& s = v->src;
    int32_t vals[6] = {int32_t(s.pos), s.buffer, s.samples_since_key_on, s.bytes_since_key_on, s.samples_total,
                       s.bytes_total};
    std::memcpy(p, vals, sizeof vals);
  }
  ret(c, NGS_OK);
}

void VoiceGetParamsOutOfRange(Cpu& c) {
  VOICE_OR_FAIL(v);
  uint32_t buf = arg(c, 2);
  if (!buf || !c.mem().valid(buf, 128)) return ret(c, NGS_ERROR_INVALID_PARAM);
  static const char msg[] = "No value was out of range\n";
  std::memcpy(c.mem().ptr(buf), msg, sizeof msg);
  ret(c, NGS_OK);
}


// --- definitions -------------------------------------------------------------------------------------------------
template <size_t I>
void DefGet(Cpu& c) {
  std::lock_guard lk(ngs_lock);
  if (!def_tags) def_tags = c.mem().alloc(0x1000, "NGS voice definitions");
  ret(c, def_tags + uint32_t(I) * 16);
}

}  // namespace

void register_ngs() {
  REG(sceNgsSystemGetRequiredMemorySize, SystemGetRequiredMemorySize);
  REG(sceNgsSystemInit, SystemInit);
  REG(sceNgsSystemRelease, SystemRelease);
  REG(sceNgsSystemUpdate, SystemUpdate);
  REG(sceNgsRackGetRequiredMemorySize, RackGetRequiredMemorySize);
  REG(sceNgsRackInit, RackInit);
  REG(sceNgsRackRelease, RackRelease);
  REG(sceNgsRackGetVoiceHandle, RackGetVoiceHandle);
  REG(sceNgsPatchCreateRouting, PatchCreateRouting);
  REG(sceNgsPatchRemoveRouting, PatchRemoveRouting);
  REG(sceNgsPatchGetInfo, PatchGetInfo);
  REG(sceNgsVoicePatchSetVolumesMatrix, PatchSetVolumesMatrix);
  REG(sceNgsVoicePlay, VoicePlay);
  REG(sceNgsVoiceKeyOff, VoiceKeyOff);
  REG(sceNgsVoiceKill, VoiceKill);
  REG(sceNgsVoicePause, VoicePause);
  REG(sceNgsVoiceResume, VoiceResume);
  REG(sceNgsVoiceLockParams, VoiceLockParams);
  REG(sceNgsVoiceUnlockParams, VoiceUnlockParams);
  REG(sceNgsVoiceBypassModule, VoiceBypassModule);
  REG(sceNgsVoiceSetModuleCallback, VoiceSetModuleCallback);
  REG(sceNgsVoiceGetInfo, VoiceGetInfo);
  REG(sceNgsVoiceGetStateData, VoiceGetStateData);
  REG(sceNgsVoiceGetParamsOutOfRange, VoiceGetParamsOutOfRange);
  REG(sceNgsVoiceDefGetTemplate1, DefGet<0>);
  REG(sceNgsVoiceDefGetAtrac9Voice, DefGet<1>);
  REG(sceNgsVoiceDefGetMasterBuss, DefGet<2>);
  REG(sceNgsVoiceDefGetReverbBuss, DefGet<3>);
  REG(sceNgsVoiceDefGetDelayBuss, DefGet<4>);
  REG(sceNgsVoiceDefGetDistortionBuss, DefGet<5>);
  REG(sceNgsVoiceDefGetCompressorBuss, DefGet<6>);
  REG(sceNgsVoiceDefGetEqBuss, DefGet<7>);
  REG(sceNgsVoiceDefGetMixerBuss, DefGet<8>);
  REG(sceNgsVoiceDefGetPitchShiftBuss, DefGet<9>);
}

}  // namespace hle
