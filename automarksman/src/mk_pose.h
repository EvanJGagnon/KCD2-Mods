// mk_pose.h -- Theatrical AutoMarksman: the bow's live launch pose, and how a real arrow is judged.
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Plain C++17 (tested by tools/marksim.cpp).
//
// How the game launches the player's arrow (build 15693, docs/NOTES.md "Launch pose"): from the bow's "arrow"
// attachment joint, along that joint's -Z axis (up = its +Y axis), at the speed the RPG formula gives for the
// bow, the arrow and the draw.  The arrow entity is placed so that its "slt_0" helper sits on the joint, so
// the launch point is the joint minus that helper's offset in the joint frame (dir x up, dir, up).  There is
// no camera ray in it: the arrow leaves wherever the bow points, which is not where the camera points.
#pragma once
#include "mk_ballistics.h"

namespace mk {

inline V3 cross(const V3& a, const V3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

struct LivePose {
    V3 pos, up, dir;              // joint position, its +Y and -Z axes (world)
    bool ok = false;
};

// Offset of the arrow's own helper, in the joint frame (side = dir x up, dir, up).
inline V3 slot_shift(const V3& dir, const V3& up, const V3& t) { V3 s = cross(dir, up); return s * t.x + dir * t.y + up * t.z; }
inline V3 slot_learn(const V3& dir, const V3& up, const V3& jointMinusLaunch) {
    V3 s = cross(dir, up); return {jointMinusLaunch.dot(s), jointMinusLaunch.dot(dir), jointMinusLaunch.dot(up)};
}
inline V3 launch_point(const LivePose& p, const V3& slot) { return p.pos - slot_shift(p.dir, p.up, slot); }

// Yaw/pitch of the bow relative to the camera (what the arrow does differently from the view).
struct PoseOffset {
    float yaw = 0, pitch = 0; bool have = false;
    // tau: smoothing time constant in seconds (the bow sways a little every frame; steering follows the trend).
    void update(const V3& bowDir, const V3& camFwd, float dt, float tau) {
        float y = wrap_pi(yaw_of(bowDir) - yaw_of(camFwd)), p = pitch_of(bowDir) - pitch_of(camFwd);
        if (!have || dt <= 0.f) { yaw = y; pitch = p; have = true; return; }
        float a = std::min(1.f, dt / std::max(1e-3f, tau));
        yaw += wrap_pi(y - yaw) * a; pitch += (p - pitch) * a;
    }
};

// Where a sampled flight crosses horizontal distance d from `origin` measured along the horizontal heading `head`
// (unit).  Linear interpolation between samples.  t returns the time of the crossing.
inline bool crossing(const ShotTrack& s, const V3& origin, const V3& head, float d, V3& at, float& t) {
    float prev = 0; V3 pp = origin; float pt = 0;
    for (int i = 0; i < s.n; i++) {
        V3 r = s.p[i] - origin; float x = r.x * head.x + r.y * head.y;
        if (x >= d && i > 0 && x > prev) {
            float f = (d - prev) / (x - prev);
            at = pp + (s.p[i] - pp) * f; t = pt + (s.t[i] - pt) * f; return true;
        }
        prev = x; pp = s.p[i]; pt = s.t[i];
    }
    return false;
}

// Signed miss of `arrow` against `target` as seen along the horizontal heading `head`: up (+ = above) and
// right (+ = right of it).
inline void miss_parts(const V3& head, const V3& arrow, const V3& target, float& up, float& right) {
    V3 d = arrow - target; V3 r{head.y, -head.x, 0};
    up = d.z; right = d.x * r.x + d.y * r.y;
}
inline V3 heading(const V3& v) { V3 h{v.x, v.y, 0}; float l = h.len(); return l > 1e-6f ? h * (1.f / l) : V3{0, 1, 0}; }

// The game's speed formula is called live; what is learnt from real arrows is only how far reality is from it
// (an effect the formula call does not see).  Shots that differ a lot are reported, not learnt from.
struct SpeedCheck {
    float ratio = 1.f; int n = 0;
    bool add(float actual, float predicted) {           // true: used (within 6%)
        if (!(predicted > 1.f && actual > 1.f)) return false;
        float r = actual / predicted;
        if (std::fabs(r - 1.f) > 0.06f) return false;
        ratio += (r - ratio) * (n < 3 ? 0.5f : 0.25f); n++;
        ratio = std::max(0.9f, std::min(1.1f, ratio));
        return true;
    }
};

}  // namespace mk
