#include "acb.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace dcp::formats {
namespace {

enum Kind { kWaveform = 1, kSynth = 2, kSequence = 3 };
constexpr int kMaxDepth = 16;  // guards against reference cycles in malformed files

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

bool is_utf(std::span<const uint8_t> d) { return d.size() >= 8 && std::memcmp(d.data(), "@UTF", 4) == 0; }

std::optional<UtfTable> nested(const UtfTable& t, std::string_view col) {
    auto d = t.data(0, col);
    if (!is_utf(d)) return std::nullopt;
    return UtfTable(d);
}

}  // namespace

const char* Acb::codec_name(uint8_t encode_type) {
    switch (encode_type) {
        case 0: return "ADX";
        case 2: return "HCA";
        case 6: return "HCA-MX";
        default: return "other";
    }
}

Acb::Acb(std::span<const uint8_t> bytes) : header_(bytes) {
    if (header_.name() != "Header" || header_.rows() != 1) throw std::runtime_error("acb: not an ACB header table");
    name_ = header_.str(0, "Name");
    version_ = header_.str(0, "VersionString");

    if (auto w = nested(header_, "WaveformTable")) {
        // Older ACBs have a single "Id" column whose meaning depends on Streaming.
        bool legacy_id = !w->find_column("MemoryAwbId") && w->find_column("Id");
        for (uint32_t r = 0; r < w->rows(); ++r) {
            AcbWaveform f;
            f.encode_type = uint8_t(w->u64(r, "EncodeType"));
            f.streaming = uint8_t(w->u64(r, "Streaming"));
            f.channels = uint8_t(w->u64(r, "NumChannels"));
            f.loop = w->u64(r, "LoopFlag") != 0;
            f.sample_rate = uint32_t(w->u64(r, "SamplingRate"));
            f.num_samples = uint32_t(w->u64(r, "NumSamples"));
            if (legacy_id) {
                uint16_t id = uint16_t(w->u64(r, "Id", 0xFFFF));
                (f.streaming ? f.stream_awb_id : f.memory_awb_id) = id;
            } else {
                f.memory_awb_id = uint16_t(w->u64(r, "MemoryAwbId", 0xFFFF));
                f.stream_awb_id = uint16_t(w->u64(r, "StreamAwbId", 0xFFFF));
                f.stream_awb_port = uint16_t(w->u64(r, "StreamAwbPortNo", 0xFFFF));
            }
            waveforms_.push_back(f);
        }
    }
    if (auto s = nested(header_, "StreamAwbHash"))
        for (uint32_t r = 0; r < s->rows(); ++r) stream_awbs_.push_back(s->str(r, "Name"));

    // Copy of the streaming AWB's AFS2 header; newer ACBs wrap it in a one-row "StreamAwbHeader" table.
    auto sh = header_.data(0, "StreamAwbAfs2Header");
    if (is_utf(sh)) {
        UtfTable t(sh);
        if (t.rows()) {
            auto inner = t.data(0, "Header");
            stream_awb_header_.assign(inner.begin(), inner.end());
        }
    } else if (sh.size() >= 4 && std::memcmp(sh.data(), "AFS2", 4) == 0) {
        stream_awb_header_.assign(sh.begin(), sh.end());
    }

    seq_ = nested(header_, "SequenceTable");
    synth_ = nested(header_, "SynthTable");
    track_ = nested(header_, "TrackTable");
    events_ = nested(header_, "TrackEventTable");
    if (!events_) events_ = nested(header_, "CommandTable");  // pre-TrackEventTable versions

    auto cue_t = nested(header_, "CueTable");
    if (!cue_t) return;
    cues_.resize(cue_t->rows());
    for (uint32_t r = 0; r < cue_t->rows(); ++r)
        cue_refs_.push_back({uint32_t(cue_t->u64(r, "ReferenceType")), uint32_t(cue_t->u64(r, "ReferenceIndex"))});
    for (uint32_t r = 0; r < cue_t->rows(); ++r) {
        AcbCue& c = cues_[r];
        c.index = r;
        c.id = uint32_t(cue_t->u64(r, "CueId"));
        c.length_ms = uint32_t(cue_t->u64(r, "Length"));
        walk_cue(r, c, 0);
    }
    if (auto n = nested(header_, "CueNameTable"))
        for (uint32_t r = 0; r < n->rows(); ++r) {
            uint32_t idx = uint32_t(n->u64(r, "CueIndex"));
            if (idx >= cues_.size()) throw std::runtime_error("acb: cue name index out of range");
            cues_[idx].name = n->str(r, "CueName");
        }
}

void Acb::walk_cue(uint32_t index, AcbCue& cue, int depth) {
    if (index >= cue_refs_.size()) throw std::runtime_error("acb: cue index out of range");
    auto [type, ref] = cue_refs_[index];
    if (type >= kWaveform && type <= kSequence)
        walk(int(type), ref, cue, depth);
    else
        skip("cue reference type " + std::to_string(type));
}

void Acb::walk(int kind, uint32_t index, AcbCue& cue, int depth) {
    if (depth > kMaxDepth) throw std::runtime_error("acb: reference chain too deep");
    switch (kind) {
        case kWaveform: {
            if (index >= waveforms_.size()) throw std::runtime_error("acb: waveform index out of range");
            if (std::find(cue.waveforms.begin(), cue.waveforms.end(), index) == cue.waveforms.end())
                cue.waveforms.push_back(uint16_t(index));
            return;
        }
        case kSynth: {
            if (!synth_ || index >= synth_->rows()) throw std::runtime_error("acb: synth index out of range");
            auto items = synth_->data(index, "ReferenceItems");
            for (size_t i = 0; i + 4 <= items.size(); i += 4) {
                uint16_t type = be16(&items[i]), idx = be16(&items[i + 2]);
                if (type == 0) continue;
                if (type <= kSequence)
                    walk(type, idx, cue, depth + 1);
                else
                    skip("synth item type " + std::to_string(type));
            }
            return;
        }
        case kSequence: {
            if (!seq_ || index >= seq_->rows()) throw std::runtime_error("acb: sequence index out of range");
            auto tracks = seq_->data(index, "TrackIndex");
            uint32_t ntracks = uint32_t(seq_->u64(index, "NumTracks", tracks.size() / 2));
            for (uint32_t t = 0; t < ntracks && 2 * t + 2 <= tracks.size(); ++t) {
                uint16_t ti = be16(&tracks[2 * t]);
                if (!track_ || ti >= track_->rows()) throw std::runtime_error("acb: track index out of range");
                uint32_t ev = uint32_t(track_->u64(ti, "EventIndex", 0xFFFF));
                if (ev == 0xFFFF) continue;
                if (!events_ || ev >= events_->rows()) throw std::runtime_error("acb: event index out of range");
                // TLV list: u16 opcode, u8 size, payload. Only "note on" opcodes reference playable items.
                auto cmd = events_->data(ev, "Command");
                for (size_t p = 0; p + 3 <= cmd.size();) {
                    uint16_t op = be16(&cmd[p]);
                    size_t sz = cmd[p + 2];
                    if (p + 3 + sz > cmd.size()) throw std::runtime_error("acb: truncated track command");
                    if ((op == 2000 || op == 2003) && sz >= 4) {
                        uint16_t type = be16(&cmd[p + 3]), idx = be16(&cmd[p + 5]);
                        if (type == kSynth || type == kSequence)
                            walk(type, idx, cue, depth + 1);
                        else
                            skip("note-on target type " + std::to_string(type));
                    }
                    p += 3 + sz;
                }
            }
            return;
        }
    }
}

std::span<const uint8_t> Acb::memory_awb() const {
    auto d = header_.data(0, "AwbFile");
    return d.size() >= 4 && std::memcmp(d.data(), "AFS2", 4) == 0 ? d : std::span<const uint8_t>{};
}

}  // namespace dcp::formats
