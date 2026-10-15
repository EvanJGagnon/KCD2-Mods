// marksim.cpp -- offline tests for Theatrical AutoMarksman's ballistics, calibration and steering
// (src/mk_ballistics.h).  Plain C++17: builds with MSVC (build.bat) or g++/clang (g++ -O2 -std=c++17).
//
// The "game" here is an independent model: arrows are integrated step by step (not with the closed form
// the mod uses), the bow launches them with an unknown speed, drag, gravity and an unknown offset between
// camera and bow, and targets walk around.  The mod's code only sees what it sees in the game: launch
// velocities, flight samples and the camera.  Every test prints PASS/FAIL; the exit code is the number
// of failures.
#include "../src/mk_ballistics.h"
#include "../src/mk_target.h"
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace mk;
static int g_fail = 0;
static void check(bool ok, const char* what) { printf("  %s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) g_fail++; }

static uint32_t g_rng = 20261014;
static float rnd() { g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) / 16777216.f; }
static float rnd(float a, float b) { return a + (b - a) * rnd(); }

// The world's arrow: semi-implicit Euler at 1 kHz with true g/k.
struct TrueArrow { float g, k; };
// Closest approach of an arrow to point `tp` moving at `tv` (both from t=0); returns miss distance.
static float shoot(const TrueArrow& w, V3 p, V3 v, V3 tp, V3 tv, ShotTrack* track = nullptr, float sampleHz = 60.f) {
    float dt = 0.001f, best = 1e9f, nextSample = 0;
    for (float t = 0; t < 6.f; t += dt) {
        V3 cur = tp + tv * t;
        float d = (p - cur).len(); if (d < best) best = d;
        if (track && t >= nextSample) { track->add(t, p); nextSample += 1.f / sampleHz; }
        v = v + V3{0, 0, -w.g * dt} - v * (w.k * dt);
        p = p + v * dt;
        if (p.z < -60.f) break;
    }
    return best;
}

// The world's bow: launch speed, and how the launch differs from where the camera looks.
// drawFull > 0: the bow only reaches full speed after that many seconds of draw (55% at a snap shot).
struct Bow { float speed, yawOff, pitchOff; V3 originOff; float jitter; float drawFull = 0;
             float speed_at(float hold) const { return drawFull > 0 ? speed * (0.55f + 0.45f * std::min(1.f, hold / drawFull)) : speed; } };
static V3 cam_axes_point(const V3& camPos, const V3& f0, const V3& off) {
    V3 f = f0.norm(), r = V3{f.y, -f.x, 0}.norm(), u = V3{r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
    return camPos + r * off.x + f * off.y + u * off.z;
}
static void bow_launch(const Bow& b, const V3& camPos, const V3& camFwd, V3& pos, V3& vel, float hold = 1.f) {
    pos = cam_axes_point(camPos, camFwd, b.originOff);
    float y = yaw_of(camFwd) + b.yawOff + rnd(-1, 1) * b.jitter, p = pitch_of(camFwd) + b.pitchOff + rnd(-1, 1) * b.jitter;
    vel = dir_of(y, p) * b.speed_at(hold);
}

// A camera whose look angles the mod nudges (like C_ActorPhysicsState::m_lookAngleAccum), ticking at fps.
struct Cam { float yaw = 0, pitch = 0; V3 pos{0, 0, 1.7f}; V3 fwd() const { return dir_of(yaw, pitch); } };

// Steer the camera onto the launch direction that hits the target, as the mod does every frame.
static float aim_and_shoot(const TrueArrow& w, const Bow& bow, Calib& cal, Cam& cam, V3 tgt, V3 tvel, float fps, float settle,
                           bool learn, float* tSettle = nullptr, float* maxRate = nullptr, float hold = -1.f) {
    if (hold < 0) hold = settle;    // released when the aim has settled
    Steer st; float dt = 1.f / fps, rate = 0, t = 0; bool settled = false, first = true; float lastY = 0, lastP = 0;
    for (; t < settle; t += dt) {
        V3 o = cal.origin(cam.pos, cam.fwd());
        Solution s = solve(o, tgt + tvel * t, tvel, cal.speed_at(hold), cal.flight);
        float wy = yaw_of(s.dir) - cal.yawOff, wp = pitch_of(s.dir) - cal.pitchOff;
        float ey = wrap_pi(wy - cam.yaw), ep = wp - cam.pitch, dy, dp;
        float fy = first ? 0.f : wrap_pi(wy - lastY) / dt, fp = first ? 0.f : (wp - lastP) / dt;
        first = false; lastY = wy; lastP = wp;
        st.step(ey, ep, fy, fp, dt, dy, dp);
        cam.yaw = wrap_pi(cam.yaw + dy); cam.pitch += dp;
        rate = std::max(rate, std::sqrt(dy * dy + dp * dp) / dt);
        if (!settled && std::fabs(ey) < 0.001f && std::fabs(ep) < 0.001f) { settled = true; if (tSettle) *tSettle = t; }
    }
    if (maxRate) *maxRate = rate;
    V3 p, v; bow_launch(bow, cam.pos, cam.fwd(), p, v, hold);
    ShotTrack tr; tr.p0 = p; tr.v0 = v;
    float miss = shoot(w, p, v, tgt + tvel * t, tvel, &tr);
    if (learn) {
        cal.on_launch(p, v, cam.pos, cam.fwd(), hold);
        float g, k; if (fit_flight(tr, g, k)) cal.on_flight(g, k);
    }
    return miss;
}

int main() {
    printf("1. solver vs step-by-step arrows (true parameters known)\n");
    {
        int n = 0, hit = 0; float worst = 0;
        for (int i = 0; i < 400; i++) {
            TrueArrow w{9.81f, rnd(0.f, 0.08f)}; float speed = rnd(35.f, 75.f);
            Flight f; f.g = w.g; f.k = w.k;
            V3 o{0, 0, 1.6f}, tp{rnd(-60, 60), rnd(5, 90), rnd(-8, 12)};
            Solution s = solve(o, tp, V3{}, speed, f);
            if (!s.ok) continue;
            n++; float m = shoot(w, o, s.dir * speed, tp, V3{});
            worst = std::max(worst, m); if (m < 0.05f) hit++;
        }
        printf("     %d reachable targets, %d within 5 cm, worst miss %.3f m\n", n, hit, worst);
        check(n > 300 && hit == n, "every reachable target hit within 5 cm");
    }
    printf("2. out of range\n");
    {
        Flight f; Solution s = solve(V3{0, 0, 0}, V3{0, 2000, 0}, V3{}, 40.f, f);
        check(!s.ok, "2 km at 40 m/s is reported out of range (the mod then leaves the camera alone)");
    }
    printf("3. flight fit from 60 Hz samples\n");
    {
        int good = 0, tries = 0;
        for (int i = 0; i < 100; i++) {
            TrueArrow w{rnd(8.f, 11.f), rnd(0.f, 0.1f)}; V3 v = dir_of(rnd(-3, 3), rnd(-0.1f, 0.3f)) * rnd(35, 75);
            ShotTrack tr; tr.p0 = V3{0, 0, 1.6f}; tr.v0 = v;
            shoot(w, tr.p0, v, V3{0, 1e4f, 0}, V3{}, &tr);
            float g, k; tries++;
            if (fit_flight(tr, g, k) && std::fabs(g - w.g) < 0.15f && std::fabs(k - w.k) < 0.006f) good++;
        }
        printf("     %d/%d fits within 0.15 m/s2 and 0.006 1/s\n", good, tries);
        check(good >= 95, "flight fit recovers gravity and drag");
    }
    printf("4. flight fit stops at the impact (arrow stuck in a wall at 0.4 s)\n");
    {
        TrueArrow w{9.81f, 0.03f}; V3 v = dir_of(0, 0.05f) * 60.f;
        ShotTrack tr; tr.p0 = V3{0, 0, 1.6f}; tr.v0 = v;
        float dt = 1.f / 60; V3 p = tr.p0, vv = v;
        for (float t = 0; t < 1.5f; t += dt) {
            tr.add(t, p);
            if (t < 0.4f) { for (int s = 0; s < 16; s++) { vv = vv + V3{0, 0, -w.g * dt / 16} - vv * (w.k * dt / 16); p = p + vv * (dt / 16); } }
        }
        float g = 0, k = 0, span = 0; bool ok = fit_flight(tr, g, k, &span);
        float zf = 0, zt = 0; height_at_dist(20.f, v.hlen(), v.z, g, k, zf); height_at_dist(20.f, v.hlen(), v.z, 9.81f, 0.03f, zt);
        printf("     fit %s over %.1f m: g=%.2f k=%.4f, height at 20 m off by %.3f m\n", ok ? "ok" : "rejected", span, g, k, std::fabs(zf - zt));
        check(ok && span < 25.f && std::fabs(zf - zt) < 0.02f, "samples after the impact are ignored");
    }
    printf("5. learning an unknown bow: head shots at 40 m before and after calibration\n");
    {
        TrueArrow w{9.81f, 0.045f};
        Bow bow{62.f, rad(0.6f), rad(1.4f), V3{0.18f, 0.35f, -0.12f}, 0.f};
        Calib cal; Cam cam; float head = 0.11f;
        V3 tgt{6.f, 40.f, 1.65f};
        float m0 = aim_and_shoot(w, bow, cal, cam, tgt, V3{}, 60, 1.5f, true);
        printf("     uncalibrated (55 m/s, no drag, no offsets): miss %.2f m\n", m0);
        float m = 0; int hits = 0;
        for (int i = 0; i < 6; i++) {
            V3 t2{rnd(-15, 15), rnd(20, 60), rnd(0.5f, 4.f)};
            m = aim_and_shoot(w, bow, cal, cam, t2, V3{}, 60, 1.5f, true);
            if (i >= 2 && m < head) hits++;
        }
        printf("     learned speed %.1f g %.2f k %.4f yawOff %.2f pitchOff %.2f deg origin (%.2f %.2f %.2f) last miss %.3f\n",
               cal.speed_at(1.5f), cal.flight.g, cal.flight.k, deg(cal.yawOff), deg(cal.pitchOff), cal.originOff.x, cal.originOff.y, cal.originOff.z, m);
        check(hits == 4, "after two calibration shots every head shot (r=11 cm, 20-60 m) hits");
    }
    printf("6. moving targets with lead (walking 1.4 m/s, running 4 m/s), calibrated bow\n");
    {
        TrueArrow w{9.81f, 0.045f}; Bow bow{62.f, 0, 0, V3{}, 0.f};
        Calib cal; cal.speed = 62.f; cal.flight.g = 9.81f; cal.flight.k = 0.045f; Cam cam;
        int hits = 0, n = 0;
        for (int i = 0; i < 40; i++) {
            float sp = i % 2 ? 4.f : 1.4f, a = rnd(0, 2 * PI);
            V3 tv{std::cos(a) * sp, std::sin(a) * sp, 0}, t0{rnd(-20, 20), rnd(15, 55), 1.65f};
            float m = aim_and_shoot(w, bow, cal, cam, t0, tv, 60, 1.2f, false); n++;
            if (m < 0.11f) hits++;
        }
        printf("     %d/%d head hits\n", hits, n);
        check(hits == n, "lead keeps every moving head shot within 11 cm");
    }
    printf("7. steering: 25 deg turn at 60 and 30 fps\n");
    {
        for (float fps : {60.f, 30.f}) {
            Calib cal; Cam cam; float ts = -1, mr = 0;
            V3 tgt = cam.pos + dir_of(rad(25), rad(3)) * 30.f;
            TrueArrow w{9.81f, 0}; Bow bow{55, 0, 0, V3{}, 0};
            aim_and_shoot(w, bow, cal, cam, tgt, V3{}, fps, 2.0f, false, &ts, &mr);
            printf("     %2.0f fps: on target after %.2f s, fastest turn %.0f deg/s\n", fps, ts, deg(mr));
            check(ts > 0.2f && ts < 1.2f && mr <= Steer{}.maxRate * 1.001f, "turns steadily (not a snap), settles within 1.2 s, never above the rate cap");
        }
    }
    printf("8. aim jitter is not removed (bow sway stays the player's problem)\n");
    {
        TrueArrow w{9.81f, 0.02f}; Bow bow{60.f, 0, 0, V3{}, rad(0.5f)};
        Calib cal; cal.speed = 60; cal.flight.k = 0.02f; Cam cam; int miss = 0;
        for (int i = 0; i < 40; i++) if (aim_and_shoot(w, bow, cal, cam, V3{rnd(-10, 10), 50, 1.65f}, V3{}, 60, 1.f, false) > 0.11f) miss++;
        printf("     %d/40 head shots missed with 0.5 deg sway at 50 m\n", miss);
        check(miss > 5, "a shaky bow still misses sometimes");
    }
    printf("9. vital point\n");
    {
        V3 h = vital_point(V3{-0.3f, -0.3f, 0}, V3{0.3f, 0.3f, 1.8f}, V3{0, 1, 0}, 0.13f);
        V3 d = vital_point(V3{-0.3f, -0.8f, 0}, V3{0.3f, 0.8f, 1.2f}, V3{0, 1, 0}, 0.13f);
        check(std::fabs(h.z - 1.67f) < 1e-3f && std::fabs(h.y) < 1e-3f, "person: head 13 cm below the top of the box");
        check(d.y > 0.4f && d.z > 0.7f && d.z < 0.9f, "deer-shaped box: chest/neck ahead of the centre");
    }
    printf("10. round 1 report: strong bow (82 m/s), first-shot guess 55 m/s\n");
    {
        TrueArrow w{9.81f, 0.03f}; Bow bow{82.f, rad(0.3f), rad(0.8f), V3{0.15f, 0.3f, -0.1f}, 0.f};
        Calib cal; Cam cam;
        float before[3], after[3]; float dist[3] = {10.f, 30.f, 60.f};
        for (int i = 0; i < 3; i++) { Calib c0; Cam c; before[i] = aim_and_shoot(w, bow, c0, c, V3{2.f, dist[i], 1.65f}, V3{}, 60, 1.5f, false); }
        aim_and_shoot(w, bow, cal, cam, V3{-3.f, 25.f, 1.65f}, V3{}, 60, 1.5f, true);   // one real shot measures the bow
        for (int i = 0; i < 3; i++) after[i] = aim_and_shoot(w, bow, cal, cam, V3{2.f, dist[i], 1.65f}, V3{}, 60, 1.5f, false);
        for (int i = 0; i < 3; i++) printf("     %2.0f m: miss %.2f m before the first arrow, %.3f m after it\n", dist[i], before[i], after[i]);
        check(before[2] > 0.5f, "unmeasured, the strong bow shoots high at 60 m (what Evan saw)");
        check(after[0] < 0.11f && after[1] < 0.11f && after[2] < 0.11f, "after one measured arrow: head hits at 10, 30 and 60 m");
    }
    printf("11. speed that grows with the draw (55%% at a snap shot, full after 1.2 s)\n");
    {
        TrueArrow w{9.81f, 0.03f}; Bow bow{75.f, 0, 0, V3{}, 0.f, 1.2f};
        Calib cal; Cam cam;
        for (float h : {0.4f, 0.5f, 0.9f, 1.6f, 2.5f}) aim_and_shoot(w, bow, cal, cam, V3{rnd(-5, 5), 30.f, 1.65f}, V3{}, 60, 1.f, true, nullptr, nullptr, h);
        float worst = 0;
        int kept = cal.bow.n;
        for (float h : {0.45f, 0.7f, 1.0f, 1.3f, 2.0f, 4.0f}) worst = std::max(worst, std::fabs(cal.speed_at(h) - bow.speed_at(h)) / bow.speed_at(h));
        int hits = 0;
        for (float h : {0.5f, 0.8f, 1.4f, 3.0f}) if (aim_and_shoot(w, bow, cal, cam, V3{rnd(-5, 5), rnd(20, 45), 1.65f}, V3{}, 60, 1.f, false, nullptr, nullptr, h) < 0.11f) hits++;
        printf("     %d speed points kept, worst predicted speed error %.1f%%, %d/4 head hits at other draw times\n", kept, worst * 100, hits);
        check(kept >= 4, "a longer draw is not mistaken for a new bow");
        check(worst < 0.05f, "predicted speed within 5% at any draw time it has seen around");
        check(hits >= 3, "head hits at draw times it never measured");
    }
    printf("12. switching to a weaker bow (82 -> 58 m/s)\n");
    {
        TrueArrow w{9.81f, 0.03f}; Bow strong{82.f, 0, 0, V3{}, 0.f}, weak{58.f, 0, 0, V3{}, 0.f};
        Calib cal; Cam cam;
        for (int i = 0; i < 3; i++) aim_and_shoot(w, strong, cal, cam, V3{rnd(-5, 5), 30.f, 1.65f}, V3{}, 60, 1.5f, true);
        V3 p, v; bow_launch(weak, cam.pos, cam.fwd(), p, v, 1.5f);
        bool changed = cal.on_launch(p, v, cam.pos, cam.fwd(), 1.5f);
        printf("     change noticed: %s, speed now %.1f m/s\n", changed ? "yes" : "no", cal.speed_at(1.5f));
        check(changed && std::fabs(cal.speed_at(1.5f) - 58.f) < 0.5f, "one arrow from the new bow replaces the old speed (no slow averaging)");
    }
    printf("13. close range (4-8 m): bow beside the eye\n");
    {
        TrueArrow w{9.81f, 0.03f}; Bow bow{70.f, 0, 0, V3{0.22f, 0.4f, -0.18f}, 0.f};
        Calib cal; Cam cam; float worst0 = 0, worst1 = 0;
        for (float d : {4.f, 6.f, 8.f}) { Calib c0; c0.speed = 70.f; Cam c; worst0 = std::max(worst0, aim_and_shoot(w, bow, c0, c, V3{1.f, d, 1.65f}, V3{}, 60, 1.5f, false)); }
        aim_and_shoot(w, bow, cal, cam, V3{0.f, 20.f, 1.65f}, V3{}, 60, 1.5f, true);
        for (float d : {4.f, 6.f, 8.f}) worst1 = std::max(worst1, aim_and_shoot(w, bow, cal, cam, V3{1.f, d, 1.65f}, V3{}, 60, 1.5f, false));
        printf("     worst miss %.2f m without the measured offset, %.3f m with it\n", worst0, worst1);
        check(worst1 < 0.05f, "close head shots land within 5 cm once the bow's position is measured");
    }
    printf("14. calibration file round trip and bad input\n");
    {
        Calib a; a.flight.g = 9.7f; a.flight.k = 0.031f; a.yawOff = 0.004f; a.pitchOff = 0.012f; a.originOff = V3{0.2f, 0.35f, -0.1f};
        a.bow.add(0.5f, 50.f); a.bow.add(1.4f, 78.f); a.shots = 7; a.fits = 5;
        char buf[256]; a.save(buf, sizeof buf);
        Calib b; bool ok = b.load(buf);
        Calib c; bool bad = c.load("v1 1 1 9.8 0.03 0 0 0 0 0 1 0.5 9999") || c.load("garbage") || c.load("v1 1 1 99 0 0 0 0 0 0 0");
        printf("     \"%s\"\n", buf);
        check(ok && b.bow.n == 2 && std::fabs(b.speed_at(1.4f) - 78.f) < 0.01f && std::fabs(b.flight.k - 0.031f) < 1e-4f && std::fabs(b.originOff.z + 0.1f) < 1e-3f,
              "saved calibration loads back unchanged");
        check(!bad && !c.measured(), "corrupt or implausible files are ignored");
    }
    printf("15. round 2 flight: 135 fps frames, arrow moved by 30 Hz physics, pivot 6 cm off the launch point\n");
    {
        int good = 0;
        for (int i = 0; i < 40; i++) {
            TrueArrow w{rnd(8.f, 11.f), rnd(0.f, 0.08f)}; V3 v = dir_of(rnd(-3, 3), rnd(-0.05f, 0.15f)) * rnd(55, 85);
            ShotTrack tr; tr.p0 = V3{0, 0, 1.6f}; tr.v0 = v;
            V3 p = tr.p0, vv = v, shown = p + V3{0, 0, 0.06f}; float physT = 0;
            for (float t = 0; t < 0.71f; t += 1.f / 135) {
                while (physT + 1.f / 30 <= t) {   // physics steps
                    for (int k = 0; k < 40; k++) { float h = 1.f / 1200; vv = vv + V3{0, 0, -w.g * h} - vv * (w.k * h); p = p + vv * h; }
                    physT += 1.f / 30; shown = p + V3{0, 0, 0.06f};
                }
                tr.add(t, shown);
            }
            float g, k, span;
            if (fit_flight(tr, g, k, &span)) {
                // what matters for aiming: the drop at 60 m
                float zf = 0, zt = 0; height_at_dist(60.f, v.hlen(), v.z, g, k, zf); height_at_dist(60.f, v.hlen(), v.z, w.g, w.k, zt);
                if (std::fabs(zf - zt) < 0.05f) good++; else printf("       off %.3f m (k %.3f/%.3f g %.2f/%.2f span %.0f)\n", zf - zt, k, w.k, g, w.g, span);
            }
        }
        printf("     %d/40 fits predict the height at 60 m within 5 cm\n", good);
        check(good >= 38, "the arrow drop is measured from a real-looking track (round 2 said 'too short to measure')");
    }
    printf("16. predicted impact lands on the head after the aim settles (near, mid, far; still and walking)\n");
    {
        TrueArrow w{9.81f, 0.03f}; Bow bow{76.f, rad(-0.09f), rad(2.47f), V3{0.01f, 0.55f, -0.01f}, 0.f};   // round 2's measured bow
        Calib cal; Cam cam; aim_and_shoot(w, bow, cal, cam, V3{0, 25, 1.65f}, V3{}, 60, 1.5f, true);
        cal.flight.g = 9.81f; cal.flight.k = 0.03f;
        float worstPred = 0, worstReal = 0;
        for (float d : {8.f, 25.f, 60.f}) for (float sp : {0.f, 1.4f}) {
            V3 t0{rnd(-4, 4), d, 1.7f}, tv{sp, 0, 0};
            Cam c = cam; float real = aim_and_shoot(w, bow, cal, c, t0, tv, 60, 1.5f, false);
            V3 o = cal.origin(c.pos, c.fwd()), at;
            V3 launchDir = dir_of(yaw_of(c.fwd()) + cal.yawOff, pitch_of(c.fwd()) + cal.pitchOff);
            Solution s = solve(o, t0 + tv * 1.5f, tv, cal.speed_at(1.5f), cal.flight);
            impact_near(o, launchDir, cal.speed_at(1.5f), cal.flight, t0 + tv * (1.5f + s.tof), at);
            worstPred = std::max(worstPred, (at - (t0 + tv * (1.5f + s.tof))).len()); worstReal = std::max(worstReal, real);
        }
        printf("     worst predicted impact %.3f m from the head, worst real arrow %.3f m\n", worstPred, worstReal);
        check(worstPred < 0.05f && worstReal < 0.08f, "the predicted impact point and the real arrow both meet the head");
    }
    printf("17. target choice: people before horses and dogs\n");
    {
        TargetPolicy p; Lock L;
        Cand c[] = {{1, rad(3.f), 40.f, K_HORSE}, {2, rad(6.f), 25.f, K_PERSON}, {3, rad(1.f), 8.f, K_DOG}};
        L.update(c, 3, p, false);
        check(L.id == 2, "Henry's horse and dog are skipped; the person in view is taken");
        Cand d[] = {{4, rad(2.f), 30.f, K_ANIMAL}, {5, rad(2.5f), 30.f, K_PERSON}};
        Lock L2; L2.update(d, 2, p, false);
        check(L2.id == 5, "a person a little off-centre wins over a deer");
        Cand e[] = {{4, rad(1.f), 30.f, K_ANIMAL}, {5, rad(6.f), 30.f, K_PERSON}};
        Lock L3; L3.update(e, 2, p, false);
        check(L3.id == 4, "a deer right in the middle is still hunted");
    }
    printf("18. soft lock: looking away lets go, and the camera never swings back\n");
    {
        TargetPolicy p; Lock L;
        Cand a[] = {{7, rad(4.f), 30.f, K_PERSON}, {8, rad(9.f), 30.f, K_PERSON}};
        L.update(a, 2, p, false); bool took = L.id == 7;
        Cand b[] = {{7, rad(12.f), 30.f, K_PERSON}, {8, rad(3.f), 30.f, K_PERSON}};
        Lock::Why w1 = L.update(b, 2, p, true);           // mouse moving: 7 is now outside the cone
        bool dropped = w1 == Lock::LOOKED_AWAY && L.id == 0;
        Lock::Why w2 = L.update(b, 2, p, false);          // mouse still: fresh choice from the new view
        check(took && dropped, "pulling the view off the target releases it at once");
        check(w2 == Lock::NEW && L.id == 8, "the next choice is the person now in view, not the old one");
        Cand far[] = {{7, rad(14.f), 30.f, K_PERSON}};
        Lock L2; L2.id = 7; L2.update(far, 1, p, true); L2.update(far, 1, p, false);
        check(L2.id == 0, "with nobody in view after looking away, the assist stays off (no snap back)");
        Cand near[] = {{7, rad(11.f), 30.f, K_PERSON}};
        Lock L3; L3.id = 7; L3.update(near, 1, p, false);
        check(L3.id == 7, "while the mod steers, a small drift outside the cone does not drop the target");
    }
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail;
}
