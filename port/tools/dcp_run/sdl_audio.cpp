// dcp_run's SDL3 audio sink: mixes every open AudioOut port down to float stereo 48 kHz into one SDL audio stream.
// submit() blocks while more than kQueueBatches mixer passes are queued, which is what paces CRI's mixer thread.
#include "sdl_audio.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <vector>

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include "hle/audio.h"

namespace {

constexpr int kQueueBatches = 4;  // ~21 ms at 256 frames: latency vs. underrun margin

class SdlSink final : public dcp::hle::AudioSink {
public:
    explicit SdlSink(SDL_AudioStream* s) : s_(s) {}
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
        int bytes = int(mix_.size() * sizeof(float));
        while (SDL_GetAudioStreamQueued(s_) > bytes * kQueueBatches) SDL_Delay(1);
        SDL_PutAudioStreamData(s_, mix_.data(), bytes);
    }

private:
    SDL_AudioStream* s_;
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
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s) {
        std::fprintf(stderr, "audio: open: %s (falling back to silent paced output)\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(s);
    name_audio_session();
    dcp::hle::set_audio_sink(std::make_unique<SdlSink>(s));
    std::fprintf(stderr, "audio: SDL3 %s, f32 stereo 48 kHz\n", SDL_GetCurrentAudioDriver());
    return true;
}
