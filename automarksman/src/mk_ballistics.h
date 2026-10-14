// mk_ballistics.h -- Theatrical AutoMarksman: arrow flight model, aim solver and calibration fits.
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Plain C++17, no game or Windows dependencies, so tools/marksim.cpp can test it on any machine.
// World frame as in CryEngine: z up, yaw 0 looks along +y, positive yaw turns left (towards -x),
// positive pitch looks up.  Angles in radians.
//
// Flight model: a point with launch velocity v0, constant gravity g (downwards) and linear air drag k
// (dv/dt = -g*z - k*v), which is what a CryEngine physics particle integrates.  Closed form:
//   E(t) = (1 - e^-kt) / k        (= t when k = 0)
//   horizontal distance  = vh * E(t)
//   height               = vz * E(t) - g * (t - E(t)) / k      (= vz*t - g*t*t/2 when k = 0)
// The game's own values for v0, g and k are not assumed: AutoMarksman measures them from Henry's real
// arrows (launch velocity from CProjectile::Launch, g and k from the arrow's flight) - see Calib.
#pragma once
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace mk {

struct V3 {
    float x = 0, y = 0, z = 0;
    V3() = default;
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
    V3 operator+(const V3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(const V3& o) const { return x * o.x + y * o.y + z * o.z; }
    float len() const { return std::sqrt(x * x + y * y + z * z); }
    float hlen() const { return std::sqrt(x * x + y * y); }
    V3 norm() const { float l = len(); return l > 1e-9f ? *this * (1.f / l) : V3{0, 1, 0}; }
};

static const float PI = 3.14159265358979f;
inline float wrap_pi(float a) { while (a > PI) a -= 2 * PI; while (a < -PI) a += 2 * PI; return a; }
inline float yaw_of(const V3& d) { return std::atan2(-d.x, d.y); }
inline float pitch_of(const V3& d) { float l = d.len(); return l > 1e-9f ? std::asin(std::max(-1.f, std::min(1.f, d.z / l))) : 0.f; }
inline V3 dir_of(float yaw, float pitch) { float c = std::cos(pitch); return {-std::sin(yaw) * c, std::cos(yaw) * c, std::sin(pitch)}; }
inline float rad(float deg) { return deg * PI / 180.f; }
inline float deg(float r) { return r * 180.f / PI; }

// Angle between two directions.
inline float angle_between(const V3& a, const V3& b) {
    float c = a.norm().dot(b.norm());
    return std::acos(std::max(-1.f, std::min(1.f, c)));
}

struct Flight {
    float g = 9.81f;    // m/s^2, downwards
    float k = 0.f;      // 1/s, linear drag
    float E(float t) const { return k > 1e-5f ? (1.f - std::exp(-k * t)) / k : t; }
    // Position relative to the launch point after t seconds.
    V3 at(const V3& v0, float t) const {
        float e = E(t);
        float drop = k > 1e-5f ? g * (t - e) / k : 0.5f * g * t * t;
        return {v0.x * e, v0.y * e, v0.z * e - drop};
    }
    // Time to cover horizontal distance d at horizontal speed vh (<0: never, drag stops it first).
    float time_for(float d, float vh) const {
        if (vh <= 1e-4f) return -1.f;
        if (k <= 1e-5f) return d / vh;
        float q = k * d / vh;
        if (q >= 0.999f) return -1.f;
        return -std::log(1.f - q) / k;
    }
    // Height (relative to launch) when the arrow launched at `pitch` and `speed` has covered d metres.
    bool height_at(float d, float speed, float pitch, float& z, float& t) const {
        float vh = speed * std::cos(pitch), vz = speed * std::sin(pitch);
        t = time_for(d, vh);
        if (t < 0) return false;
        float e = E(t);
        z = vz * e - (k > 1e-5f ? g * (t - e) / k : 0.5f * g * t * t);
        return true;
    }
};

struct Solution {
    bool ok = false;        // false: target out of range (dir then holds the longest-range pitch)
    V3 dir;                 // launch direction (unit)
    float tof = 0;          // seconds to the target
    V3 aim;                 // the point aimed at (target moved ahead by its velocity * tof)
};

// Low-arc launch direction from `origin` that hits the point `target` (moving at `tvel`) with an arrow of
// launch speed `speed`.  Up to 4 lead iterations: aim where the target will be after the flight time.
inline Solution solve(const V3& origin, const V3& target, const V3& tvel, float speed, const Flight& f) {
    Solution s;
    V3 aim = target;
    for (int it = 0; it < 4; it++) {
        V3 r = aim - origin;
        float d = r.hlen(), dz = r.z, yaw = yaw_of(r);
        if (d < 0.05f) { s.ok = true; s.dir = r.norm(); s.tof = r.len() / std::max(1.f, speed); s.aim = aim; return s; }
        // miss(p) = arrow height at distance d - target height.  Rises with pitch on the low arc up to the
        // pitch of longest range; find the first pitch from below where it crosses zero, then bisect.
        float lo = -1.25f, hi = lo, zl = 0, zh = 0, t = 0; bool have = false, low = false;
        float best = lo, bestZ = -1e9f;
        for (float p = -1.25f; p <= 1.25f; p += 0.01f) {
            float z;
            if (!f.height_at(d, speed, p, z, t)) { if (low) break; continue; }
            float m = z - dz;
            if (z > bestZ) { bestZ = z; best = p; }
            if (!low) { low = true; lo = p; zl = m; if (m >= 0) { hi = p; zh = m; have = true; break; } continue; }
            if (m >= 0) { hi = p; zh = m; have = true; break; }
            if (z < bestZ - 1e-3f) break;           // past the longest-range pitch: high arc only from here
            lo = p; zl = m;
        }
        if (!have || zl > 0) {                      // out of range: return the farthest-reaching pitch
            s.ok = false; s.dir = dir_of(yaw, best); s.aim = aim;
            float z; f.height_at(d, speed, best, z, s.tof);
            return s;
        }
        (void)zh;
        for (int b = 0; b < 40; b++) {
            float m = 0.5f * (lo + hi), z;
            if (!f.height_at(d, speed, m, z, t)) { hi = m; continue; }
            if (z - dz >= 0) hi = m; else lo = m;
        }
        float p = 0.5f * (lo + hi), z;
        f.height_at(d, speed, p, z, t);
        s.ok = true; s.dir = dir_of(yaw, p); s.tof = t; s.aim = aim;
        V3 next = target + tvel * t;
        if ((next - aim).len() < 0.02f) break;
        aim = next;
    }
    return s;
}

// ---------------- calibration from real arrows ----------------
// One arrow Henry shot: where and how fast it left (CProjectile::Launch) plus samples of its flight.
struct ShotTrack {
    V3 p0, v0;
    static const int N = 96;
    float t[N]; V3 p[N]; int n = 0;
    void add(float tt, const V3& pp) { if (n < N) { t[n] = tt; p[n] = pp; n++; } }
};

// Fit drag k and gravity g to a tracked flight.  Samples after the arrow stops (hit something) are cut.
// Returns false if there is not enough clean flight to say anything.
inline bool fit_flight(const ShotTrack& s, float& g, float& k) {
    float vh = std::sqrt(s.v0.x * s.v0.x + s.v0.y * s.v0.y), vz = s.v0.z;
    if (vh < 5.f) return false;
    int n = 0; float last = 0;                         // usable prefix: steady horizontal progress
    for (int i = 0; i < s.n; i++) {
        float h = (s.p[i] - s.p0).hlen();
        if (s.t[i] <= 0) continue;
        if (i > 0 && h < last + 0.2f * vh * (s.t[i] - s.t[i - 1])) break;   // slowed sharply: it hit something
        last = h; n = i + 1;
    }
    int use = 0; float tmax = 0; int imax = -1;
    for (int i = 0; i < n; i++) if (s.t[i] > 0.05f) { use++; if (s.t[i] > tmax) { tmax = s.t[i]; imax = i; } }
    if (use < 4 || tmax < 0.25f) return false;
    // k: horizontal distance at the last clean sample = vh * E(tmax; k); E falls as k grows -> bisection.
    float hT = (s.p[imax] - s.p0).hlen();
    Flight f; float klo = 0, khi = 3.f;
    if (hT >= vh * tmax) k = 0;
    else {
        for (int b = 0; b < 50; b++) { f.k = 0.5f * (klo + khi); if (vh * f.E(tmax) > hT) klo = f.k; else khi = f.k; }
        k = 0.5f * (klo + khi);
    }
    // g: height = vz*E - g*G(t) with G = (t-E)/k  -> least squares over the clean samples.
    f.k = k; double num = 0, den = 0;
    for (int i = 0; i < n; i++) {
        if (s.t[i] <= 0.05f) continue;
        float e = f.E(s.t[i]); float G = k > 1e-5f ? (s.t[i] - e) / k : 0.5f * s.t[i] * s.t[i];
        float z = s.p[i].z - s.p0.z;
        num += (double)G * (vz * e - z); den += (double)G * G;
    }
    if (den < 1e-9) return false;
    g = (float)(num / den);
    return g > 0.5f && g < 40.f && k >= 0 && k < 2.9f;
}

// Launch speed of the bow Henry is using, as measured from his real arrows: a few (draw time, speed)
// points, so a bow whose speed grows with the draw is followed too.  Speed is a property of bow + arrow +
// Henry's strength (the game computes it from those, not from a fixed number), so a shot that disagrees
// clearly with the points measured at a similar draw time means something changed (another bow or arrow,
// a strength change): the old points are dropped at once instead of being averaged in slowly.
struct BowSpeed {
    static const int N = 6;
    int n = 0; float hold[N] = {0}, speed[N] = {0};
    static constexpr float kNear = 0.15f;    // s: shots this close in draw time are compared
    static constexpr float kChange = 0.08f;  // an 8% difference there means bow/arrow/strength changed
    float at(float h) const {
        if (n <= 0) return 0.f;
        if (h <= hold[0]) return speed[0];
        for (int i = 1; i < n; i++)
            if (h <= hold[i]) { float a = (h - hold[i - 1]) / std::max(1e-4f, hold[i] - hold[i - 1]); return speed[i - 1] + a * (speed[i] - speed[i - 1]); }
        return speed[n - 1];
    }
    // Returns true when the shot showed that the bow (or arrow, or strength) changed.
    bool add(float h, float s) {
        bool changed = false;
        for (int i = 0; i < n; i++)
            if (std::fabs(hold[i] - h) < kNear && std::fabs(speed[i] - s) > kChange * speed[i]) changed = true;
        if (changed) n = 0;
        int nearest = -1;
        for (int i = 0; i < n; i++) if (std::fabs(hold[i] - h) < 0.12f && (nearest < 0 || std::fabs(hold[i] - h) < std::fabs(hold[nearest] - h))) nearest = i;
        if (nearest < 0 && n == N) {    // full: merge into the closest point
            nearest = 0; for (int i = 1; i < n; i++) if (std::fabs(hold[i] - h) < std::fabs(hold[nearest] - h)) nearest = i;
        }
        if (nearest >= 0) { speed[nearest] = 0.5f * (speed[nearest] + s); return changed; }
        int k = n++;
        while (k > 0 && hold[k - 1] > h) { hold[k] = hold[k - 1]; speed[k] = speed[k - 1]; k--; }
        hold[k] = h; speed[k] = s;
        return changed;
    }
};

// What AutoMarksman has learned about Henry's bow and arrows (kept between sessions in a small file).
struct Calib {
    float speed = 55.f;       // first-shot guess (ArrowSpeed in the .ini) until a real arrow has been measured
    BowSpeed bow;             // measured launch speeds of the current bow
    Flight flight;            // gravity and drag
    float yawOff = 0, pitchOff = 0;   // launch direction minus camera direction at release (radians)
    V3 originOff;             // launch point minus camera position, in camera axes (right, forward, up)
    int shots = 0, fits = 0;
    bool measured() const { return bow.n > 0; }
    float speed_at(float hold) const { return bow.n ? bow.at(hold) : speed; }
    static float ema(float old, float v, int n) { float a = n < 1 ? 1.f : (n < 4 ? 0.5f : 0.25f); return old + a * (v - old); }
    // A real launch: velocity and position of the arrow, camera at that moment, how long the bow was drawn.
    // Returns true when the bow (or arrow, or strength) evidently changed.
    bool on_launch(const V3& pos, const V3& vel, const V3& camPos, const V3& camFwd, float hold) {
        float sp = vel.len();
        if (sp < 5.f || sp > 300.f) return false;
        bool changed = bow.add(hold, sp);
        float dy = wrap_pi(yaw_of(vel) - yaw_of(camFwd)), dp = pitch_of(vel) - pitch_of(camFwd);
        if (std::fabs(dy) < 0.2f && std::fabs(dp) < 0.2f) { yawOff = ema(yawOff, dy, shots); pitchOff = ema(pitchOff, dp, shots); }
        V3 f = camFwd.norm(), r = V3{f.y, -f.x, 0}.norm(), u = V3{r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
        V3 d = pos - camPos;
        if (d.len() < 3.f) {
            V3 o{d.dot(r), d.dot(f), d.dot(u)};
            originOff = shots < 1 ? o : originOff + (o - originOff) * 0.5f;
        }
        shots++;
        return changed;
    }
    void on_flight(float g, float k) {
        flight.g = ema(flight.g, g, fits); flight.k = ema(flight.k, k, fits); fits++;
    }
    // Launch point for a camera at camPos looking along camFwd.
    V3 origin(const V3& camPos, const V3& camFwd) const {
        V3 f = camFwd.norm(), r = V3{f.y, -f.x, 0}.norm(), u = V3{r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
        return camPos + r * originOff.x + f * originOff.y + u * originOff.z;
    }
    // One-line text form, for the file the mod keeps next to its DLL.  load() accepts only sane values.
    int save(char* out, size_t cap) const {
        int w = std::snprintf(out, cap, "v1 %d %d %.4f %.5f %.6f %.6f %.4f %.4f %.4f %d", shots, fits, flight.g, flight.k, yawOff, pitchOff,
                              originOff.x, originOff.y, originOff.z, bow.n);
        for (int i = 0; i < bow.n && w > 0 && (size_t)w < cap; i++) w += std::snprintf(out + w, cap - w, " %.3f %.3f", bow.hold[i], bow.speed[i]);
        return w;
    }
    bool load(const char* in) {
        Calib c = *this; int used = 0;
        if (std::sscanf(in, "v1 %d %d %f %f %f %f %f %f %f %d%n", &c.shots, &c.fits, &c.flight.g, &c.flight.k, &c.yawOff, &c.pitchOff,
                        &c.originOff.x, &c.originOff.y, &c.originOff.z, &c.bow.n, &used) != 10) return false;
        if (c.bow.n < 0 || c.bow.n > BowSpeed::N || !(c.flight.g > 0.5f && c.flight.g < 40.f) || !(c.flight.k >= 0 && c.flight.k < 3.f) ||
            !(std::fabs(c.yawOff) < 0.2f && std::fabs(c.pitchOff) < 0.2f) || !(c.originOff.len() < 3.f)) return false;
        const char* p = in + used;
        for (int i = 0; i < c.bow.n; i++) {
            int u2 = 0;
            if (std::sscanf(p, " %f %f%n", &c.bow.hold[i], &c.bow.speed[i], &u2) != 2) return false;
            if (!(c.bow.speed[i] > 5.f && c.bow.speed[i] < 300.f && c.bow.hold[i] >= 0.f && c.bow.hold[i] < 60.f)) return false;
            if (i && c.bow.hold[i] < c.bow.hold[i - 1]) return false;
            p += u2;
        }
        *this = c; return true;
    }
};

// ---------------- camera steering ----------------
// Turns the view like a steady hand: follows the aim point's own motion (feed-forward, so a walking target
// is tracked without lag) and removes the remaining error proportionally, never faster than maxRate.
struct Steer {
    float maxRate = 2.0f;     // rad/s  (~115 deg/s) on the combined yaw/pitch turn
    float gain = 9.f;         // 1/s: fraction of the error removed per second (before the rate cap)
    float dead = 0.0006f;     // rad: closer than this counts as on target
    // Look-angle change for this frame (x = pitch, z = yaw, like the game's Ang3).  ffYaw/ffPitch: how fast
    // the wanted direction itself is turning (rad/s).
    void step(float errYaw, float errPitch, float ffYaw, float ffPitch, float dt, float& dYaw, float& dPitch) const {
        float a = std::min(1.f, gain * dt), cap = maxRate * dt;
        dYaw = ffYaw * dt + (std::fabs(errYaw) < dead ? 0.f : errYaw * a);
        dPitch = ffPitch * dt + (std::fabs(errPitch) < dead ? 0.f : errPitch * a);
        float m = std::sqrt(dYaw * dYaw + dPitch * dPitch);
        if (m > cap) { dYaw *= cap / m; dPitch *= cap / m; }
    }
};

// Where to aim on a body: from its world bounding box and facing.  People: the head (the top of the box,
// HeadDrop below it).  A box clearly longer than tall is an animal: its chest/neck, ahead of the centre.
inline V3 vital_point(const V3& bmin, const V3& bmax, const V3& fwd, float headDrop) {
    V3 c = (bmin + bmax) * 0.5f;
    float h = bmax.z - bmin.z, lx = bmax.x - bmin.x, ly = bmax.y - bmin.y, len = std::max(lx, ly);
    if (len > 1.25f * h && h > 0.2f) {
        V3 f = V3{fwd.x, fwd.y, 0}.norm();
        return {c.x + f.x * 0.3f * len, c.y + f.y * 0.3f * len, bmin.z + 0.7f * h};
    }
    return {c.x, c.y, bmax.z - std::min(headDrop, 0.4f * h)};
}

}  // namespace mk
