// at9_test: decodes RIFF ATRAC9 (.at9) files with our decoder.
//
//   at9_test <file.at9> [out.pcm] [reference.pcm]
// Writes raw s16 interleaved PCM; with a reference decode (e.g. FFmpeg's, `-f s16le`) it reports the alignment
// offset and the signal-to-error ratio over the overlapping samples. Polarity is matched too: FFmpeg 9's ATRAC9
// decoder outputs the inverted signal (checked against LibAtrac9, which matches ours to +-1 LSB).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "codec/atrac9.h"

namespace {

std::vector<uint8_t> load(const char* path) {
  std::vector<uint8_t> d;
  if (std::FILE* f = std::fopen(path, "rb")) {
    std::fseek(f, 0, SEEK_END);
    d.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
    std::fclose(f);
  }
  return d;
}

uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: at9_test <file.at9> [out.pcm] [reference.pcm]\n");
    return 2;
  }
  std::vector<uint8_t> f = load(argv[1]);
  if (f.size() < 12 || std::memcmp(f.data(), "RIFF", 4) || std::memcmp(f.data() + 8, "WAVE", 4)) {
    std::fprintf(stderr, "not a RIFF WAVE file\n");
    return 1;
  }
  const uint8_t* cfg = nullptr;
  const uint8_t* data = nullptr;
  size_t data_size = 0;
  for (size_t p = 12; p + 8 <= f.size();) {
    uint32_t size = u32(&f[p + 4]);
    if (!std::memcmp(&f[p], "fmt ", 4) && size >= 0x34) cfg = &f[p + 8 + 0x2c];
    if (!std::memcmp(&f[p], "data", 4)) {
      data = &f[p + 8];
      data_size = std::min<size_t>(size, f.size() - p - 8);
    }
    p += 8 + size + (size & 1);
  }
  codec::Atrac9 dec;
  if (!cfg || !data || !dec.init(cfg)) {
    std::fprintf(stderr, "no ATRAC9 config / data chunk\n");
    return 1;
  }
  uint32_t ch = dec.channels(), sfb = dec.superframe_bytes(), sfs = dec.superframe_samples();
  std::vector<int16_t> pcm;
  std::vector<float> buf(size_t(sfs) * ch);
  size_t errors = 0, superframes = data_size / sfb;
  for (size_t i = 0; i < superframes; i++) {
    if (!dec.decode_superframe(data + i * sfb, buf.data())) errors++;
    for (float v : buf) pcm.push_back(int16_t(std::clamp(std::lround(v), -32768L, 32767L)));
  }
  std::printf("%s: %u ch, %u Hz, superframe %u bytes / %u samples, %zu superframes, %zu errors\n", argv[1], ch,
              dec.sample_rate(), sfb, sfs, superframes, errors);
  if (argc > 2) {
    if (std::FILE* o = std::fopen(argv[2], "wb")) {
      std::fwrite(pcm.data(), 2, pcm.size(), o);
      std::fclose(o);
    }
  }
  if (argc > 3) {
    std::vector<uint8_t> r = load(argv[3]);
    const int16_t* ref = reinterpret_cast<const int16_t*>(r.data());
    size_t nref = r.size() / 2 / ch, nour = pcm.size() / ch;
    // Best frame offset (the reference may drop the encoder delay at the start).
    long best = 0;
    int sign = 1;
    double best_err = 1e300;
    for (int sg : {1, -1})
      for (long off = 0; off <= 4096 && size_t(off) < nour; off++) {
        double e = 0;
        size_t n = std::min<size_t>(std::min(nref, nour - size_t(off)), 8192);
        for (size_t i = 0; i < n * ch; i++) {
          double d = double(pcm[size_t(off) * ch + i]) - sg * ref[i];
          e += d * d;
        }
        if (n && e < best_err) best_err = e, best = off, sign = sg;
      }
    double sig = 0, err = 0, maxd = 0;
    size_t n = std::min(nref, nour - size_t(best));
    for (size_t i = 0; i < n * ch; i++) {
      double d = double(pcm[size_t(best) * ch + i]) - sign * ref[i];
      sig += double(ref[i]) * ref[i];
      err += d * d;
      maxd = std::max(maxd, std::fabs(d));
    }
    std::printf("  vs reference: offset %ld samples%s, %zu compared, SNR %.1f dB, max |diff| %.0f\n", best,
                sign < 0 ? " (inverted polarity)" : "", n,
                err > 0 ? 10 * std::log10(sig / err) : 999.0, maxd);
  }
  return errors ? 3 : 0;
}
