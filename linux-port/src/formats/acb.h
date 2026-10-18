// CRI ACB (.acb) cue sheet: an @UTF "Header" table whose data columns hold nested @UTF tables.
// Resolves cue name -> cue id -> waveform(s) -> memory AWB (embedded AFS2) or streaming AWB (.awb) stream id.
//
// Reference chain followed: Cue.ReferenceType 1 (Waveform) / 2 (Synth) / 3 (Sequence);
// Sequence.TrackIndex -> Track.EventIndex -> TrackEvent (or legacy Command) table TLV commands 2000/2003
// ("note on": target 2 = Synth, 3 = Sequence); Synth.ReferenceItems (type 1 Waveform, 2 Synth, 3 Sequence).
// Anything else is counted in skipped(). DCP uses one such kind: synth item type 7 (561 items, SE banks only). Its
// index fits both the CueTable and SynthTable, but either reading yields self-references, so it is left to the
// eboot's CRI runtime to settle.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "utf_table.h"

namespace dcp::formats {

struct AcbWaveform {
    uint8_t encode_type = 0;  // 2 = HCA, 6 = HCA-MX (also an HCA bitstream); see Acb::codec_name
    uint8_t streaming = 0;    // 0 memory, 1 streaming, 2 memory prefetch + streaming
    uint8_t channels = 0;
    bool loop = false;
    uint32_t sample_rate = 0, num_samples = 0;
    uint16_t memory_awb_id = 0xFFFF, stream_awb_id = 0xFFFF, stream_awb_port = 0xFFFF;
    bool in_memory() const { return streaming != 1; }
    bool in_stream() const { return streaming != 0; }
};

struct AcbCue {
    uint32_t id = 0;     // CueId (what the game asks for)
    uint32_t index = 0;  // row in the CueTable
    std::string name;    // from CueNameTable; empty if the cue has no name
    uint32_t length_ms = 0;
    std::vector<uint16_t> waveforms;  // indexes into Acb::waveforms(), unique, in reference order
};

class Acb {
public:
    explicit Acb(std::span<const uint8_t> bytes);  // throws on malformed input

    const std::string& name() const { return name_; }
    const std::string& version_string() const { return version_; }
    const std::vector<AcbCue>& cues() const { return cues_; }
    const std::vector<AcbWaveform>& waveforms() const { return waveforms_; }

    std::span<const uint8_t> memory_awb() const;         // embedded AFS2 (AwbFile), empty if none
    // Copy of the streaming AWB's AFS2 header (lets the AWB be validated against its ACB), empty if none.
    std::span<const uint8_t> stream_awb_header() const { return stream_awb_header_; }
    const std::vector<std::string>& stream_awb_names() const { return stream_awbs_; }

    // Reference kinds met while resolving cues but not followed, with occurrence counts.
    const std::map<std::string, size_t>& skipped() const { return skipped_; }

    static const char* codec_name(uint8_t encode_type);

private:
    void walk_cue(uint32_t index, AcbCue& cue, int depth);
    void walk(int kind, uint32_t index, AcbCue& cue, int depth);
    void skip(const std::string& what) { ++skipped_[what]; }

    UtfTable header_;
    std::optional<UtfTable> seq_, synth_, track_, events_;  // needed while resolving the reference chain

    std::string name_, version_;
    std::vector<AcbCue> cues_;
    std::vector<AcbWaveform> waveforms_;
    std::vector<std::string> stream_awbs_;
    std::vector<uint8_t> stream_awb_header_;
    std::map<std::string, size_t> skipped_;
    std::vector<std::pair<uint32_t, uint32_t>> cue_refs_;  // CueTable (ReferenceType, ReferenceIndex) per row
};

}  // namespace dcp::formats
