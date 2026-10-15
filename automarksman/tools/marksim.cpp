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
#include "../src/mk_pose.h"
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
    printf("19. arrow helper offset: learnt from one launch, applied to the next\n");
    {
        bool ok = true;
        for (int i = 0; i < 50; i++) {
            V3 dir = dir_of(rnd(-3, 3), rnd(-0.5f, 0.5f)), side = cross(dir, V3{0, 0, 1}).norm(), up = cross(side, dir).norm();
            V3 t{rnd(-0.1f, 0.1f), rnd(-0.6f, 0.1f), rnd(-0.05f, 0.05f)}, joint{rnd(-50, 50), rnd(-50, 50), rnd(0, 40)};
            V3 launch = joint - slot_shift(dir, up, t);
            V3 back = slot_learn(dir, up, joint - launch);
            LivePose lp; lp.pos = joint; lp.dir = dir; lp.up = up;
            ok = ok && (back - t).len() < 1e-4f && (launch_point(lp, back) - launch).len() < 1e-4f;
        }
        check(ok, "joint -> launch point -> offset -> launch point round trip (50 random poses)");
    }
    printf("20. judging a real arrow: signed miss where it crosses the target's distance\n");
    {
        TrueArrow w{9.0f, 0.0f}; V3 p0{0, 0, 1.6f}; V3 head{1.0f, 30.f, 1.7f};
        // aim 10 cm high and 5 cm right of the head
        V3 want = head + V3{0.05f * 30.f / 30.f, 0, 0.10f};
        Flight f; f.g = 9.0f; f.k = 0; Solution s = solve(p0, want, V3{}, 70.f, f);
        ShotTrack tr; tr.p0 = p0; tr.v0 = s.dir * 70.f; shoot(w, p0, tr.v0, head, V3{}, &tr, 135.f);
        V3 h = heading(tr.v0), at; float tc = 0; V3 r = head - p0; float d = r.x * h.x + r.y * h.y;
        bool got = crossing(tr, p0, h, d, at, tc); float up = 0, right = 0; if (got) miss_parts(h, at, head, up, right);
        printf("     crossing after %.3f s: %+.1f cm up, %+.1f cm right (aimed +10 up, +5 right)\n", tc, up * 100, right * 100);
        check(got && std::fabs(up - 0.10f) < 0.01f && std::fabs(right - 0.05f) < 0.01f, "signed up/right miss recovered within 1 cm");
    }
    printf("21. round 3 replay (Evan's real arrows, 0.3.0): fixed camera offsets explain the misses\n");
    {
        // From kcd2_automarksman.log (round 3): each arrow's real launch vs camera (yaw, pitch deg), distance to the
        // aim point at release, the closest approach the mod measured (cm), and the calibration as loaded (round 2).
        struct Shot { int n; float yaw, pitch, dist, closest; bool aimed = false; };
        Shot sh[] = {{2, -0.17f, 2.30f, 13.0f, 5}, {3, 0.01f, 2.03f, 27.0f, 18}, {4, -0.00f, 2.04f, 19.1f, 10}, {5, 0.14f, 2.22f, 7.2f, 5},
                     {6, 0.54f, -0.13f, 0, 0}, {7, 0.54f, 2.05f, 0, 0}, {8, -0.11f, 2.31f, 11.8f, 16}};
        for (auto& x : sh) x.aimed = x.dist > 0;
        Calib c; c.shots = 1; c.yawOff = -0.001578f; c.pitchOff = 0.043052f;
        printf("     arrow  dist   offset used -> real (pitch deg)   explained   measured\n");
        bool ok = true;
        for (auto& x : sh) {
            float usedY = c.yawOff, usedP = c.pitchOff;
            V3 cf{0, 1, 0}; V3 v = dir_of(rad(x.yaw), rad(x.pitch)) * 76.f;
            if (x.aimed) {
                float ey = rad(x.yaw) - usedY, ep = rad(x.pitch) - usedP;
                float expl = std::sqrt(ey * ey + ep * ep) * x.dist * 100.f;
                printf("     %5d %5.1f m   %+.2f -> %+.2f            %5.1f cm   %5.0f cm\n", x.n, x.dist, deg(usedP), x.pitch, expl, x.closest);
                ok = ok && std::fabs(expl - x.closest) < 8.f;
                // With the bow's live pose the direction used is the bow's own: what remains is only how much the
                // bow turns between the last aimed frame and the shot (not in this log; checked in game).
            }
            c.on_launch(V3{0, 0.55f, 0}, v, V3{}, cf, 2.f);
        }
        check(ok, "every aimed round 3 miss is within 8 cm of what the camera-offset error alone predicts");
        check(std::fabs(deg(c.pitchOff) - 1.851f) < 0.01f, "the replayed calibration ends where Evan's file does (pitch offset 1.851 deg)");
    }
    printf("22. live bow pose: bow pitch depends on the view and sways; the impact still lands on the head\n");
    {
        TrueArrow w{9.0f, 0.0f}; Flight fl; fl.g = 9.0f; fl.k = 0;
        V3 slot{0.0f, -0.62f, 0.01f};
        // The world's bow: points 2.1 deg above the camera plus 4% of the view pitch, sways 0.15 deg at 0.6 Hz.
        auto bow_pose = [&](const Cam& cm, float t) {
            LivePose lp; float pit = cm.pitch + rad(2.1f) + 0.04f * cm.pitch + rad(0.15f) * std::sin(t * 3.77f);
            float yw = cm.yaw + rad(0.1f) * std::sin(t * 2.3f);
            lp.dir = dir_of(yw, pit); V3 side = cross(lp.dir, V3{0, 0, 1}).norm(); lp.up = cross(side, lp.dir).norm();
            lp.pos = cm.pos + side * 0.05f + lp.dir * 0.6f - lp.up * 0.04f; lp.ok = true; return lp; };
        float worstGap = 0, worstReal = 0, worstOld = 0;
        for (float dist : {8.f, 15.f, 30.f, 45.f, 60.f}) for (int rep = 0; rep < 6; rep++) {
            Cam cam; cam.yaw = rnd(-0.3f, 0.3f); cam.pitch = rnd(-0.05f, 0.05f);
            V3 head{rnd(-3, 3), dist, 1.7f + rnd(-1.f, 3.f)};
            Steer st; PoseOffset off; float dt = 1.f / 120.f, lastY = 0, lastP = 0; bool first = true; float gap = 1;
            float tRel = 1.2f + rnd(0, 0.8f);
            for (float t = 0; t < tRel; t += dt) {
                LivePose lp = bow_pose(cam, t); V3 o = launch_point(lp, slot);
                off.update(lp.dir, cam.fwd(), dt, 0.03f);
                Solution s = solve(o, head, V3{}, 76.f, fl);
                float wy = yaw_of(s.dir) - off.yaw, wp = pitch_of(s.dir) - off.pitch;
                float ey = wrap_pi(wy - cam.yaw), ep = wp - cam.pitch, dy, dp;
                float fy = first ? 0.f : wrap_pi(wy - lastY) / dt, fp = first ? 0.f : (wp - lastP) / dt; first = false; lastY = wy; lastP = wp;
                st.step(ey, ep, fy, fp, dt, dy, dp); cam.yaw = wrap_pi(cam.yaw + dy); cam.pitch += dp;
                V3 at; if (impact_near(o, lp.dir, 76.f, fl, head, at)) gap = (at - head).len();
            }
            // The game fires ~80 ms later (animation event), from the bow's pose at that moment.
            LivePose f = bow_pose(cam, tRel + 0.08f); V3 p0 = launch_point(f, slot);
            float real = shoot(w, p0, f.dir * 76.f, head, V3{});
            // The 0.3.0 way: camera + a fixed offset (the average bow offset), same camera.
            float oldReal = 0;
            {
                Cam c2; c2.yaw = rnd(-0.3f, 0.3f); c2.pitch = 0; Calib cal; cal.yawOff = 0; cal.pitchOff = rad(2.1f); cal.originOff = V3{0.05f, -0.02f, -0.04f}; cal.speed = 76.f; cal.flight = fl;
                Steer st2; float lY = 0, lP = 0; bool fr = true; float t = 0;
                for (; t < tRel; t += dt) {
                    V3 o = cal.origin(c2.pos, c2.fwd()); Solution s = solve(o, head, V3{}, 76.f, fl);
                    float wy = yaw_of(s.dir) - cal.yawOff, wp = pitch_of(s.dir) - cal.pitchOff, ey = wrap_pi(wy - c2.yaw), ep = wp - c2.pitch, dy, dp;
                    float fy = fr ? 0.f : wrap_pi(wy - lY) / dt, fp = fr ? 0.f : (wp - lP) / dt; fr = false; lY = wy; lP = wp;
                    st2.step(ey, ep, fy, fp, dt, dy, dp); c2.yaw = wrap_pi(c2.yaw + dy); c2.pitch += dp;
                }
                LivePose f2 = bow_pose(c2, tRel + 0.08f); oldReal = shoot(w, launch_point(f2, slot), f2.dir * 76.f, head, V3{});
            }
            worstGap = std::max(worstGap, gap); worstReal = std::max(worstReal, real); worstOld = std::max(worstOld, oldReal);
        }
        printf("     worst predicted impact at release %.3f m; worst real arrow %.3f m (0.3.0 fixed offsets: %.3f m)\n", worstGap, worstReal, worstOld);
        check(worstGap < 0.05f, "the predicted impact from the live pose stays on the head while the bow sways (8-60 m)");
        check(worstReal < 0.12f && worstReal < 0.5f * worstOld, "real arrows (bow fired 80 ms after release, still swaying) land on the head; fixed offsets miss far more");
    }
    printf("23. speed check: the game's formula is trusted; odd shots are reported, not learnt\n");
    {
        SpeedCheck sc; bool a = sc.add(76.2f, 76.2f), b = sc.add(60.5f, 76.2f), c = sc.add(77.0f, 76.2f);
        check(a && !b && c && std::fabs(sc.ratio - 1.f) < 0.02f, "a 21% slower arrow (round 3, 15.6 s hold) does not change the speed used");
    }
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail;
}
