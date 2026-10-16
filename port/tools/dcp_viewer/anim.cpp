#include "anim.h"

#include <algorithm>
#include <cmath>

namespace dcp::viewer {

using formats::Mbs;
using formats::MbsFrame;
using formats::MbsTransform;

namespace {

constexpr uint32_t kFrameJump = 0x04, kFrameStop = 0x08, kFrameSkip = 0x40;

float catmull(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (3 * p1 - p0 - 3 * p2 + p3) * t3);
}

std::array<float, 8> tf_vec(const MbsTransform& t) {
    return {t.pos[0], t.pos[1], t.pos[2], t.rot[0], t.rot[1], t.rot[2], t.scale[0], t.scale[1]};
}

// Tint u32: alpha in the high byte (0xaaffffff seen as fades), so memory order R,G,B,A.
std::array<float, 4> tint_vec(uint32_t c) {
    return {float(c & 0xff), float(c >> 8 & 0xff), float(c >> 16 & 0xff), float(c >> 24)};
}

}  // namespace

void MbsPlayer::set_motion(int motion) {
    layers_.clear();
    motion_ = -1;
    if (motion < 0 || size_t(motion) >= m_->motions.size()) return;
    motion_ = motion;
    const auto& mo = m_->motions[motion];
    for (int i = 0; i < mo.track_count; ++i) {
        Layer l;
        l.track = mo.first_track + i;
        const auto& tr = m_->tracks[l.track];
        // Track byte 0x13 = signed start frame (Layer_Init); negative treated as 0.
        l.idx = tr.frame_count ? std::clamp<int>(tr.start, 0, tr.frame_count - 1) : 0;
        layers_.push_back(l);
    }
}

void MbsPlayer::advance(double ticks) {
    for (auto& l : layers_) {
        const auto& tr = m_->tracks[l.track];
        if (l.held || !tr.frame_count) continue;
        l.t += ticks;
        for (int guard = 0; guard < 256; ++guard) {
            const MbsFrame& f = m_->frames[tr.first_frame + l.idx];
            if (l.t < f.duration) break;
            l.t -= f.duration;
            // GUESS (flag meanings med-confidence in the map): 0x08 holds the frame, 0x04 jumps to +0xc, else next; wrap = loop.
            if (f.flags & kFrameStop) { l.held = true; l.t = f.duration; break; }
            if ((f.flags & kFrameJump) && f.jump < tr.frame_count) l.idx = f.jump;
            else if (++l.idx >= tr.frame_count) l.idx = 0;
        }
    }
}

void MbsPlayer::emit(const Affine& world, const std::array<uint8_t, 4>& tint, std::vector<DrawPart>& out) const {
    const Mbs& m = *m_;
    for (const auto& l : layers_) {
        const auto& tr = m.tracks[l.track];
        if (!tr.frame_count) continue;
        auto fr = [&](int i) -> const MbsFrame& { return m.frames[tr.first_frame + std::clamp(i, 0, tr.frame_count - 1)]; };
        const MbsFrame& cur = fr(l.idx);
        if (cur.flags & kFrameSkip) continue;  // GUESS: 0x40 = inactive frame, nothing drawn
        float t = cur.duration ? float(std::min(l.t / cur.duration, 1.0)) : 0.f;
        if (l.held) t = 0;
        // Time curve +0xf: 1 = linear; other values (2..12 seen) GUESS: smoothstep.
        float te = cur.time_curve == 1 ? t : t * t * (3 - 2 * t);
        const MbsFrame &prev = fr(l.idx - 1), &next = fr(l.idx + 1), &next2 = fr(l.idx + 2);
        auto p0 = tf_vec(m.transforms[prev.transform]), p1 = tf_vec(m.transforms[cur.transform]),
             p2 = tf_vec(m.transforms[next.transform]), p3 = tf_vec(m.transforms[next2.transform]);
        std::array<float, 8> v;
        for (int k = 0; k < 8; ++k)
            v[k] = cur.interp == 1 ? p1[k] + (p2[k] - p1[k]) * te : catmull(p0[k], p1[k], p2[k], p3[k], te);
        auto c1 = tint_vec(m.transforms[cur.transform].tint), c2 = tint_vec(m.transforms[next.transform].tint);
        float ftint[4];
        for (int k = 0; k < 4; ++k) ftint[k] = (c1[k] + (c2[k] - c1[k]) * te) / 255.f * tint[k] / 255.f;

        // Local matrix: T(pos) * R(rot z) * S(scale); rot x/y GUESS: orthographic 3D tilt, i.e. scale by their cosines.
        float sx = v[6] * std::cos(v[4]), sy = v[7] * std::cos(v[3]);
        float cs = std::cos(v[5]), sn = std::sin(v[5]);
        Affine local{cs * sx, sn * sx, -sn * sy, cs * sy, v[0], v[1]};
        Affine w = world * local;

        const auto& cell = m.cells[cur.cell];
        const auto* morph = (cur.morph && next.cell != cur.cell && m.cells[next.cell].part_count == cell.part_count)
                                ? &m.cells[next.cell] : nullptr;  // morph 1/2 approximated as 2-cell lerp
        bool ignore_colors = cell.flags & 8;
        for (uint32_t pi = 0; pi < cell.part_count; ++pi) {
            const auto& p = m.parts[cell.first_part + pi];
            if (p.flags & 2) continue;  // hidden
            const auto& pos = m.positions[p.pos];
            const auto* pos2 = morph ? &m.positions[m.parts[morph->first_part + pi].pos] : nullptr;
            const auto& col = m.colors[p.color];
            DrawPart d;
            d.texture = p.untextured() ? -1 : p.texture;
            d.blend = p.blend;
            for (int k = 0; k < 6; ++k) {
                float x = pos.v[k][0], y = pos.v[k][1];
                if (pos2) { x += (pos2->v[k][0] - x) * te; y += (pos2->v[k][1] - y) * te; }
                auto wp = w.apply(x, y);
                DrawVert& dv = d.v[k];
                dv.x = wp[0]; dv.y = wp[1];
                dv.u = p.untextured() ? 0 : m.uvs[p.uv].v[k][0];
                dv.v = p.untextured() ? 0 : m.uvs[p.uv].v[k][1];
                for (int ch = 0; ch < 4; ++ch) {
                    float base = ignore_colors ? 255.f : col.rgba[k][ch];
                    dv.rgba[ch] = uint8_t(std::clamp(base * ftint[ch] + 0.5f, 0.f, 255.f));
                }
            }
            out.push_back(d);
        }
    }
}

}  // namespace dcp::viewer
