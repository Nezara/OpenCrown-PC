// ATRAC9 decoder. The decoding procedure and its constant tables follow the format as documented by LibAtrac9
// (Copyright (c) 2018 Alex Barney, MIT License, https://github.com/Thealexbarney/LibAtrac9); the Huffman books are
// generated from it (tools/at9_huffman.py) and the band-extension tables below are its values. The code here is
// our own; derived tables (quantiser steps, scales, windows, gradient curves) are computed.
#include "codec/atrac9.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>

namespace codec {
namespace {

constexpr double PI = 3.14159265358979323846;

// --- format tables ---------------------------------------------------------------------------------------------
enum BlockType { MONO, STEREO, LFE };
struct ChannelLayout {
  int blocks;
  BlockType type[5];
};
const ChannelLayout LAYOUTS[6] = {{1, {MONO}},
                                  {2, {MONO, MONO}},
                                  {1, {STEREO}},
                                  {4, {STEREO, MONO, LFE, STEREO}},
                                  {5, {STEREO, MONO, LFE, STEREO, STEREO}},
                                  {2, {STEREO, STEREO}}};
const int SAMPLE_RATES[16] = {11025, 12000, 16000, 22050, 24000, 32000,  44100,  48000,
                              44100, 48000, 64000, 88200, 96000, 128000, 176400, 192000};
const uint8_t FRAME_POWER[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
const uint8_t MAX_BANDS[16] = {8, 8, 12, 12, 12, 18, 18, 18, 8, 8, 12, 12, 12, 16, 16, 16};
const uint8_t BAND_UNITS[19] = {0, 4, 8, 10, 12, 13, 14, 15, 16, 18, 20, 21, 22, 23, 24, 25, 26, 28, 30};
// Quantisation units: 30 units over 256 coefficients.
const int UNIT_START[31] = {0,  2,  4,  6,  8,  10, 12,  14,  16,  20,  24,  28,  32,  40,  48, 56,
                            64, 72, 80, 88, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256};
const uint8_t UNIT_BOOK[30] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
const uint8_t MIN_BANDS[2] = {3, 1}, MAX_EXT_BAND[2] = {18, 16}, MAX_HUFF_PRECISION[2] = {7, 1};
const uint8_t SF_WEIGHTS[8][32] = {
    {0, 0, 0, 1, 1, 2, 2, 2, 2, 2, 2, 3, 2, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 6, 6, 7, 7, 8, 10, 12, 12, 12},
    {3, 2, 2, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 2, 3, 3, 4, 5, 7, 10, 10, 10},
    {0, 2, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 8, 9, 12, 12, 12},
    {0, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 6, 7, 8, 8, 10, 11, 11, 12, 13, 13, 13, 13},
    {0, 2, 2, 3, 3, 4, 4, 5, 4, 5, 5, 5, 5, 6, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 11, 12, 12, 13, 13, 14, 14, 14},
    {1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 6, 7, 7, 9, 11, 11, 11},
    {0, 5, 8, 10, 11, 11, 12, 12, 12, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 13,
     15, 15, 15},
    {0, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 15, 15,
     15}};
const uint8_t GRADIENT_BASE[48] = {1,  1,  1,  1,  2,  2,  2,  2,  3,  3,  3,  4,  4,  5,  5,  6,
                                   7,  8,  9,  10, 11, 12, 13, 15, 16, 18, 19, 20, 21, 22, 23, 24,
                                   25, 26, 26, 27, 27, 28, 28, 28, 29, 29, 29, 29, 30, 30, 30, 30};

// Band extension: {group B unit, group C unit, band count} by quantisation-unit count - 13.
struct BexGroup {
  int b_unit, c_unit, bands;
};
const BexGroup BEX_GROUPS[8] = {{16, 21, 0}, {18, 22, 1}, {20, 22, 2}, {21, 22, 3},
                                {21, 22, 3}, {23, 24, 4}, {23, 24, 4}, {24, 24, 5}};
const int BEX_VALUE_COUNT[5][6] = {
    {0, 0, 0, 4, 4, 2}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 2, 2, 1}, {0, 0, 0, 2, 2, 2}, {1, 1, 1, 0, 0, 0}};
const int BEX_VALUE_BITS[5][6][4] = {
    {{0}, {0}, {0}, {5, 4, 3, 3}, {4, 4, 3, 4}, {4, 5}},
    {{0}, {0}, {0}, {0}, {0}, {0}},
    {{0}, {0}, {0}, {6, 6}, {6, 6}, {6}},
    {{0}, {0}, {0}, {4, 4}, {4, 4}, {4, 4}},
    {{3}, {3}, {3}, {0}, {0}, {0}}};
const double BEX_M0_B3[5][32] = {
    {0.000000e+0, 1.988220e-1, 2.514343e-1, 2.960510e-1, 3.263550e-1, 3.771362e-1, 3.786926e-1, 4.540405e-1,
     4.877625e-1, 5.262451e-1, 5.447083e-1, 5.737000e-1, 6.212158e-1, 6.222839e-1, 6.560974e-1, 6.896667e-1,
     7.555542e-1, 7.677917e-1, 7.918091e-1, 7.971497e-1, 8.188171e-1, 8.446045e-1, 9.790649e-1, 9.822083e-1,
     9.846191e-1, 9.859314e-1, 9.863586e-1, 9.863892e-1, 9.873352e-1, 9.881287e-1, 9.898682e-1, 9.913330e-1},
    {0.000000e+0, 9.982910e-1, 7.592773e-2, 7.179565e-1, 9.851379e-1, 5.340271e-1, 9.013672e-1, 6.349182e-1,
     7.226257e-1, 1.948547e-1, 7.628174e-1, 9.873657e-1, 8.112183e-1, 2.715454e-1, 9.734192e-1, 1.443787e-1,
     4.640198e-1, 3.249207e-1, 3.790894e-1, 8.276367e-2, 5.954590e-1, 2.864380e-1, 9.806824e-1, 7.929077e-1,
     6.292114e-1, 4.887085e-1, 2.905273e-1, 1.301880e-1, 3.140869e-1, 5.482483e-1, 4.210815e-1, 1.182861e-1},
    {0.000000e+0, 3.155518e-2, 8.581543e-2, 1.364746e-1, 1.858826e-1, 2.368469e-1, 2.888184e-1, 3.432617e-1,
     4.012451e-1, 4.623108e-1, 5.271301e-1, 5.954895e-1, 6.681213e-1, 7.448425e-1, 8.245239e-1, 9.097290e-1},
    {0.000000e+0, 4.418945e-2, 1.303711e-1, 2.273560e-1, 3.395996e-1, 4.735718e-1, 6.267090e-1, 8.003845e-1},
    {0.000000e+0, 2.804565e-2, 9.683228e-2, 1.849976e-1, 3.005981e-1, 4.470520e-1, 6.168518e-1, 8.007813e-1}};
const double BEX_M0_B4[5][16] = {
    {0.000000e+0, 2.708740e-1, 3.479614e-1, 3.578186e-1, 5.083618e-1, 5.299072e-1, 5.819092e-1, 6.381836e-1,
     7.276917e-1, 7.595520e-1, 7.878723e-1, 9.707336e-1, 9.713135e-1, 9.736023e-1, 9.759827e-1, 9.832458e-1},
    {0.000000e+0, 2.330627e-1, 5.891418e-1, 7.170410e-1, 2.036438e-1, 1.613464e-1, 6.668701e-1, 9.481201e-1,
     9.769897e-1, 5.111694e-1, 3.522644e-1, 8.209534e-1, 2.933960e-1, 9.757690e-1, 5.289917e-1, 4.372253e-1},
    {0.000000e+0, 4.360962e-2, 1.056519e-1, 1.590576e-1, 2.078857e-1, 2.572937e-1, 3.082581e-1, 3.616028e-1,
     4.191589e-1, 4.792175e-1, 5.438538e-1, 6.125183e-1, 6.841125e-1, 7.589417e-1, 8.365173e-1, 9.148254e-1},
    {0.000000e+0, 4.074097e-2, 1.164551e-1, 2.077026e-1, 3.184509e-1, 4.532166e-1, 6.124268e-1, 7.932129e-1},
    {0.000000e+0, 8.880615e-3, 2.932739e-2, 5.593872e-2, 8.825684e-2, 1.259155e-1, 1.721497e-1, 2.270813e-1,
     2.901611e-1, 3.579712e-1, 4.334106e-1, 5.147095e-1, 6.023254e-1, 6.956177e-1, 7.952881e-1, 8.977356e-1}};
const double BEX_M0_B5[3][32] = {
    {0.000000e+0, 7.379150e-2, 1.806335e-1, 2.687073e-1, 3.407898e-1, 4.047546e-1, 4.621887e-1, 5.168762e-1,
     5.703125e-1, 6.237488e-1, 6.763611e-1, 7.288208e-1, 7.808533e-1, 8.337708e-1, 8.874512e-1, 9.418030e-1},
    {0.000000e+0, 7.980347e-2, 1.615295e-1, 1.665649e-1, 1.822205e-1, 2.185669e-1, 2.292175e-1, 2.456665e-1,
     2.666321e-1, 3.306580e-1, 3.330688e-1, 3.765259e-1, 4.085083e-1, 4.400024e-1, 4.407654e-1, 4.817505e-1,
     4.924011e-1, 5.320740e-1, 5.893860e-1, 6.131287e-1, 6.212463e-1, 6.278076e-1, 6.308899e-1, 7.660828e-1,
     7.850647e-1, 7.910461e-1, 7.929382e-1, 8.038330e-1, 9.834900e-1, 9.846191e-1, 9.852295e-1, 9.862671e-1},
    {0.000000e+0, 6.084290e-1, 3.672791e-1, 3.151855e-1, 1.488953e-1, 2.571716e-1, 5.103455e-1, 3.311157e-1,
     5.426025e-2, 4.254456e-1, 7.998352e-1, 7.873230e-1, 5.418701e-1, 2.925110e-1, 8.468628e-2, 1.410522e-1,
     9.819641e-1, 9.609070e-1, 3.530884e-2, 9.729004e-2, 5.758362e-1, 9.941711e-1, 7.215576e-1, 7.183228e-1,
     2.028809e-1, 9.588623e-2, 2.032166e-1, 1.338806e-1, 5.003357e-1, 1.874390e-1, 9.804993e-1, 1.107788e-1}};
const double BEX_M2_SCALE[64] = {
    4.272461e-4, 1.312256e-3, 2.441406e-3, 3.692627e-3, 4.913330e-3, 6.134033e-3, 7.507324e-3, 8.972168e-3,
    1.049805e-2, 1.223755e-2, 1.406860e-2, 1.599121e-2, 1.800537e-2, 2.026367e-2, 2.264404e-2, 2.517700e-2,
    2.792358e-2, 3.073120e-2, 3.344727e-2, 3.631592e-2, 3.952026e-2, 4.275513e-2, 4.608154e-2, 4.968262e-2,
    5.355835e-2, 5.783081e-2, 6.195068e-2, 6.677246e-2, 7.196045e-2, 7.745361e-2, 8.319092e-2, 8.993530e-2,
    9.759521e-2, 1.056213e-1, 1.138916e-1, 1.236267e-1, 1.348267e-1, 1.470337e-1, 1.603394e-1, 1.755676e-1,
    1.905823e-1, 2.071228e-1, 2.245178e-1, 2.444153e-1, 2.658997e-1, 2.897644e-1, 3.146057e-1, 3.450012e-1,
    3.766174e-1, 4.122620e-1, 4.505615e-1, 4.893799e-1, 5.305481e-1, 5.731201e-1, 6.157837e-1, 6.580811e-1,
    6.985168e-1, 7.435303e-1, 7.865906e-1, 8.302612e-1, 8.718567e-1, 9.125671e-1, 9.575806e-1, 9.996643e-1};
const double BEX_M3_INITIAL[16] = {3.491211e-1, 5.371094e-1, 6.782227e-1, 7.910156e-1, 9.057617e-1, 1.024902e+0,
                                   1.156250e+0, 1.290527e+0, 1.458984e+0, 1.664551e+0, 1.929688e+0, 2.278320e+0,
                                   2.831543e+0, 3.659180e+0, 5.257813e+0, 8.373047e+0};
const double BEX_M3_RATE[16] = {-2.913818e-1, -2.541504e-1, -1.664429e-1, -1.476440e-1, -1.342163e-1, -1.220703e-1,
                                -1.117554e-1, -1.026611e-1, -9.436035e-2, -8.483887e-2, -7.476807e-2, -6.304932e-2,
                                -4.492188e-2, -2.447510e-2, +1.831055e-4, +4.174805e-2};
const double BEX_M4_MULT[8] = {3.610229e-2, 1.260681e-1, 2.227478e-1, 3.338318e-1,
                               4.662170e-1, 6.221313e-1, 7.989197e-1, 9.939575e-1};

// --- Huffman books -----------------------------------------------------------------------------------------------
struct BookDef {
  const uint8_t* bits = nullptr;
  const uint16_t* codes = nullptr;
  int entries = 0, values_per_code = 0, values_per_code_log2 = 0, value_bits = 0, value_range = 0, max_bits = 0;
};
#include "codec/at9_huffman.inc"

struct Book {
  const BookDef* def = nullptr;
  std::vector<uint8_t> lookup;  // max_bits-wide prefix -> entry
};
struct Books {
  Book sf_unsigned[7], sf_signed[6], spectrum[64];
  static void build(Book& b, const BookDef& d) {
    if (!d.entries) return;
    b.def = &d;
    b.lookup.assign(size_t(1) << d.max_bits, 0);
    for (int e = 0; e < d.entries; e++) {
      if (!d.bits[e]) continue;
      int free_bits = d.max_bits - d.bits[e];
      uint32_t first = uint32_t(d.codes[e]) << free_bits;
      std::fill_n(b.lookup.begin() + first, size_t(1) << free_bits, uint8_t(e));
    }
  }
  Books() {
    for (int i = 0; i < 7; i++) build(sf_unsigned[i], kSfUnsignedBooks[i]);
    for (int i = 0; i < 6; i++) build(sf_signed[i], kSfSignedBooks[i]);
    for (int i = 0; i < 64; i++) build(spectrum[i], kSpectrumBooks[i]);
  }
};
const Books& books() {
  static const Books b;
  return b;
}

// --- derived tables ----------------------------------------------------------------------------------------------
double step_size(int precision) { return 2.0 / double((1 << (precision + 1)) - 1); }
double fine_step_size(int precision) { return step_size(15) / double((1 << (precision + 1)) - 1); }
double spectrum_scale(int sf) { return std::ldexp(1.0, sf - 15); }

struct Gradients {
  uint8_t curve[48][48];
  Gradients() {
    for (int len = 1; len <= 48; len++)
      for (int i = 0; i < len; i++) curve[len - 1][i] = GRADIENT_BASE[i * 48 / len];
  }
};
const Gradients& gradients() {
  static const Gradients g;
  return g;
}

int sign_extend(int v, int bits) { return int(uint32_t(v) << (32 - bits)) >> (32 - bits); }

// MSB-first bit reader over a zero-padded copy of the superframe.
struct Bits {
  const uint8_t* p;
  size_t pos = 0;  // in bits
  uint32_t peek(int n) const {
    size_t byte = pos >> 3;
    uint64_t w = 0;
    for (int i = 0; i < 5; i++) w = (w << 8) | p[byte + i];
    return uint32_t((w << (24 + (pos & 7))) >> (64 - n)) & ((n == 32) ? 0xffffffffu : ((1u << n) - 1));
  }
  int read(int n) {
    if (!n) return 0;
    uint32_t v = peek(n);
    pos += size_t(n);
    return int(v);
  }
  int read_signed(int n) { return sign_extend(read(n), n); }
  void skip(int n) { pos += size_t(n); }
  void align() { pos = (pos + 7) & ~size_t(7); }
  int huff(const Book& b, bool is_signed) {
    const BookDef& d = *b.def;
    int e = b.lookup[peek(d.max_bits)];
    pos += d.bits[e];
    return is_signed ? sign_extend(e, d.value_bits) : e;
  }
};

// --- IMDCT ------------------------------------------------------------------------------------------------------
// DCT-IV of size N (X[k] = sum x[n] cos(pi/N (n+1/2)(k+1/2))) through an N/2-point complex FFT, then the format's
// windowed overlap-add.
struct Imdct {
  int n = 0;
  std::vector<std::complex<double>> pre, post, twiddle;
  std::vector<int> rev;
  std::vector<double> window;
  void init(int bits) {
    n = 1 << bits;
    int h = n / 2;
    pre.resize(h);
    post.resize(h);
    for (int i = 0; i < h; i++) {
      pre[i] = std::polar(1.0, -PI * (4 * i + 1) / (4.0 * n));
      post[i] = std::polar(1.0, -PI * i / double(n));
    }
    twiddle.resize(h / 2 > 0 ? h / 2 : 1);
    for (int i = 0; i < h / 2; i++) twiddle[i] = std::polar(1.0, -2 * PI * i / h);
    rev.resize(h);
    int lb = bits - 1;
    for (int i = 0; i < h; i++) {
      int r = 0;
      for (int b = 0; b < lb; b++) r |= ((i >> b) & 1) << (lb - 1 - b);
      rev[i] = r;
    }
    // Synthesis window: w[i] / (w[i]^2 + w[N-1-i]^2) for the analysis sine window w.
    std::vector<double> w(n);
    for (int i = 0; i < n; i++) w[i] = (std::sin(((i + 0.5) / n - 0.5) * PI) + 1.0) * 0.5;
    window.resize(n);
    for (int i = 0; i < n; i++) window[i] = w[i] / (w[n - 1 - i] * w[n - 1 - i] + w[i] * w[i]);
  }
  void dct4(const double* x, double* out) const {
    int h = n / 2;
    std::complex<double> z[128];
    for (int i = 0; i < h; i++) z[rev[i]] = std::complex<double>(x[2 * i], x[n - 1 - 2 * i]) * pre[i];
    for (int len = 2; len <= h; len <<= 1)
      for (int s = 0; s < h; s += len)
        for (int j = 0; j < len / 2; j++) {
          std::complex<double> t = z[s + j + len / 2] * twiddle[j * (h / len)];
          z[s + j + len / 2] = z[s + j] - t;
          z[s + j] += t;
        }
    for (int k = 0; k < h; k++) {
      std::complex<double> y = z[k] * post[k];
      out[2 * k] = y.real();
      out[n - 1 - 2 * k] = -y.imag();
    }
  }
  // spectrum -> pcm (n samples), `overlap` carries the second half between frames.
  void run(const double* spectrum, double* pcm, double* overlap) const {
    double d[256];
    dct4(spectrum, d);
    int h = n / 2;
    for (int i = 0; i < h; i++) {
      pcm[i] = window[i] * d[i + h] + overlap[i];
      pcm[i + h] = -window[i + h] * d[n - 1 - i] - overlap[i + h];
      overlap[i] = -window[n - 1 - i] * d[h - 1 - i];
      overlap[i + h] = window[h - 1 - i] * d[i];
    }
  }
};

// --- decoder state -----------------------------------------------------------------------------------------------
struct Block;
struct Channel {
  Block* block = nullptr;
  int index = 0;  // within the block
  int coded_units = 0, sf_mode = 0;
  int sf[31] = {}, sf_prev[31] = {};
  int precision[30] = {}, precision_fine[30] = {}, mask[30] = {}, book_set[30] = {};
  int q[256] = {}, q_fine[256] = {};
  double spectrum[256] = {}, pcm[256] = {}, overlap[256] = {};
  int bex_mode = 0, bex_count = 0, bex_values[4] = {};
  bool rng_seeded = false;
  uint16_t rng[4] = {};  // A, B, C, D
};

struct Block {
  BlockType type = MONO;
  int nch = 1;
  Channel ch[2];
  bool first_in_superframe = false, reuse_band_params = false;
  int bands = 0, stereo_band = 0, ext_band = 0;
  int units = 0, stereo_units = 0, ext_units = 0, units_prev = 0;
  int gradient[48] = {}, grad_mode = 0, grad_start_unit = 0, grad_start_value = 0, grad_end_unit = 0,
      grad_end_value = 0, grad_boundary = 0;
  int primary = 0;
  int js_signs[30] = {};
  bool bex_enabled = false, has_ext_data = false;
};

}  // namespace

struct Atrac9::State {
  bool high_rate = false;
  int rate_index = 0, frame_bits = 0;
  int frame_in_superframe = 0;
  int nblocks = 0;
  Block blocks[5];
  Channel* out_ch[8] = {};
  Imdct imdct;

  bool unpack_frame(Bits& br);
  bool unpack_block(Block& b, Bits& br);
  bool read_band_params(Block& b, Bits& br);
  bool read_gradient(Block& b, Bits& br);
  void make_gradient(Block& b);
  void read_stereo(Block& b, Bits& br);
  bool read_extension(Block& b, Bits& br);
  bool read_scale_factors(Channel& c, Bits& br);
  void compute_mask(Channel& c);
  void compute_precisions(Channel& c);
  void choose_books(Channel& c);
  void read_spectra(Channel& c, Bits& br);
  void read_spectra_fine(Channel& c, Bits& br);
  void unpack_lfe(Block& b, Bits& br);
  void reconstruct(Block& b);
  void band_extension(Channel& c);
  static uint16_t rng_next(Channel& c);
};

// --- bitstream unpacking -----------------------------------------------------------------------------------------
bool Atrac9::State::unpack_frame(Bits& br) {
  for (int i = 0; i < nblocks; i++) {
    if (!unpack_block(blocks[i], br)) return false;
    if (blocks[i].first_in_superframe && frame_in_superframe) return false;
  }
  return true;
}

bool Atrac9::State::unpack_block(Block& b, Bits& br) {
  b.first_in_superframe = !br.read(1);
  b.reuse_band_params = br.read(1);
  if (b.type == LFE) {
    unpack_lfe(b, br);
  } else {
    if (b.first_in_superframe && b.reuse_band_params) return false;
    if (!b.reuse_band_params && !read_band_params(b, br)) return false;
    if (!read_gradient(b, br)) return false;
    make_gradient(b);
    read_stereo(b, br);
    if (!read_extension(b, br)) return false;
    for (int i = 0; i < b.nch; i++) {
      Channel& c = b.ch[i];
      c.coded_units = b.primary == c.index ? b.units : b.stereo_units;
      if (!read_scale_factors(c, br)) return false;
      compute_mask(c);
      compute_precisions(c);
      choose_books(c);
      read_spectra(c, br);
      read_spectra_fine(c, br);
    }
    b.units_prev = b.bex_enabled ? b.ext_units : b.units;
  }
  br.align();
  return true;
}

bool Atrac9::State::read_band_params(Block& b, Bits& br) {
  int min_bands = MIN_BANDS[high_rate];
  b.bands = br.read(4) + min_bands;
  if (b.bands > MAX_BANDS[rate_index]) return false;
  b.units = BAND_UNITS[b.bands];
  if (b.type == STEREO) {
    b.stereo_band = br.read(4) + min_bands;
    b.stereo_units = BAND_UNITS[b.stereo_band];
  } else {
    b.stereo_band = b.bands;
  }
  if (b.stereo_band > b.bands) return false;
  b.bex_enabled = br.read(1);
  if (b.bex_enabled) {
    b.ext_band = br.read(4) + min_bands;
    if (b.ext_band < b.bands || b.ext_band > MAX_EXT_BAND[high_rate]) return false;
    b.ext_units = BAND_UNITS[b.ext_band];
  } else {
    b.ext_band = b.bands;
    b.ext_units = b.units;
  }
  return true;
}

bool Atrac9::State::read_gradient(Block& b, Bits& br) {
  b.grad_mode = br.read(2);
  if (b.grad_mode > 0) {
    b.grad_end_unit = 31;
    b.grad_end_value = 31;
    b.grad_start_unit = br.read(5);
    b.grad_start_value = br.read(5);
  } else {
    b.grad_start_unit = br.read(6);
    b.grad_end_unit = br.read(6) + 1;
    b.grad_start_value = br.read(5);
    b.grad_end_value = br.read(5);
  }
  b.grad_boundary = br.read(4);
  return b.grad_boundary <= b.units && b.grad_start_unit < 48 && b.grad_end_unit < 48 &&
         b.grad_start_unit <= b.grad_end_unit;
}

void Atrac9::State::make_gradient(Block& b) {
  for (int i = 0; i < b.grad_end_unit; i++) b.gradient[i] = b.grad_start_value;
  for (int i = b.grad_end_unit; i <= b.units; i++) b.gradient[i] = b.grad_end_value;
  int units = b.grad_end_unit - b.grad_start_unit, values = b.grad_end_value - b.grad_start_value;
  if (units <= 0 || values == 0) return;
  const uint8_t* curve = gradients().curve[units - 1];
  double scale = (std::abs(values) - 1) / 31.0;
  int dir = values > 0 ? 1 : -1;
  int base = b.grad_start_value + dir;
  for (int i = b.grad_start_unit; i < b.grad_end_unit; i++)
    b.gradient[i] = base + dir * int(curve[i - b.grad_start_unit] * scale);
}

void Atrac9::State::read_stereo(Block& b, Bits& br) {
  if (b.type != STEREO) return;
  b.primary = br.read(1);
  bool has_signs = br.read(1);
  std::memset(b.js_signs, 0, sizeof b.js_signs);
  if (has_signs)
    for (int i = b.stereo_units; i < b.units; i++) b.js_signs[i] = br.read(1);
}

bool Atrac9::State::read_extension(Block& b, Bits& br) {
  int bex_bands = 0;
  auto header = [&](Channel& c) {
    int mode = br.read(2);
    c.bex_mode = bex_bands > 2 ? mode : 4;
    c.bex_count = BEX_VALUE_COUNT[c.bex_mode][bex_bands];
  };
  auto values = [&](Channel& c) {
    for (int i = 0; i < c.bex_count; i++) c.bex_values[i] = br.read(BEX_VALUE_BITS[c.bex_mode][bex_bands][i]);
  };
  if (b.bex_enabled) {
    if (b.units < 13) return false;
    bex_bands = BEX_GROUPS[b.units - 13].bands;
    if (b.type == STEREO) header(b.ch[1]);
    else br.skip(1);
  }
  b.has_ext_data = br.read(1);
  if (!b.has_ext_data) return true;
  if (!b.bex_enabled) {
    br.read(2);  // mode, unused without band extension
    br.skip(br.read(5));
    return true;
  }
  header(b.ch[0]);
  int len = br.read(5);
  if (!len) return true;
  size_t end = br.pos + size_t(len);
  values(b.ch[0]);
  if (b.type == STEREO) values(b.ch[1]);
  return br.pos <= end;
}

bool Atrac9::State::read_scale_factors(Channel& c, Bits& br) {
  Block& b = *c.block;
  std::memset(c.sf, 0, sizeof c.sf);
  c.sf_mode = br.read(2);
  const Books& bk = books();

  // Absolute: fixed-width values above a base.
  auto clc = [&] {
    int width = br.read(2) + 2;
    int base = width < 5 ? br.read(5) : 0;
    for (int i = 0; i < b.ext_units; i++) c.sf[i] = br.read(width) + base;
  };
  // First value, then Huffman-coded deltas (mod book range), plus a base minus a weight curve.
  auto vlc_delta = [&] {
    const uint8_t* weights = SF_WEIGHTS[br.read(3)];
    int base = br.read(5);
    int width = br.read(2) + 3;
    const Book& book = bk.sf_unsigned[width];
    c.sf[0] = br.read(width);
    for (int i = 1; i < b.ext_units; i++) c.sf[i] = (c.sf[i - 1] + br.huff(book, false)) & (book.def->value_range - 1);
    for (int i = 0; i < b.ext_units; i++) c.sf[i] += base - weights[i];
  };
  // Signed Huffman distance from a baseline (previous frame or the other channel).
  auto vlc_distance = [&](const int* baseline, int baseline_len) {
    int width = br.read(2) + 2;
    const Book& book = bk.sf_signed[width];
    int n = std::min(b.ext_units, baseline_len);
    for (int i = 0; i < n; i++) c.sf[i] = (baseline[i] + br.huff(book, true)) & 31;
    for (int i = n; i < b.ext_units; i++) c.sf[i] = br.read(5);
  };
  // Huffman deltas added to a baseline plus a signed offset.
  auto vlc_delta_baseline = [&](const int* baseline, int baseline_len) {
    int base = br.read(5) - 16;
    int width = br.read(2) + 1;
    const Book& book = bk.sf_unsigned[width];
    int n = std::min(b.ext_units, baseline_len);
    c.sf[0] = br.read(width);
    for (int i = 1; i < n; i++) c.sf[i] = (c.sf[i - 1] + br.huff(book, false)) & (book.def->value_range - 1);
    for (int i = 0; i < n; i++) c.sf[i] += base + baseline[i];
    for (int i = n; i < b.ext_units; i++) c.sf[i] = br.read(5);
  };

  if (c.index == 0) {
    switch (c.sf_mode) {
      case 0: vlc_delta(); break;
      case 1: clc(); break;
      case 2:
        if (b.first_in_superframe) return false;
        vlc_distance(c.sf_prev, b.units_prev);
        break;
      case 3:
        if (b.first_in_superframe) return false;
        vlc_delta_baseline(c.sf_prev, b.units_prev);
        break;
    }
  } else {
    switch (c.sf_mode) {
      case 0: vlc_delta(); break;
      case 1: vlc_distance(b.ch[0].sf, b.ext_units); break;
      case 2: vlc_delta_baseline(b.ch[0].sf, b.ext_units); break;
      case 3:
        if (b.first_in_superframe) return false;
        vlc_distance(c.sf_prev, b.units_prev);
        break;
    }
  }
  for (int i = 0; i < b.ext_units; i++)
    if (c.sf[i] < 0 || c.sf[i] > 31) return false;
  std::memcpy(c.sf_prev, c.sf, sizeof c.sf);
  return true;
}

// Units next to a big scale-factor step get extra precision.
void Atrac9::State::compute_mask(Channel& c) {
  std::memset(c.mask, 0, sizeof c.mask);
  for (int i = 1; i < c.block->units; i++) {
    int d = c.sf[i] - c.sf[i - 1];
    if (d > 1) c.mask[i] += std::min(d - 1, 5);
    else if (d < -1) c.mask[i - 1] += std::min(-d - 1, 5);
  }
}

void Atrac9::State::compute_precisions(Channel& c) {
  Block& b = *c.block;
  for (int i = 0; i < b.units; i++) {
    int p;
    if (b.grad_mode) {
      p = c.sf[i] + c.mask[i] - b.gradient[i];
      if (p > 0) p = b.grad_mode == 1 ? p / 2 : b.grad_mode == 2 ? 3 * p / 8 : p / 4;
    } else {
      p = c.sf[i] - b.gradient[i];
    }
    c.precision[i] = std::max(p, 1);
  }
  for (int i = 0; i < b.grad_boundary; i++) c.precision[i]++;
  for (int i = 0; i < b.units; i++) {
    c.precision_fine[i] = std::max(c.precision[i] - 15, 0);
    c.precision[i] = std::min(c.precision[i], 15);
  }
}

// Peaky units use the second book set.
void Atrac9::State::choose_books(Channel& c) {
  std::memset(c.book_set, 0, sizeof c.book_set);
  int n = c.coded_units;
  if (n <= 1 || high_rate) return;
  int sf[32];
  std::memcpy(sf, c.sf, sizeof c.sf);
  sf[n] = sf[n - 1];
  int avg = 0;
  if (n > 12) {
    for (int i = 0; i < 12; i++) avg += sf[i];
    avg = (avg + 6) / 12;
  }
  for (int i = 8; i < n; i++) {
    int lo = std::min(sf[i - 1], sf[i + 1]);
    if (sf[i] - lo >= 3 || sf[i] - sf[i - 1] + sf[i] - sf[i + 1] >= 3) c.book_set[i] = 1;
  }
  for (int i = 12; i < n; i++) {
    if (c.book_set[i]) continue;
    int lo = std::min(sf[i - 1], sf[i + 1]);
    int width = UNIT_START[i + 1] - UNIT_START[i];
    if (sf[i] - lo >= 2 && sf[i] >= avg - (width == 16 ? 1 : 0)) c.book_set[i] = 1;
  }
}

void Atrac9::State::read_spectra(Channel& c, Bits& br) {
  std::memset(c.q, 0, sizeof c.q);
  for (int i = 0; i < c.coded_units; i++) {
    int start = UNIT_START[i], count = UNIT_START[i + 1] - start;
    int bits = c.precision[i] + 1;
    if (bits <= MAX_HUFF_PRECISION[high_rate]) {
      const Book& book = books().spectrum[c.book_set[i] * 32 + bits * 4 + UNIT_BOOK[i]];
      const BookDef& d = *book.def;
      int mask = (1 << d.value_bits) - 1, k = start;
      for (int g = 0; g < count >> d.values_per_code_log2; g++) {
        int v = br.huff(book, false);
        for (int j = 0; j < d.values_per_code; j++, v >>= d.value_bits) c.q[k++] = sign_extend(v & mask, d.value_bits);
      }
    } else {
      for (int k = start; k < start + count; k++) c.q[k] = br.read_signed(bits);
    }
  }
}

void Atrac9::State::read_spectra_fine(Channel& c, Bits& br) {
  std::memset(c.q_fine, 0, sizeof c.q_fine);
  for (int i = 0; i < c.coded_units; i++)
    if (c.precision_fine[i] > 0)
      for (int k = UNIT_START[i]; k < UNIT_START[i + 1]; k++) c.q_fine[k] = br.read_signed(c.precision_fine[i] + 1);
}

void Atrac9::State::unpack_lfe(Block& b, Bits& br) {
  Channel& c = b.ch[0];
  b.units = 2;
  std::memset(c.sf, 0, sizeof c.sf);
  for (int i = 0; i < 2; i++) c.sf[i] = br.read(5);
  int p = b.reuse_band_params ? 8 : 4;
  for (int i = 0; i < 2; i++) {
    c.precision[i] = p;
    c.precision_fine[i] = 0;
  }
  c.coded_units = 2;
  std::memset(c.q, 0, sizeof c.q);
  std::memset(c.q_fine, 0, sizeof c.q_fine);
  for (int i = 0; i < 2; i++)
    for (int k = UNIT_START[i]; k < UNIT_START[i + 1]; k++) c.q[k] = br.read_signed(p + 1);
}

// --- reconstruction ----------------------------------------------------------------------------------------------
uint16_t Atrac9::State::rng_next(Channel& c) {
  uint16_t t = uint16_t(c.rng[3] ^ (c.rng[3] << 5));
  c.rng[3] = c.rng[2];
  c.rng[2] = c.rng[1];
  c.rng[1] = c.rng[0];
  c.rng[0] = uint16_t(t ^ c.rng[0] ^ ((t ^ (c.rng[0] >> 5)) >> 4));
  return c.rng[0];
}

void Atrac9::State::band_extension(Channel& c) {
  Block& b = *c.block;
  const BexGroup& g = BEX_GROUPS[b.units - 13];
  int a_unit = b.units, total_units = std::max(g.c_unit, 22);
  int a_bin = UNIT_START[a_unit], b_bin = UNIT_START[g.b_unit], c_bin = UNIT_START[g.c_unit];
  int total_bins = UNIT_START[total_units];
  double* s = c.spectrum;
  // Mirror the coded spectrum upwards into the three groups.
  for (int i = 0; i < b_bin - a_bin; i++) s[a_bin + i] = s[a_bin - i - 1];
  for (int i = 0; i < c_bin - b_bin; i++) s[b_bin + i] = s[b_bin - i - 1];
  for (int i = 0; i < total_bins - c_bin; i++) s[c_bin + i] = s[c_bin - i - 1];

  auto noise = [&](int start, int count) {
    if (!c.rng_seeded) {
      uint16_t seed = uint16_t(543 * (c.sf[8] + c.sf[12] + c.sf[15] + 1));
      int v = 0x4D93 * (seed ^ (seed >> 14));
      for (int i = 0; i < 4; i++) c.rng[i] = uint16_t(3 - i - v);
      c.rng_seeded = true;
    }
    for (int i = 0; i < count; i++) s[start + i] = rng_next(c) / 65535.0 * 2.0 - 1.0;
  };
  auto scale_units = [&](const double* scales) {
    for (int u = a_unit; u < total_units; u++)
      for (int k = UNIT_START[u]; k < UNIT_START[u + 1]; k++) s[k] *= scales[u - a_unit];
  };
  auto scale_bins = [&](int from, int to, double f) {
    for (int k = from; k < to; k++) s[k] *= f;
  };
  const int* v = c.bex_values;
  double scales[8] = {};
  switch (c.bex_mode) {
    case 0:
      if (g.bands == 3) {
        scales[0] = BEX_M0_B3[0][v[0]];
        scales[1] = BEX_M0_B3[1][v[0]];
        scales[2] = BEX_M0_B3[2][v[1]];
        scales[3] = BEX_M0_B3[3][v[2]];
        scales[4] = BEX_M0_B3[4][v[3]];
      } else if (g.bands == 4) {
        scales[0] = BEX_M0_B4[0][v[0]];
        scales[1] = BEX_M0_B4[1][v[0]];
        scales[2] = BEX_M0_B4[2][v[1]];
        scales[3] = BEX_M0_B4[3][v[2]];
        scales[4] = BEX_M0_B4[4][v[3]];
      } else if (g.bands == 5) {
        scales[0] = BEX_M0_B5[0][v[0]];
        scales[1] = BEX_M0_B5[1][v[1]];
        scales[2] = BEX_M0_B5[2][v[1]];
      }
      scales[total_units - a_unit - 1] = spectrum_scale(c.sf[a_unit]);
      noise(UNIT_START[total_units - 1], UNIT_START[total_units] - UNIT_START[total_units - 1]);
      scale_units(scales);
      break;
    case 1:
      for (int u = a_unit; u < total_units; u++) scales[u - a_unit] = spectrum_scale(c.sf[u]);
      noise(a_bin, total_bins - a_bin);
      scale_units(scales);
      break;
    case 2:
      scale_bins(a_bin, b_bin, BEX_M2_SCALE[v[0]]);
      scale_bins(b_bin, c_bin, BEX_M2_SCALE[v[1]]);
      break;
    case 3: {
      double rate = std::pow(2.0, BEX_M3_RATE[v[1]]), f = BEX_M3_INITIAL[v[0]];
      for (int k = a_bin; k < total_bins; k++) s[k] *= (f *= rate);
      break;
    }
    case 4: {
      double m = BEX_M4_MULT[v[0]];
      scale_bins(a_bin, b_bin, 0.7079468 * m);
      scale_bins(b_bin, c_bin, 0.5011902 * m);
      scale_bins(c_bin, total_bins, 0.3548279 * m);
      break;
    }
  }
}

void Atrac9::State::reconstruct(Block& b) {
  // Dequantise.
  for (int i = 0; i < b.nch; i++) {
    Channel& c = b.ch[i];
    std::memset(c.spectrum, 0, sizeof c.spectrum);
    for (int u = 0; u < c.coded_units; u++) {
      double step = step_size(c.precision[u]), fine = fine_step_size(c.precision_fine[u]);
      for (int k = UNIT_START[u]; k < UNIT_START[u + 1]; k++) c.spectrum[k] = c.q[k] * step + c.q_fine[k] * fine;
    }
  }
  // Intensity stereo: units above the stereo band copy the primary channel, sign-flipped where flagged.
  if (b.type == STEREO && b.stereo_units < b.units) {
    Channel& src = b.ch[b.primary];
    Channel& dst = b.ch[1 - b.primary];
    for (int u = b.stereo_units; u < b.units; u++)
      for (int k = UNIT_START[u]; k < UNIT_START[u + 1]; k++)
        dst.spectrum[k] = b.js_signs[u] ? -src.spectrum[k] : src.spectrum[k];
  }
  // Scale factors.
  for (int i = 0; i < b.nch; i++) {
    Channel& c = b.ch[i];
    for (int u = 0; u < b.units; u++)
      for (int k = UNIT_START[u]; k < UNIT_START[u + 1]; k++) c.spectrum[k] *= spectrum_scale(c.sf[u]);
  }
  if (b.bex_enabled && b.has_ext_data)
    for (int i = 0; i < b.nch; i++) band_extension(b.ch[i]);
  for (int i = 0; i < b.nch; i++) imdct.run(b.ch[i].spectrum, b.ch[i].pcm, b.ch[i].overlap);
}

// --- public ------------------------------------------------------------------------------------------------------
Atrac9::Atrac9() = default;
Atrac9::~Atrac9() = default;

bool Atrac9::init(const uint8_t cfg[4]) {
  s_.reset();
  channels_ = 0;
  if (cfg[0] != 0xFE) return false;
  int rate = cfg[1] >> 4, layout = (cfg[1] >> 1) & 7;
  if ((cfg[1] & 1) || layout > 5) return false;
  int frame_bytes = ((cfg[2] << 3) | (cfg[3] >> 5)) + 1, sf_index = (cfg[3] >> 3) & 3;
  s_ = std::make_unique<State>();
  State& s = *s_;
  s.rate_index = rate;
  s.high_rate = rate > 7;
  s.frame_bits = FRAME_POWER[rate];
  s.nblocks = LAYOUTS[layout].blocks;
  int n = 0;
  for (int i = 0; i < s.nblocks; i++) {
    Block& b = s.blocks[i];
    b.type = LAYOUTS[layout].type[i];
    b.nch = b.type == STEREO ? 2 : 1;
    for (int j = 0; j < b.nch; j++) {
      b.ch[j].block = &b;
      b.ch[j].index = j;
      s.out_ch[n++] = &b.ch[j];
    }
  }
  s.imdct.init(s.frame_bits);
  channels_ = uint32_t(n);
  sample_rate_ = uint32_t(SAMPLE_RATES[rate]);
  frame_samples_ = 1u << s.frame_bits;
  frames_per_superframe_ = 1u << sf_index;
  superframe_bytes_ = uint32_t(frame_bytes) << sf_index;
  books();
  gradients();
  return true;
}

void Atrac9::reset() {
  if (!s_) return;
  State& s = *s_;
  s.frame_in_superframe = 0;
  for (int i = 0; i < s.nblocks; i++) {
    Block& b = s.blocks[i];
    BlockType t = b.type;
    int nch = b.nch;
    b = Block{};
    b.type = t;
    b.nch = nch;
    for (int j = 0; j < nch; j++) {
      b.ch[j].block = &b;
      b.ch[j].index = j;
    }
  }
}

bool Atrac9::decode_superframe(const uint8_t* data, float* out) {
  if (!s_) return false;
  State& s = *s_;
  std::vector<uint8_t> buf(superframe_bytes_ + 8, 0);  // zero padding for the bit reader's lookahead
  std::memcpy(buf.data(), data, superframe_bytes_);
  Bits br{buf.data()};
  bool ok = true;
  for (uint32_t f = 0; f < frames_per_superframe_; f++) {
    s.frame_in_superframe = int(f);
    float* o = out + size_t(f) * frame_samples_ * channels_;
    if (ok && s.unpack_frame(br) && br.pos <= size_t(superframe_bytes_) * 8) {
      for (int i = 0; i < s.nblocks; i++) s.reconstruct(s.blocks[i]);
      for (uint32_t k = 0; k < frame_samples_; k++)
        for (uint32_t ch = 0; ch < channels_; ch++) o[k * channels_ + ch] = float(s.out_ch[ch]->pcm[k]);
    } else {
      ok = false;
      std::fill_n(o, size_t(frame_samples_) * channels_, 0.0f);
    }
  }
  return ok;
}

}  // namespace codec
