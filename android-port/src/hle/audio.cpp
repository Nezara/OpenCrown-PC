// SceAudio output ports. sceAudioOutOutput blocks like the hardware does: each call returns once the previous
// grain would have played, which paces the game's audio thread in real time. Samples go to the host sink set by
// audio_set_sink (the platform layer's device, SDL3) and audio_set_wav (a 16-bit WAV file for checking the mix).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hle/audio.h"
#include "hle/common.h"

namespace hle {
namespace {

enum : uint32_t {
  AUDIO_OUT_ERROR_INVALID_PORT = 0x80260003,  // vitasdk psp2common/audioout.h
  AUDIO_OUT_ERROR_INVALID_POINTER = 0x80260004,
  AUDIO_OUT_ERROR_PORT_FULL = 0x80260005,
  AUDIO_OUT_ERROR_INVALID_SIZE = 0x80260006,
  AUDIO_OUT_ERROR_INVALID_FORMAT = 0x80260007,
  AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ = 0x80260008,
  AUDIO_OUT_ERROR_INVALID_PORT_TYPE = 0x8026000a,
};

struct Port {
  uint32_t type, len, freq, channels;
  int32_t vol[2] = {0x8000, 0x8000};
  std::chrono::steady_clock::time_point next{};
};
std::mutex audio_lock;
std::map<uint32_t, Port> ports;
uint32_t next_port = 0;

std::function<void(const int16_t*, uint32_t)> host_sink;
std::function<uint32_t()> host_queued;
std::string wav_path;
std::FILE* wav = nullptr;
uint32_t wav_bytes = 0;

void wav_header(std::FILE* f, uint32_t data_bytes) {
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fseek(f, 0, SEEK_SET);
  std::fwrite("RIFF", 1, 4, f);
  u32(36 + data_bytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16);
  u16(1);
  u16(2);
  u32(48000);
  u32(48000 * 4);
  u16(4);
  u16(16);
  std::fwrite("data", 1, 4, f);
  u32(data_bytes);
  std::fseek(f, 0, SEEK_END);
}

// 48 kHz grains go to the host device and the WAV sink as s16 stereo, with the port volume (0x8000 = unity) applied.
void sink(const Port& p, const int16_t* s) {
  if (p.freq != 48000 || (wav_path.empty() && !host_sink)) return;
  std::vector<int16_t> lr(size_t(p.len) * 2);
  for (uint32_t i = 0; i < p.len; i++)
    for (int ch = 0; ch < 2; ch++) {
      int32_t v = p.channels == 2 ? s[2 * i + ch] : s[i];
      lr[2 * i + ch] = int16_t(std::clamp<int32_t>(v * p.vol[ch] / 0x8000, -32768, 32767));
    }
  if (host_sink) host_sink(lr.data(), p.len);
  if (wav_path.empty()) return;
  if (!wav) {
    wav = std::fopen(wav_path.c_str(), "wb");
    if (!wav) return;
    wav_header(wav, 0);
  }
  std::fwrite(lr.data(), 2, lr.size(), wav);
  wav_bytes += p.len * 4;
  wav_header(wav, wav_bytes);  // keep the file valid if the run is cut short
}

// int sceAudioOutOpenPort(type, len, freq, mode): type 0 MAIN, 1 BGM, 2 VOICE; mode 0 mono, 1 stereo.
void OpenPort(Cpu& c) {
  uint32_t type = arg(c, 0), len = arg(c, 1), freq = arg(c, 2), mode = arg(c, 3);
  if (type > 2) return ret(c, AUDIO_OUT_ERROR_INVALID_PORT_TYPE);
  if (len < 64 || len > 65472 || len % 64) return ret(c, AUDIO_OUT_ERROR_INVALID_SIZE);
  if (mode > 1) return ret(c, AUDIO_OUT_ERROR_INVALID_FORMAT);
  if (freq != 48000 && type == 0) return ret(c, AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ);
  std::lock_guard lk(audio_lock);
  if (ports.size() >= 8) return ret(c, AUDIO_OUT_ERROR_PORT_FULL);
  uint32_t id = next_port++;
  ports[id] = {type, len, freq, mode ? 2u : 1u};
  HLE_NOTE("sceAudioOutOpenPort(type %u, %u frames, %u Hz, %s) -> %u\n", type, len, freq, mode ? "stereo" : "mono",
           id);
  ret(c, id);
}

void ReleasePort(Cpu& c) {
  std::lock_guard lk(audio_lock);
  ret(c, ports.erase(arg(c, 0)) ? 0u : uint32_t(AUDIO_OUT_ERROR_INVALID_PORT));
}

// int sceAudioOutOutput(port, buf): waits for the port's previous grain, then queues this one (NULL = drain).
void Output(Cpu& c) {
  uint32_t buf = arg(c, 1);
  Port p;
  std::chrono::steady_clock::time_point wake;
  {
    std::lock_guard lk(audio_lock);
    auto it = ports.find(arg(c, 0));
    if (it == ports.end()) return ret(c, AUDIO_OUT_ERROR_INVALID_PORT);
    Port& port = it->second;
    auto now = std::chrono::steady_clock::now();
    auto grain = std::chrono::nanoseconds(uint64_t(port.len) * 1000000000ull / port.freq);
    if (port.next < now - grain) port.next = now;  // first call or the game fell behind: don't burst
    wake = port.next;
    port.next += grain;
    p = port;
  }
  if (host_queued && p.freq == 48000) {
    // Device pacing: return once the device holds less than the target, so it always has ~40 ms in hand.
    constexpr uint32_t kTarget = 48000 / 25;
    for (int spins = 0; host_queued() > kTarget && spins < 200; spins++)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } else {
    std::this_thread::sleep_until(wake);
  }
  if (buf) {
    if (!c.mem().valid(buf, p.len * p.channels * 2)) return ret(c, AUDIO_OUT_ERROR_INVALID_POINTER);
    sink(p, reinterpret_cast<const int16_t*>(c.mem().ptr(buf)));
  }
  ret(c, 0);
}

// int sceAudioOutSetVolume(port, ch flags (1 = L, 2 = R), int vol[2]): 0..0x8000.
void SetVolume(Cpu& c) {
  uint32_t flags = arg(c, 1), vol = arg(c, 2);
  if (!vol) return ret(c, AUDIO_OUT_ERROR_INVALID_POINTER);
  std::lock_guard lk(audio_lock);
  auto it = ports.find(arg(c, 0));
  if (it == ports.end()) return ret(c, AUDIO_OUT_ERROR_INVALID_PORT);
  for (int ch = 0; ch < 2; ch++)
    if (flags & (1u << ch)) it->second.vol[ch] = std::clamp<int32_t>(int32_t(c.mem().read<uint32_t>(vol + 4 * ch)), 0, 0x8000);
  ret(c, 0);
}

// int sceAudioOutGetAdopt(type): every port type is available.
void GetAdopt(Cpu& c) { ret(c, 1); }

}  // namespace

void audio_set_wav(const std::string& path) { wav_path = path; }
void audio_set_sink(std::function<void(const int16_t*, uint32_t)> f, std::function<uint32_t()> queued) {
  host_sink = std::move(f);
  host_queued = std::move(queued);
}

void register_audio() {
  REG(sceAudioOutOpenPort, OpenPort);
  REG(sceAudioOutReleasePort, ReleasePort);
  REG(sceAudioOutOutput, Output);
  REG(sceAudioOutSetVolume, SetVolume);
  REG(sceAudioOutGetAdopt, GetAdopt);
}

}  // namespace hle
