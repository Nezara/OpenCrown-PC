// dcp_run's SDL3 audio sink: mixes every open AudioOut port down to float stereo 48 kHz into one SDL audio stream.
// submit() blocks while more than the queue target is queued, which is what paces CRI's mixer thread.
#include "sdl_audio.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include "hle/audio.h"

namespace {

constexpr int kQueueBatches = 4;  // ~21 ms at 256 frames: latency vs. underrun margin
// The device pulls whole buffers of its own size from the stream. With a target of only 4 batches (1024 frames) a pull
// can find less than one buffer queued when the device buffer is large (Bluetooth / USB headsets; PulseAudio's 768
// frames underran ~10x/s in the Linux build), and SDL pads the rest with silence (crackle). So the target is at least
// kDeviceBuffers device buffers.
constexpr int kDeviceBuffers = 3;

class SdlSink final : public dcp::hle::AudioSink {
public:
    SdlSink(SDL_AudioStream* s, int target_frames) : s_(s), target_(target_frames * 8) {}
    void open(int port, int channels, int) override { ch_[port] = channels; }
    void close(int port) override { ch_[port] = 0; }
    void submit(int count, const int* ports, const float* const* data, int frames) override {
        mix_.assign(size_t(frames) * 2, 0.0f);
        for (int i = 0; i < count; ++i) {
            if (!data[i]) continue;
            int ch = ch_[ports[i]];
            for (int f = 0; f < frames; ++f) {
                const float* x = data[i] + size_t(f) * ch;
                float l = x[0], r = ch >= 2 ? x[1] : x[0];
                if (ch == 8) {  // FL FR C LFE SL SR BL BR -> stereo
                    l += 0.707f * x[2] + 0.707f * x[4] + 0.5f * x[6];
                    r += 0.707f * x[2] + 0.707f * x[5] + 0.5f * x[7];
                }
                mix_[f * 2] += l;
                mix_[f * 2 + 1] += r;
            }
        }
        for (float& v : mix_) v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
        if (tone_)  // DCP_AUDIO_TONE=1: a 440 Hz sine instead of the game mix, same pacing and SDL path (output check)
            for (int f = 0; f < frames; ++f, ++tone_pos_)
                mix_[f * 2] = mix_[f * 2 + 1] = 0.25f * std::sin(6.2831853f * 440.0f * float(tone_pos_ % 48000) / 48000.0f);
        int bytes = int(mix_.size() * sizeof(float));
        int queued = SDL_GetAudioStreamQueued(s_);
        uint64_t t0 = SDL_GetTicksNS();
        while (queued > target_) SDL_Delay(1), queued = SDL_GetAudioStreamQueued(s_);
        SDL_PutAudioStreamData(s_, mix_.data(), bytes);
        if (log_) stat(queued, bytes, SDL_GetTicksNS() - t0);
    }

private:
    // DCP_AUDIO_LOG=1: once a second, batches put, queue depth before each put (min / max, bytes), puts that found
    // the queue empty (the device had already drained it: an underrun unless at start-up) and time spent waiting.
    void stat(int queued, int bytes, uint64_t wait_ns) {
        uint64_t now = SDL_GetTicksNS();
        if (!n_) t0_ = now, qmin_ = queued, qmax_ = queued;
        ++n_, empty_ += queued == 0, wait_ += wait_ns;
        qmin_ = queued < qmin_ ? queued : qmin_, qmax_ = queued > qmax_ ? queued : qmax_;
        if (now - t0_ >= 1000000000ull) {
            std::fprintf(stderr, "audio: %d batches (%d frames) in %.3f s, queued %d..%d B, empty %d, wait %.1f ms\n", n_,
                         n_ * bytes / 8, (now - t0_) / 1e9, qmin_, qmax_, empty_, wait_ / 1e6);
            n_ = empty_ = 0, wait_ = 0;
        }
    }

    SDL_AudioStream* s_;
    int target_;  // bytes (f32 stereo)
    bool log_ = std::getenv("DCP_AUDIO_LOG") != nullptr;
    bool tone_ = std::getenv("DCP_AUDIO_TONE") != nullptr;
    uint64_t tone_pos_ = 0;
    int n_ = 0, empty_ = 0, qmin_ = 0, qmax_ = 0;
    uint64_t t0_ = 0, wait_ = 0;
    int ch_[8] = {};
    std::vector<float> mix_;
};

// Windows lists audio sessions in the volume mixer by display name, falling back to the exe name ("dcp_run"), so the
// game was hard to find there. SDL's WASAPI stream uses the process's default session on the default endpoint: name
// that one. Best effort; the session keeps working without it.
void name_audio_session() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninit = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE: COM already up on this thread in another mode, still usable
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioSessionManager* mgr = nullptr;
    IAudioSessionControl* ctl = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) &&
        SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr))) &&
        SUCCEEDED(mgr->GetAudioSessionControl(nullptr, 0, &ctl)))
        ok = SUCCEEDED(ctl->SetDisplayName(L"Dragon's Crown Pro", nullptr));
    if (ctl) ctl->Release();
    if (mgr) mgr->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    if (uninit) CoUninitialize();
    std::fprintf(stderr, "audio: volume mixer name %s\n", ok ? "set" : "not set");
}

}  // namespace

bool install_sdl_audio() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "audio: SDL_InitSubSystem: %s (falling back to silent paced output)\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
    // DCP_AUDIO_DEVICE=<name substring>: open that playback device instead of the default (tests per device).
    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (const char* want = std::getenv("DCP_AUDIO_DEVICE"); want && *want) {
        int n = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
        for (int i = 0; ids && i < n; ++i)
            if (const char* name = SDL_GetAudioDeviceName(ids[i]); name && std::strstr(name, want)) { id = ids[i]; break; }
        SDL_free(ids);
        if (id == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) std::fprintf(stderr, "audio: no device matches '%s', using the default\n", want);
    }
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(id, &spec, nullptr, nullptr);
    if (!s) {
        std::fprintf(stderr, "audio: open: %s (falling back to silent paced output)\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(s);
    name_audio_session();
    SDL_AudioSpec dev{};
    int dev_frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(s), &dev, &dev_frames))
        std::fprintf(stderr, "audio: device %s, %d Hz, %d ch, format 0x%x, %d sample frames per buffer\n",
                     SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(s)), dev.freq, dev.channels, unsigned(dev.format),
                     dev_frames);
    int target = kQueueBatches * 256;
    if (dev_frames * kDeviceBuffers > target) target = dev_frames * kDeviceBuffers;
    if (const char* e = std::getenv("DCP_AUDIO_QUEUE"); e && std::atoi(e) > 0) target = std::atoi(e);  // frames, for tests
    dcp::hle::set_audio_sink(std::make_unique<SdlSink>(s, target));
    std::fprintf(stderr, "audio: SDL3 %s, f32 stereo 48 kHz, queue target %d frames\n", SDL_GetCurrentAudioDriver(), target);
    return true;
}
