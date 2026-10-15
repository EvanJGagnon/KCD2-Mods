// lpsim.cpp -- offline check of the AutoLockpick hand (src/lp_logic.h) against a model of the game's lock.
// The lock model follows C_LockPicking's per-frame code in WHGame.dll build 15693 (Update 0x89A940:
// rotation 0x89AA30, pick 0x89AE30, judgement 0x89BAEC; turn key 0xA98614) for states, rotation, spring-back,
// the 0.2 withdrawn zone and the open angle.  Tremble shape, spring rate and pick wear are NOT decoded
// (they come from the RPG tables): the model uses plausible values and says so.  It checks the hand's logic,
// not the game's balance.
// Usage: lpsim [locks=1000] [seed=1] [verbose=0]   exit code 1 if the hand opens too few easy locks.
#include "lp_logic.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <random>

struct Lock {
    int state = lp::ST_IDLE;
    float angle = 0, start = 0, end = 358, tol, bx, by, cx = 0, cy = 0, sx = 0, sy = 0, err = 0, health = 1;
    float turn = 0;               // +0xB4
    float spring;                 // deg/s at 360 degrees
    float shakeAmp, shakeHz, ph[4], wear;
    double time = 0;
    float shake_x(double t) const { return shakeAmp * 0.5f * (float)(std::sin(t * shakeHz * 6.283 + ph[0]) + std::sin(t * shakeHz * 4.1 + ph[1])); }
    float shake_y(double t) const { return shakeAmp * 0.5f * (float)(std::sin(t * shakeHz * 5.3 + ph[2]) + std::sin(t * shakeHz * 3.7 + ph[3])); }
    void input_turn(int mode) {
        if (state < 3 || state > 5) return;
        if (mode == lp::TURN_HOLD) turn = 100.f;
        else if (mode == lp::TURN_RELEASE) { turn = 0; angle = start; state = lp::ST_IDLE; }
    }
    void input_pick(float x, float y) { if (state >= 3 && state <= 5) { cx = x; cy = y; } }
    void update(float dt) {
        time += dt;
        if (state == lp::ST_SUCCESS || state == lp::ST_COMMIT || state == lp::ST_BROKE) return;
        if (angle > start) angle = std::max(0.f, angle - angle / 360.f * spring * dt);
        if (turn != 0) {
            if (state == lp::ST_IDLE && start + 9 > angle) state = lp::ST_DRYRUN;
            if (state == lp::ST_ONPLACE || state == lp::ST_DRYRUN) {
                float a = std::min(360.f, std::max(0.f, angle + turn * dt));
                if (state == lp::ST_DRYRUN && a > start + 4) a = start + 3;
                angle = a;
            }
        }
        float px, py; lp::pin_at(bx, by, angle, px, py);
        float r = std::sqrt(cx * cx + cy * cy); if (r > 1) { cx /= r; cy /= r; }
        sx = std::min(1.f, std::max(-1.f, cx + shake_x(time)));
        sy = std::min(1.f, std::max(-1.f, cy + shake_y(time)));
        err = std::hypot(sx - px, sy - py) / std::max(tol, 0.0001f);
        if (angle > start) { health -= wear * err * err * dt; if (health <= 0) { state = lp::ST_BROKE; return; } }
        if (cx * cx + cy * cy <= 0.04f) { if (state == lp::ST_ONPLACE) state = lp::ST_IDLE; return; }
        if (err <= 1) { if (angle >= end) state = lp::ST_SUCCESS; else state = lp::ST_ONPLACE; }
        else if (state == lp::ST_ONPLACE) state = lp::ST_IDLE;
    }
    lp::View view() const {
        lp::View v{}; v.state = state; v.angle = angle; v.start = start; v.end = end; v.tol = tol;
        v.cx = cx; v.cy = cy; v.sx = sx; v.sy = sy; v.bx = bx; v.by = by; v.err = err; v.health = health; return v;
    }
};

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1000; unsigned seed = argc > 2 ? (unsigned)atoi(argv[2]) : 1u; int verbose = argc > 3 ? atoi(argv[3]) : 0;
    std::mt19937 rng(seed); std::uniform_real_distribution<float> U(0, 1);
    int res[8] = {0}, easyN = 0, easyOpen = 0; double tOpen = 0; int tries = 0;
    for (int i = 0; i < n; i++) {
        Lock L; float a = U(rng) * 6.2832f, r = 0.3f + 0.6f * U(rng);
        L.bx = r * std::cos(a); L.by = r * std::sin(a);
        L.tol = 0.06f + 0.2f * U(rng);                       // pin spot size
        L.shakeAmp = 0.04f + 0.13f * U(rng);                 // half-hand-shaking mod range 0.04..0.17
        L.shakeHz = 0.3f + 0.5f * U(rng);
        for (float& p : L.ph) p = U(rng) * 6.2832f;
        L.spring = 20.f + 60.f * U(rng);
        L.wear = 0.05f + 0.1f * U(rng);
        bool easy = L.shakeAmp < 0.6f * L.tol;                // tremble well inside the spot: a steady hand opens it
        lp::Hand h; lp::View v = L.view(); h.begin(v);
        const float dt = 1.f / 60.f; double t = 0;
        while (h.active() && t < 120) {
            v = L.view();
            lp::Cmd c = h.step(v, dt);
            if (c.move) L.input_pick(c.x, c.y);
            if (c.turn) L.input_turn(c.turn);
            L.update(dt); t += dt;
        }
        if (h.active()) { v = L.view(); h.step(v, dt); }
        res[h.outcome]++; tries += h.tries;
        if (h.outcome == lp::O_OPENED) tOpen += t;
        if (easy) { easyN++; if (h.outcome == lp::O_OPENED) easyOpen++; }
        if (verbose) printf("lock %d tol=%.3f shake=%.3f r=%.2f spring=%.0f -> %s in %.1fs, %d tries, health %.2f\n",
                            i, L.tol, L.shakeAmp, r, L.spring, lp::outcome_name(h.outcome), t, h.tries, L.health);
    }
    printf("%d locks: opened %d, pick broke %d, gave up (tries) %d, gave up (seek) %d, other %d; mean open time %.1fs, mean tries %.2f\n",
           n, res[lp::O_OPENED], res[lp::O_BROKE], res[lp::O_GAVE_UP_TRIES], res[lp::O_GAVE_UP_SEEK], res[lp::O_ENDED] + res[lp::O_NONE],
           res[lp::O_OPENED] ? tOpen / res[lp::O_OPENED] : 0.0, (double)tries / n);
    printf("easy locks (tremble < 60%% of the spot): %d/%d opened\n", easyOpen, easyN);
    return (easyN && easyOpen < easyN * 0.95) ? 1 : 0;
}
