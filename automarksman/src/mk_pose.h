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
        yaw = wrap_pi(yaw + wrap_pi(y - yaw) * a); pitch += (p - pitch) * a;      // wrapped: 0.4.0 showed +360
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

// Is the bow's live pose one to steer with?  While the bow is raised, an arrow is nocked or an animation
// blends, the "arrow" joint points far away from the view (round 4: -28 to -66 deg pitch, +152 deg yaw) and
// jumps between frames.  Steering on that swung the camera up (the "jolt").  A drawn bow points within a few
// degrees of the view and moves slowly (sway: a few deg/s).  The gate passes a pose only after it has looked
// like that for `settle` seconds; a few bad frames once settled are skipped (no turn), more re-start the wait.
struct PoseGate {
    enum Why { OK, NO_POSE, FAR_FROM_VIEW, JUMPING, SETTLING };
    float maxOff = 0.175f;       // rad (10 deg): bow vs camera, beyond this the pose is not a drawn bow
    float maxRate = 0.44f;       // rad/s (25 deg/s): how fast the bow-vs-camera offset may change while settling
    float maxStep = 0.05f;       // rad (3 deg) in one frame once settled (the camera turning moves it a little)
    float settle = 0.2f;         // s of plausible poses before steering
    float grace = 0.1f;          // s of bad poses tolerated once settled
    float py = 0, pp = 0; bool havePrev = false, ready = false;
    float goodFor = 0, badFor = 0, waited = 0;   // waited: time since the gate last was ready (for the log)
    void reset() { havePrev = ready = false; goodFor = badFor = waited = 0; }
    Why update(bool ok, float offY, float offP, float dt) {
        Why w = OK;
        if (!ok) w = NO_POSE;
        else if (std::fabs(offY) > maxOff || std::fabs(offP) > maxOff) w = FAR_FROM_VIEW;
        else if (havePrev && dt > 0.f) {
            float j = std::sqrt(wrap_pi(offY - py) * wrap_pi(offY - py) + (offP - pp) * (offP - pp));
            if (j > (ready ? maxStep : maxRate * dt + 0.002f)) w = JUMPING;
        }
        if (ok) { py = offY; pp = offP; havePrev = true; } else havePrev = false;
        if (!ready) waited += dt;
        if (w == OK) {
            badFor = 0; goodFor += dt;
            if (!ready && goodFor >= settle) ready = true;
            return ready ? OK : SETTLING;
        }
        goodFor = 0;
        if (ready) { badFor += dt; if (badFor > grace) { ready = false; waited = 0; } }
        return w;
    }
};
inline const char* gate_why(PoseGate::Why w) {
    switch (w) { case PoseGate::OK: return "ok"; case PoseGate::NO_POSE: return "not readable"; case PoseGate::FAR_FROM_VIEW: return "far from the view (bow not drawn yet)";
                 case PoseGate::JUMPING: return "jumping between frames (animation)"; default: return "settling"; }
}

// Comfortable camera motion.  The camera is steered on the tracked bow offset (SwayTrack), never on each
// wobble; the impact marker and the log keep the raw pose, so they show where an arrow loosed now would
// really go.  On top of the rate cap of Steer:
//  - a soft dead zone: the part of the aim error within `tol` metres of the target (at its distance) is not
//    corrected, so the camera does not chase the last centimetres; beyond it the correction grows smoothly
//    from zero (no on/off switching, which would itself shake),
//  - an acceleration limit: the turn rate changes smoothly (no jolt when a target is taken or the sway turns).
struct Glide {
    float accel = 10.f;          // rad/s^2 (~570 deg/s^2)
    float tol = 0.02f;           // m at the target
    float vy = 0, vp = 0;
    void reset() { vy = vp = 0; }
    // Shrinks the error by the dead zone.  Returns false when it is inside it (on the head).
    bool shrink(float& errYaw, float& errPitch, float dist) const {
        float a = std::sqrt(errYaw * errYaw + errPitch * errPitch), z = tol / std::max(1.f, dist);
        if (a <= z) { errYaw = errPitch = 0; return false; }
        float k = (a - z) / a; errYaw *= k; errPitch *= k; return true;
    }
    // Limit the change of this frame's turn rate.  dy/dp: the turn Steer wants this frame (rad).
    void apply(float& dy, float& dp, float dt) {
        if (dt <= 0.f) { dy = dp = 0; return; }
        float wy = dy / dt, wp = dp / dt, cy = wy - vy, cp = wp - vp, c = std::sqrt(cy * cy + cp * cp), cap = accel * dt;
        if (c > cap) { cy *= cap / c; cp *= cap / c; }
        vy += cy; vp += cp; dy = vy * dt; dp = vp * dt;
    }
};

// When a check trips (the camera does not come closer, the pose went bad), the assist pauses and starts over
// instead of switching itself off.  It only gives up for the session when the same trip repeats many times in
// a row with a settled, plausible bow pose (then the game is not doing what the mod expects).
struct Trips {
    int inRow = 0, total = 0, limit = 6;
    bool trip(bool poseSettled) { total++; if (poseSettled) inRow++; return inRow >= limit; }   // true: give up
    void success() { inRow = 0; }
};

// Back-off for faults while calling the game's pose functions: pause 1, 2, 4 ... 60 s, never permanent unless
// they keep faulting (`limit` in total).
struct Backoff {
    int inRow = 0, total = 0, limit = 50; double until = 0;
    bool blocked(double now) const { return now < until; }
    bool fault(double now) { total++; inRow++; until = now + std::min(60.0, (double)(1 << std::min(inRow - 1, 6))); return total >= limit; }
    void ok() { inRow = 0; }
};

// The bow's offset as the camera follows it: a critically damped tracking filter (position and rate) with
// bandwidth 1/tau.  The bow's slow sway (a few tenths of a Hz) is followed with little lag, and its rate is a
// smooth feed-forward; the joint's tremor and frame-to-frame noise (several Hz) are filtered out, so the
// camera does not shake with them.  0.4.0 followed each frame's offset (tau 0.03 s) and fed its raw rate
// forward, which turned every wobble into camera shake.
struct SwayTrack {
    float yaw = 0, pitch = 0, vy = 0, vp = 0; bool have = false;
    void reset() { have = false; vy = vp = 0; }
    void update(float offY, float offP, float dt, float tau) {
        if (!have || dt <= 0.f) { if (!have) { yaw = offY; pitch = offP; vy = vp = 0; have = true; } return; }
        float w = 1.f / std::max(0.01f, tau); if (w * dt > 0.5f) w = 0.5f / dt;           // stable at low frame rates
        float ey = wrap_pi(offY - yaw), ep = offP - pitch;
        vy += w * w * ey * dt; vp += w * w * ep * dt;
        yaw = wrap_pi(yaw + (vy + 2.f * w * ey) * dt); pitch += (vp + 2.f * w * ep) * dt;
        vy = std::max(-1.f, std::min(1.f, vy)); vp = std::max(-1.f, std::min(1.f, vp));
    }
};

// One frame of steering on the live bow pose (round 5), shared by the mod and marksim.
//   pose(): feeds this frame's bow direction through the gate and the smoothed offsets.
//   turn(): the camera turn for a wanted launch direction (from solve()), on the smoothed offset, with the
//           target-motion feed-forward, Steer's rate cap and gain, the dead zone and the acceleration limit.
struct AimCtl {
    PoseGate gate; SwayTrack off; Glide glide; Steer st;
    float swayTau = 0.12f;       // s: 1/bandwidth of the sway tracker (larger = calmer camera, follows sway less)
    float ffY = 0, ffP = 0, lastSY = 0, lastSP = 0; bool haveS = false;
    void reset() { gate.reset(); off.reset(); glide.reset(); ffY = ffP = 0; haveS = false; }
    void hands_on() { gate.havePrev = false; glide.reset(); haveS = false; }
    PoseGate::Why pose(bool ok, const V3& bowDir, const V3& camFwd, float dt, float& rawY, float& rawP) {
        rawY = rawP = 0;
        if (ok) { rawY = wrap_pi(yaw_of(bowDir) - yaw_of(camFwd)); rawP = pitch_of(bowDir) - pitch_of(camFwd); }
        PoseGate::Why w = gate.update(ok, rawY, rawP, dt);
        if (w == PoseGate::OK || w == PoseGate::SETTLING) off.update(rawY, rawP, dt, w == PoseGate::SETTLING ? 0.05f : swayTau);
        if (w != PoseGate::OK) { glide.reset(); haveS = false; }
        return w;
    }
    // offY/offP: the bow-vs-camera offset to steer with (off.yaw/off.pitch, or measured ones without a pose);
    // offVY/offVP: its rate (off.vy/off.vp; 0 for fixed offsets).
    // Returns false when the aim is inside the dead zone (on the head: only the sway is followed).
    bool turn(const V3& solDir, const V3& camFwd, float offY, float offP, float offVY, float offVP, float dist, float rateScale, float dt,
              float& dy, float& dp, float& ey, float& ep) {
        float sy = yaw_of(solDir), sp = pitch_of(solDir);
        ey = wrap_pi(sy - offY - yaw_of(camFwd)); ep = sp - offP - pitch_of(camFwd);
        float fy = 0, fp = 0;
        if (haveS && dt > 0.f) {
            float vy = wrap_pi(sy - lastSY) / dt, vp = (sp - lastSP) / dt;
            if (std::fabs(vy) > 3.f || std::fabs(vp) > 3.f) vy = vp = 0;
            float k = std::min(1.f, dt / 0.1f); ffY += (vy - ffY) * k; ffP += (vp - ffP) * k; fy = ffY; fp = ffP;
        }
        fy -= offVY; fp -= offVP;                                    // the bow drifting up turns the camera down
        lastSY = sy; lastSP = sp; haveS = true;
        Steer s2 = st; s2.maxRate *= rateScale;
        float cy = ey, cp = ep; bool outside = glide.shrink(cy, cp, dist);
        s2.step(cy, cp, fy, fp, dt, dy, dp);
        glide.apply(dy, dp, dt);
        return outside;
    }
};

}  // namespace mk
