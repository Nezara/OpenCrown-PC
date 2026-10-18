// libSceAudioOut: up to 8 ports (handles 1..8), forwarded to an AudioSink. Formats seen in the eboot (CRI NcVoice,
// FUN_005f3be0): FLOAT_MONO 3 / FLOAT_STEREO 4 / FLOAT_8CH 5, 256 frames, 48 kHz; S16 formats 0..2 are converted.
// sceAudioOutOutputs takes {int32 handle; pad; const void* ptr} entries of 16 B. Output with a null ptr = flush.
#include <cstring>
#include <mutex>
#include <vector>


#include "audio.h"
#include "common.h"

namespace dcp::hle {
namespace {

constexpr int32_t kErrNotInit = int32_t(0x80260001);
constexpr int32_t kErrParam = int32_t(0x80260002);
constexpr int32_t kErrPort = int32_t(0x80260004);
constexpr int32_t kErrPortFull = int32_t(0x8026000a);
constexpr int kMaxPorts = 8;

struct Port {
    bool used = false;
    int channels = 0;
    bool is_float = false;
    uint32_t frames = 0;
    std::vector<float> buf;
};

// Paced null sink: sleeps until frames/rate of wall time has passed since the previous batch. DCP_AUDIO_WAV=path
// mixes every port down to 16-bit stereo 48 kHz and writes it there (header patched on every batch, so it survives kill).
class PacedSink final : public AudioSink {
public:
    PacedSink() {
        if (const char* p = std::getenv("DCP_AUDIO_WAV"); p && *p) {
            wav_ = std::fopen(p, "wb");
            if (wav_) { uint8_t h[44] = {}; std::fwrite(h, 1, 44, wav_); }
        }
    }
    void open(int port, int channels, int) override { ch_[port] = channels; }
    void close(int) override {}
    void submit(int count, const int* ports, const float* const* data, int frames) override {
        if (wav_) write_wav(count, ports, data, frames);
        int64_t now = int64_t(host_ns());
        int64_t step = int64_t(1000000000ll) * frames / 48000;
        if (next_ == 0 || now - next_ > 100000000ll) next_ = now;  // (re)sync after stalls (> 0.1 s behind)
        next_ += step;
        if (next_ > now) host_sleep_ns(uint64_t(next_ - now));
    }
private:
    void write_wav(int count, const int* ports, const float* const* data, int frames) {
        std::vector<float> mix(size_t(frames) * 2, 0.0f);
        for (int i = 0; i < count; ++i) {
            if (!data[i]) continue;
            int ch = ch_[ports[i]];
            for (int f = 0; f < frames; ++f) {
                const float* s = data[i] + size_t(f) * ch;
                mix[f * 2] += s[0];
                mix[f * 2 + 1] += ch >= 2 ? s[1] : s[0];
            }
        }
        std::vector<int16_t> out(mix.size());
        for (size_t i = 0; i < mix.size(); ++i) {
            float v = mix[i] < -1.0f ? -1.0f : mix[i] > 1.0f ? 1.0f : mix[i];
            out[i] = int16_t(v * 32767.0f);
        }
        std::fwrite(out.data(), 2, out.size(), wav_);
        bytes_ += uint32_t(out.size() * 2);
        long pos = std::ftell(wav_);
        uint8_t h[44];
        auto put32 = [&](int o, uint32_t v) { std::memcpy(h + o, &v, 4); };
        auto put16 = [&](int o, uint16_t v) { std::memcpy(h + o, &v, 2); };
        std::memcpy(h, "RIFF", 4); put32(4, 36 + bytes_); std::memcpy(h + 8, "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 2); put32(24, 48000); put32(28, 48000 * 4); put16(32, 4); put16(34, 16);
        std::memcpy(h + 36, "data", 4); put32(40, bytes_);
        std::fseek(wav_, 0, SEEK_SET);
        std::fwrite(h, 1, 44, wav_);
        std::fseek(wav_, pos, SEEK_SET);
        std::fflush(wav_);
    }

    int ch_[kMaxPorts] = {};
    int64_t next_ = 0;  // host_ns of the end of the last submitted batch
    std::FILE* wav_ = nullptr;
    uint32_t bytes_ = 0;
};

std::mutex g_mu;
std::unique_ptr<AudioSink> g_sink;
bool g_init = false;
Port g_ports[kMaxPorts];

AudioSink& sink() {
    if (!g_sink) g_sink = std::make_unique<PacedSink>();
    return *g_sink;
}

// Port data -> interleaved float (S16 formats converted in the port's scratch buffer).
const float* to_float(Port& p, const void* ptr) {
    if (!ptr) return nullptr;
    if (p.is_float) return static_cast<const float*>(ptr);
    size_t n = size_t(p.frames) * p.channels;
    p.buf.resize(n);
    const int16_t* s = static_cast<const int16_t*>(ptr);
    for (size_t i = 0; i < n; ++i) p.buf[i] = s[i] * (1.0f / 32768.0f);
    return p.buf.data();
}

DCP_SYSV int32_t sceAudioOutInit() {
    DCP_HLE_LOG("sceAudioOutInit", "ok");
    g_init = true;
    return kOk;
}

DCP_SYSV int32_t sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                                 uint32_t param) {
    if (!g_init) return kErrNotInit;
    uint32_t fmt = param & 0xff;
    static const int kCh[] = {1, 2, 8, 1, 2, 8, 8, 8};
    if (fmt > 7 || len == 0 || freq != 48000) {
        std::fprintf(stderr, "HLE sceAudioOutOpen: bad len %u freq %u param 0x%x\n", len, freq, param);
        return kErrParam;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < kMaxPorts; ++i) {
        Port& p = g_ports[i];
        if (p.used) continue;
        p.used = true;
        p.channels = kCh[fmt];
        p.is_float = fmt >= 3 && fmt != 6;  // 6 = S16_8CH_STD, 7 = FLOAT_8CH_STD
        p.frames = len;
        sink().open(i, p.channels, int(freq));
        std::fprintf(stderr, "HLE sceAudioOutOpen: user %d type %d index %d len %u fmt %u -> handle %d\n", user, type,
                     index, len, fmt, i + 1);
        return i + 1;
    }
    return kErrPortFull;
}

DCP_SYSV int32_t sceAudioOutClose(int32_t h) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (h < 1 || h > kMaxPorts || !g_ports[h - 1].used) return kErrPort;
    sink().close(h - 1);
    g_ports[h - 1] = Port{};
    return kOk;
}

struct OutputParam {
    int32_t handle;
    int32_t pad;
    const void* ptr;
};
static_assert(sizeof(OutputParam) == 16);

DCP_SYSV int32_t sceAudioOutOutputs(const OutputParam* param, uint32_t num) {
    if (!param || num == 0 || num > kMaxPorts) return kErrParam;
    int ports[kMaxPorts];
    const float* data[kMaxPorts];
    uint32_t frames = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (uint32_t i = 0; i < num; ++i) {
            int32_t h = param[i].handle;
            if (h < 1 || h > kMaxPorts || !g_ports[h - 1].used) return kErrPort;
            Port& p = g_ports[h - 1];
            ports[i] = h - 1;
            data[i] = to_float(p, param[i].ptr);
            frames = p.frames;
        }
    }
    // Submitted outside the lock: the sink blocks for pacing.
    sink().submit(int(num), ports, data, int(frames));
    return kOk;
}

DCP_SYSV int32_t sceAudioOutOutput(int32_t h, const void* ptr) {
    if (!ptr) return kOk;  // flush before Close (CRI voice teardown): nothing is queued host-side worth waiting on
    OutputParam p{h, 0, ptr};
    return sceAudioOutOutputs(&p, 1);
}

}  // namespace

void set_audio_sink(std::unique_ptr<AudioSink> s) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_sink = std::move(s);
    for (int i = 0; i < kMaxPorts; ++i)
        if (g_ports[i].used) g_sink->open(i, g_ports[i].channels, 48000);
}

void register_audio() {
    const char* l = "libSceAudioOut";
    REG(l, sceAudioOutInit); REG(l, sceAudioOutOpen); REG(l, sceAudioOutClose); REG(l, sceAudioOutOutput);
    REG(l, sceAudioOutOutputs);
}

}  // namespace dcp::hle
