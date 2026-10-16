// dcp_audio: inspect CRI audio containers (ACB cue sheets, AFS2 wave banks, HCA stream headers).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "formats/acb.h"
#include "formats/afs2.h"
#include "formats/cpk.h"
#include "formats/eboot_image.h"
#include "formats/hca_decoder.h"
#include "formats/hca_header.h"

namespace fs = std::filesystem;
using namespace dcp::formats;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: dcp_audio list   <file.acb> [file.awb]  (cue -> waveform -> AWB stream, codec, cipher)\n"
                 "       dcp_audio verify <archive.cpk>          (parse every .acb/.awb and their stream headers)\n");
    return 2;
}

// Reads `size` bytes at `offset` of one AWB; lets huge streaming AWBs be inspected without loading them.
using RangeReader = std::function<std::vector<uint8_t>(uint64_t offset, size_t size)>;

RangeReader span_reader(std::span<const uint8_t> b) {
    return [b](uint64_t off, size_t n) {
        if (off > b.size() || n > b.size() - off) throw std::runtime_error("read past end of AWB");
        return std::vector<uint8_t>(b.begin() + ptrdiff_t(off), b.begin() + ptrdiff_t(off + n));
    };
}

Afs2Header read_afs2(const RangeReader& rd, uint64_t total) {
    if (total < 16) throw std::runtime_error("afs2: file too small");
    uint64_t hs = afs2_header_size(rd(0, 16));
    if (hs > total) throw std::runtime_error("afs2: header larger than file");
    return parse_afs2(rd(0, size_t(hs)), total);
}

// HCA header of a stream, or nullopt if the stream is another codec.
std::optional<HcaHeader> read_hca(const RangeReader& rd, const Afs2Stream& s) {
    if (s.size < 8) return std::nullopt;
    auto head = rd(s.offset, 8);
    if (!is_hca(head)) return std::nullopt;
    uint16_t hs = hca_header_size(head);
    if (hs > s.size) throw std::runtime_error("hca: header larger than stream");
    return parse_hca_header(rd(s.offset, hs));
}

std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

int list(const fs::path& acb_path, const std::optional<fs::path>& awb_path) {
    auto bytes = read_file(acb_path);
    Acb acb(bytes);
    std::optional<Afs2Header> mem, strm;
    RangeReader mem_rd, strm_rd;
    if (!acb.memory_awb().empty()) {
        mem_rd = span_reader(acb.memory_awb());
        mem = read_afs2(mem_rd, acb.memory_awb().size());
    }
    auto awb_file = std::make_shared<std::ifstream>();
    if (awb_path) {
        awb_file->open(*awb_path, std::ios::binary);
        if (!*awb_file) throw std::runtime_error("cannot open " + awb_path->string());
        strm_rd = [awb_file](uint64_t off, size_t n) {
            std::vector<uint8_t> b(n);
            awb_file->clear();
            awb_file->seekg(std::streamoff(off));
            if (!awb_file->read(reinterpret_cast<char*>(b.data()), std::streamsize(n))) throw std::runtime_error("awb read failed");
            return b;
        };
        strm = read_afs2(strm_rd, fs::file_size(*awb_path));
    }

    size_t refs = 0, failures = 0;
    for (const auto& c : acb.cues()) {
        std::printf("cue %u \"%s\"%s\n", c.id, c.name.c_str(), c.waveforms.empty() ? " (no waveform)" : "");
        for (uint16_t wi : c.waveforms) {
            const AcbWaveform& w = acb.waveforms()[wi];
            ++refs;
            std::string where, cipher = "?";
            const Afs2Header* bank = w.in_memory() ? (mem ? &*mem : nullptr) : (strm ? &*strm : nullptr);
            const RangeReader& rd = w.in_memory() ? mem_rd : strm_rd;
            uint16_t id = w.in_memory() ? w.memory_awb_id : w.stream_awb_id;
            where = std::string(w.in_memory() ? "mem:" : "stream:") + std::to_string(id);
            if (bank) {
                try {
                    const Afs2Stream* s = bank->find(id);
                    if (!s) throw std::runtime_error("AWB has no stream " + std::to_string(id));
                    auto h = read_hca(rd, *s);
                    cipher = h ? std::to_string(h->cipher_type) : "n/a";
                } catch (const std::exception& ex) {
                    ++failures;
                    cipher = std::string("ERROR ") + ex.what();
                }
            }
            std::printf("  wf %u %s %uch %uHz %s cipher %s\n", wi, Acb::codec_name(w.encode_type), w.channels,
                        w.sample_rate, where.c_str(), cipher.c_str());
        }
    }
    for (const auto& [what, n] : acb.skipped()) std::printf("skipped %zux %s\n", n, what.c_str());
    std::printf("%zu cues, %zu waveform refs, %zu failures\n", acb.cues().size(), refs, failures);
    return failures ? 1 : 0;
}

struct Stats {
    size_t acb = 0, awb = 0, cues = 0, empty_cues = 0, waveforms = 0, streams = 0, hca = 0, other = 0, failures = 0;
    std::map<uint16_t, size_t> cipher;
    std::map<std::string, size_t> encode, skipped;
};

void fail(Stats& st, const std::string& file, const std::string& why) {
    ++st.failures;
    std::fprintf(stderr, "FAIL %s: %s\n", file.c_str(), why.c_str());
}

// Classifies and header-checks every stream of one bank; returns stream id -> HCA header (if HCA).
std::map<uint32_t, HcaHeader> scan_bank(Stats& st, const std::string& file, const RangeReader& rd, const Afs2Header& h) {
    std::map<uint32_t, HcaHeader> out;
    for (const auto& s : h.streams) {
        ++st.streams;
        try {
            auto hca = read_hca(rd, s);
            if (!hca) { ++st.other; continue; }
            ++st.hca;
            ++st.cipher[hca->cipher_type];
            if (!hca->crc_ok) throw std::runtime_error("header CRC mismatch");
            out[s.id] = *hca;
        } catch (const std::exception& ex) {
            fail(st, file, "stream " + std::to_string(s.id) + ": " + ex.what());
        }
    }
    return out;
}

bool same_streams(const Afs2Header& a, const Afs2Header& b) {
    if (a.streams.size() != b.streams.size()) return false;
    for (size_t i = 0; i < a.streams.size(); ++i)
        if (a.streams[i].id != b.streams[i].id || a.streams[i].offset != b.streams[i].offset ||
            a.streams[i].size != b.streams[i].size)
            return false;
    return true;
}

// Archive path of the streaming AWB an ACB refers to (named in the ACB, else the ACB's own name).
std::string stream_awb_path(const CpkEntry& e, const Acb& acb) {
    std::string awb_name = acb.stream_awb_names().empty() ? fs::path(e.name).stem().string() : acb.stream_awb_names().front();
    return (e.dir.empty() ? "" : e.dir + "/") + awb_name + ".awb";
}

int verify(const CpkArchive& cpk) {
    Stats st;
    struct StreamRef { std::string acb; uint16_t id; uint8_t channels; uint32_t rate; };
    std::map<std::string, std::vector<StreamRef>> wanted;   // awb path -> waveforms that stream from it
    std::map<std::string, std::vector<uint8_t>> acb_copy;   // awb path -> AFS2 header copy stored in the ACB
    std::map<std::string, const CpkEntry*> awbs;

    for (const auto& e : cpk.entries()) {
        std::string ext = fs::path(e.name).extension().string();
        if (ext == ".awb") { awbs[e.path()] = &e; continue; }
        if (ext != ".acb") continue;
        ++st.acb;
        try {
            auto bytes = cpk.read(e);
            Acb acb(bytes);
            st.cues += acb.cues().size();
            for (const auto& c : acb.cues()) st.empty_cues += c.waveforms.empty();
            st.waveforms += acb.waveforms().size();
            for (const auto& [what, n] : acb.skipped()) st.skipped[what] += n;
            for (const auto& w : acb.waveforms()) ++st.encode[Acb::codec_name(w.encode_type)];

            std::map<uint32_t, HcaHeader> mem;
            std::optional<Afs2Header> bank;
            if (!acb.memory_awb().empty()) {
                auto rd = span_reader(acb.memory_awb());
                bank = read_afs2(rd, acb.memory_awb().size());
                mem = scan_bank(st, e.path(), rd, *bank);
            }
            std::string awb_path = stream_awb_path(e, acb);
            if (!acb.stream_awb_header().empty())
                acb_copy[awb_path].assign(acb.stream_awb_header().begin(), acb.stream_awb_header().end());

            for (size_t i = 0; i < acb.waveforms().size(); ++i) {
                const auto& w = acb.waveforms()[i];
                if (w.in_stream()) wanted[awb_path].push_back({e.path(), w.stream_awb_id, w.channels, w.sample_rate});
                if (!w.in_memory()) continue;
                if (!bank || !bank->find(w.memory_awb_id)) {
                    fail(st, e.path(), "waveform " + std::to_string(i) + ": memory AWB has no stream " +
                                           std::to_string(w.memory_awb_id));
                    continue;
                }
                auto it = mem.find(w.memory_awb_id);
                if (it != mem.end() && (it->second.channels != w.channels || it->second.sample_rate != w.sample_rate))
                    fail(st, e.path(), "waveform " + std::to_string(i) + ": channels/rate differ from HCA header");
            }
        } catch (const std::exception& ex) {
            fail(st, e.path(), ex.what());
        }
    }

    for (const auto& [path, e] : awbs) {
        ++st.awb;
        try {
            RangeReader rd = [&cpk, e](uint64_t off, size_t n) { return cpk.read_range(*e, off, n); };
            Afs2Header h = read_afs2(rd, e->stored_size);
            auto hdrs = scan_bank(st, path, rd, h);
            if (auto c = acb_copy.find(path); c != acb_copy.end() && !same_streams(h, parse_afs2(c->second, e->stored_size)))
                fail(st, path, "stream table differs from the copy stored in its ACB");
            for (const auto& r : wanted[path]) {
                auto it = hdrs.find(r.id);
                if (!h.find(r.id))
                    fail(st, r.acb, "streaming waveform id " + std::to_string(r.id) + " missing from " + path);
                else if (it != hdrs.end() && (it->second.channels != r.channels || it->second.sample_rate != r.rate))
                    fail(st, r.acb, "stream " + std::to_string(r.id) + ": channels/rate differ from HCA header");
            }
        } catch (const std::exception& ex) {
            fail(st, path, ex.what());
        }
    }
    for (const auto& [path, refs] : wanted)
        if (!awbs.count(path)) fail(st, refs.front().acb, "streaming AWB not in archive: " + path);

    std::string cipher, encode, skipped;
    for (const auto& [t, n] : st.cipher) cipher += (cipher.empty() ? "" : " ") + std::to_string(t) + ":" + std::to_string(n);
    for (const auto& [t, n] : st.encode) encode += (encode.empty() ? "" : " ") + t + ":" + std::to_string(n);
    for (const auto& [t, n] : st.skipped) skipped += (skipped.empty() ? "" : ", ") + t + " x" + std::to_string(n);
    std::printf("%zu files (%zu acb, %zu awb), %zu cues (%zu without waveform), %zu waveforms [%s], %zu streams (%zu HCA, %zu other), "
                "cipher [%s], skipped [%s], %zu failures\n",
                st.acb + st.awb, st.acb, st.awb, st.cues, st.empty_cues, st.waveforms, encode.c_str(), st.streams, st.hca, st.other,
                cipher.c_str(), skipped.c_str(), st.failures);
    return st.failures ? 1 : 0;
}

void put_le(std::vector<uint8_t>& b, uint32_t v, int n) {
    for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

void put_tag(std::vector<uint8_t>& b, const char* tag) { b.insert(b.end(), tag, tag + 4); }

// 16-bit PCM WAV of the playable range; loop points (if any) go into a 'smpl' chunk as metadata.
void write_wav(const fs::path& out, const HcaPcm& pcm) {
    const unsigned ch = pcm.header.channels;
    uint64_t first = pcm.header.encoder_delay, avail = pcm.samples.size() / ch;
    uint64_t count = std::min<uint64_t>(pcm.playable_samples(), avail > first ? avail - first : 0);
    std::vector<uint8_t> data;
    data.reserve(size_t(count) * ch * 2);
    for (uint64_t i = first * ch; i < (first + count) * ch; ++i) {
        float v = std::clamp(pcm.samples[size_t(i)] * 32768.0f, -32768.0f, 32767.0f);
        put_le(data, uint32_t(uint16_t(int16_t(std::lrint(v)))), 2);
    }
    std::vector<uint8_t> smpl;
    if (pcm.header.has_loop) {
        HcaLoopSamples loop = hca_loop_samples(pcm.header);
        for (uint32_t v : {0u, 0u, uint32_t(1000000000ull / pcm.header.sample_rate), 60u, 0u, 0u, 0u, 1u, 0u,  // header, 1 loop
                           0u, 0u, uint32_t(loop.start), uint32_t(loop.end ? loop.end - 1 : 0), 0u, 0u})       // forward, inclusive end
            put_le(smpl, v, 4);
    }
    std::vector<uint8_t> w;
    put_tag(w, "RIFF");
    put_le(w, uint32_t(4 + 8 + 16 + 8 + data.size() + (smpl.empty() ? 0 : 8 + smpl.size())), 4);
    put_tag(w, "WAVE");
    put_tag(w, "fmt ");
    put_le(w, 16, 4); put_le(w, 1, 2); put_le(w, ch, 2); put_le(w, pcm.header.sample_rate, 4);
    put_le(w, pcm.header.sample_rate * ch * 2, 4); put_le(w, ch * 2, 2); put_le(w, 16, 2);
    if (!smpl.empty()) {
        put_tag(w, "smpl");
        put_le(w, uint32_t(smpl.size()), 4);
        w.insert(w.end(), smpl.begin(), smpl.end());
    }
    put_tag(w, "data");
    put_le(w, uint32_t(data.size()), 4);
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(w.data()), std::streamsize(w.size()));
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!f) throw std::runtime_error("cannot write " + out.string());
}

int wav(const CpkArchive& cpk, const std::string& acb_path, const std::string& cue_key, const fs::path& out) {
    const CpkEntry* e = cpk.find(acb_path);
    if (!e) throw std::runtime_error("not in archive: " + acb_path);
    auto bytes = cpk.read(*e);
    Acb acb(bytes);
    const AcbCue* cue = nullptr;
    for (const auto& c : acb.cues())
        if (c.name == cue_key) cue = &c;
    if (!cue && !cue_key.empty() && cue_key.find_first_not_of("0123456789") == std::string::npos)
        for (const auto& c : acb.cues())
            if (c.id == std::stoul(cue_key)) cue = &c;
    if (!cue) throw std::runtime_error("no cue named or numbered " + cue_key);
    if (cue->waveforms.empty()) throw std::runtime_error("cue has no waveform");
    const AcbWaveform& w = acb.waveforms()[cue->waveforms.front()];

    std::vector<uint8_t> stream;
    if (w.in_memory()) {
        Afs2Header bank = read_afs2(span_reader(acb.memory_awb()), acb.memory_awb().size());
        const Afs2Stream* s = bank.find(w.memory_awb_id);
        if (!s) throw std::runtime_error("memory AWB has no stream " + std::to_string(w.memory_awb_id));
        auto b = afs2_stream(acb.memory_awb(), *s);
        stream.assign(b.begin(), b.end());
    } else {
        std::string path = stream_awb_path(*e, acb);
        const CpkEntry* a = cpk.find(path);
        if (!a) throw std::runtime_error("streaming AWB not in archive: " + path);
        RangeReader rd = [&cpk, a](uint64_t off, size_t n) { return cpk.read_range(*a, off, n); };
        Afs2Header bank = read_afs2(rd, a->stored_size);
        const Afs2Stream* s = bank.find(w.stream_awb_id);
        if (!s) throw std::runtime_error(path + " has no stream " + std::to_string(w.stream_awb_id));
        stream = rd(s->offset, size_t(s->size));
    }
    HcaPcm pcm = decode_hca(stream);
    write_wav(out, pcm);
    std::printf("cue %u \"%s\": %uch %uHz, %llu samples%s, %zu frames, %zu CRC failures, %zu decode failures\n", cue->id,
                cue->name.c_str(), pcm.header.channels, pcm.header.sample_rate, (unsigned long long)pcm.playable_samples(),
                pcm.header.has_loop ? " (loop)" : "", pcm.frames, pcm.crc_failures, pcm.decode_failures);
    return pcm.crc_failures || pcm.decode_failures ? 1 : 0;
}

int decode_file(const fs::path& in, const fs::path& out) {
    HcaPcm pcm = decode_hca(read_file(in));
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(pcm.samples.data()), std::streamsize(pcm.samples.size() * sizeof(float)));
    if (!f) throw std::runtime_error("cannot write " + out.string());
    std::printf("%uch %uHz v%x, %zu frames, delay %u, padding %u, %zu CRC failures, %zu decode failures\n", pcm.header.channels,
                pcm.header.sample_rate, pcm.header.version, pcm.frames, pcm.header.encoder_delay, pcm.header.encoder_padding,
                pcm.crc_failures, pcm.decode_failures);
    return pcm.crc_failures || pcm.decode_failures ? 1 : 0;
}

// One HCA stream: `size` bytes at `offset` inside archive entry `entry`, or inside the memory AWB of that ACB.
struct DecodeJob { const CpkEntry* entry; bool in_acb; uint64_t offset, size; uint32_t id; };

int decode_verify(const fs::path& cpk_path, size_t sample) {
    auto t0 = std::chrono::steady_clock::now();
    CpkArchive cpk(cpk_path);
    std::vector<DecodeJob> jobs;
    size_t setup_failures = 0;
    auto add_bank = [&](const CpkEntry& e, bool in_acb, const RangeReader& rd, uint64_t size) {
        Afs2Header h = read_afs2(rd, size);
        for (const auto& s : h.streams)
            if (s.size >= 8 && is_hca(rd(s.offset, 8))) jobs.push_back({&e, in_acb, s.offset, s.size, s.id});
    };
    for (const auto& e : cpk.entries()) {
        std::string ext = fs::path(e.name).extension().string();
        try {
            if (ext == ".acb") {
                auto bytes = cpk.read(e);
                Acb acb(bytes);
                if (!acb.memory_awb().empty()) add_bank(e, true, span_reader(acb.memory_awb()), acb.memory_awb().size());
            } else if (ext == ".awb") {
                add_bank(e, false, [&cpk, &e](uint64_t off, size_t n) { return cpk.read_range(e, off, n); }, e.stored_size);
            }
        } catch (const std::exception& ex) {
            ++setup_failures;
            std::fprintf(stderr, "FAIL %s: %s\n", e.path().c_str(), ex.what());
        }
    }
    if (sample && sample < jobs.size()) {
        std::vector<DecodeJob> picked;
        for (size_t i = 0; i < sample; ++i) picked.push_back(jobs[i * jobs.size() / sample]);
        jobs.swap(picked);
    }

    std::mutex mu;
    std::atomic<size_t> next{0};
    size_t frames = 0, crc = 0, bad = 0, failed_streams = 0;
    double seconds = 0;
    std::map<std::string, size_t> layouts;  // "v2.0 2ch" -> streams
    auto worker = [&] {
        CpkArchive local(cpk_path);  // own file handle per thread
        const CpkEntry* acb_entry = nullptr;  // last ACB parsed by this thread (jobs come grouped per file)
        std::vector<uint8_t> acb_bytes;
        std::optional<Acb> acb;
        for (size_t i; (i = next++) < jobs.size();) {
            const DecodeJob& j = jobs[i];
            std::string where = j.entry->path() + " stream " + std::to_string(j.id);
            try {
                std::vector<uint8_t> bytes;
                if (j.in_acb) {
                    if (acb_entry != j.entry) {
                        acb.reset();
                        acb_bytes = local.read(*j.entry);
                        acb.emplace(acb_bytes);
                        acb_entry = j.entry;
                    }
                    bytes = span_reader(acb->memory_awb())(j.offset, size_t(j.size));
                } else if (j.entry->compressed()) {
                    throw std::runtime_error("compressed AWB");  // none in DCP; would need the whole entry in memory
                } else {
                    bytes = local.read_range(*j.entry, j.offset, size_t(j.size));
                }
                HcaPcm pcm = decode_hca(bytes, false);
                char layout[32];
                std::snprintf(layout, sizeof layout, "v%x.%x %uch", pcm.header.version >> 8, (pcm.header.version >> 4) & 15,
                              pcm.header.channels);
                std::lock_guard lock(mu);
                frames += pcm.frames;
                crc += pcm.crc_failures;
                bad += pcm.decode_failures;
                seconds += double(pcm.playable_samples()) / pcm.header.sample_rate;
                ++layouts[layout];
                if (pcm.crc_failures || pcm.decode_failures) {
                    ++failed_streams;
                    std::fprintf(stderr, "FAIL %s: %zu/%zu frames CRC, %zu decode\n", where.c_str(), pcm.crc_failures,
                                 pcm.frames, pcm.decode_failures);
                }
            } catch (const std::exception& ex) {
                std::lock_guard lock(mu);
                ++failed_streams;
                std::fprintf(stderr, "FAIL %s: %s\n", where.c_str(), ex.what());
            }
        }
    };
    std::vector<std::thread> pool;
    unsigned n = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
    for (unsigned i = 0; i < n; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    std::string lay;
    for (const auto& [k, v] : layouts) lay += (lay.empty() ? "" : ", ") + k + ":" + std::to_string(v);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%zu streams [%s], %zu frames (%.1f h audio), %zu CRC failures, %zu decode failures, %zu failed streams, "
                "%zu setup failures, %.1f s\n",
                jobs.size(), lay.c_str(), frames, seconds / 3600, crc, bad, failed_streams, setup_failures, secs);
    return crc || bad || failed_streams || setup_failures ? 1 : 0;
}

// Decoding needs the MDCT window from the user's eboot: $DCP_EBOOT, else eboot.bin next to the given file.
void load_window(const fs::path& near) {
    const char* env = std::getenv("DCP_EBOOT");
    fs::path eboot = env && *env ? fs::path(env) : near.parent_path() / "eboot.bin";
    hca_load_window_from_eboot(dcp::formats::EbootImage{eboot});
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1];
    try {
        if (cmd == "list") return list(fs::path(argv[2]), argc >= 4 ? std::optional<fs::path>(argv[3]) : std::nullopt);
        if (cmd == "verify") return verify(CpkArchive{fs::path(argv[2])});
        if (cmd == "wav" || cmd == "decode-verify" || cmd == "decode") load_window(fs::path(argv[2]));
        if (cmd == "wav" && argc == 6) return wav(CpkArchive{fs::path(argv[2])}, argv[3], argv[4], fs::path(argv[5]));
        if (cmd == "decode-verify") return decode_verify(fs::path(argv[2]), argc >= 4 ? std::stoul(argv[3]) : 0);
        if (cmd == "decode" && argc == 4) return decode_file(fs::path(argv[2]), fs::path(argv[3]));
        return usage();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
