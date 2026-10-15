// lp_logic.h -- Theatrical AutoLockpick: the hand on the pick (pure logic, no game access).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Each frame the mod reads the lock (LpView) and this decides what Henry's hands do (LpCmd): where the
// pick is held (the same -1..1 position the gamepad stick gives the game) and whether the turn key is
// held, pressed on or let go.  The game does everything else: it adds Henry's tremble to the pick, judges
// whether the pick sits on the pin, turns the lock, wears and breaks picks and opens the lock.
// Shared by the mod (autolockpick.cpp) and the offline simulator (tools/lpsim.cpp).
#pragma once
#include <cmath>
#include <algorithm>

namespace lp {

// C_LockPicking states (+0x88), build 15693
enum { ST_IDLE = 3, ST_ONPLACE = 4, ST_DRYRUN = 5, ST_SUCCESS = 6, ST_COMMIT = 7, ST_BROKE = 8 };

struct View {
    int   state;
    float angle, start, end;     // lock angle and its start / open angle, degrees
    float tol;                   // size of the pin's spot (normalised error 1 = its edge)
    float cx, cy;                // pick position as commanded (before tremble)
    float sx, sy;                // pick position after Henry's tremble (what the game judges)
    float bx, by;                // where the pin sits with the lock at 0 degrees
    float err;                   // the game's normalised error (|shaken pick - pin| / tol)
    float health;                // pick health
};

// What the hands do this frame.
enum Turn { TURN_NONE = 0, TURN_HOLD = 4, TURN_RELEASE = 2 };    // CryEngine activation modes
struct Cmd { bool move = false; float x = 0, y = 0; int turn = TURN_NONE; };

struct Params {
    float seekSpeed  = 1.1f;     // pick travel while feeling for the pin (units/s; the pick's whole range is -1..1)
    float trackSpeed = 3.0f;     // fastest the hand follows the turning pin
    float reaction   = 0.35f;    // s before the hand starts moving
    float settle     = 0.30f;    // s the pick rests on the pin before the turn key goes down
    float lostGrace  = 0.30f;    // s off the pin while turning before the key is let go (the lock springs back)
    float pause      = 0.60f;    // s between a let-go and the next try
    float seekLimit  = 10.f;     // s without finding the pin -> give up
    int   maxTries   = 6;        // tries (turn key presses) per lock before giving up
};

// Pin position for a lock angle, as the game computes it (C_LockPicking target rotation, 0x89AA30).
inline void pin_at(float bx, float by, float angleDeg, float& x, float& y) {
    const float phi = -angleDeg * 0.017453292f;
    const float c = std::cos(phi), s = std::sin(phi);
    x = bx * c + by * s;
    y = by * c - bx * s;
}

enum Phase { P_OFF, P_REACT, P_SEEK, P_SETTLE, P_TURN, P_PAUSE, P_DONE };
enum Outcome { O_NONE, O_OPENED, O_BROKE, O_GAVE_UP_TRIES, O_GAVE_UP_SEEK, O_ENDED };

struct Hand {
    Params p;
    Phase phase = P_OFF;
    Outcome outcome = O_NONE;
    float t = 0;                 // time in the current phase
    float seekT = 0;             // time feeling for the pin since the last try
    float lost = 0;              // time off the pin while turning
    float omega = 100.f;         // measured turn speed (deg/s), used to lead the pin by one frame
    float lastAngle = 0;
    int   tries = 0;
    bool  holding = false;       // turn key is down
    float hx = 0, hy = 0;        // where the hand holds the pick

    void begin(const View& v) {
        phase = P_REACT; outcome = O_NONE; t = seekT = lost = 0; tries = 0; holding = false;
        hx = v.cx; hy = v.cy; lastAngle = v.angle; omega = 100.f;
    }
    bool active() const { return phase != P_OFF && phase != P_DONE; }
    void go(Phase ph) { phase = ph; t = 0; }
    void finish(Outcome o, Cmd& c) {
        if (holding) { c.turn = TURN_RELEASE; holding = false; }
        outcome = o; go(P_DONE);
    }
    // Move the hand toward (x, y) at no more than `speed`; slows down over the last stretch like a hand does.
    void reach(float x, float y, float speed, float dt, Cmd& c) {
        float dx = x - hx, dy = y - hy, d = std::sqrt(dx * dx + dy * dy);
        float v = std::min(speed, std::max(0.12f, d * 6.f));
        float step = v * dt;
        if (d <= step || d < 1e-5f) { hx = x; hy = y; }
        else { hx += dx / d * step; hy += dy / d * step; }
        float r = std::sqrt(hx * hx + hy * hy);
        if (r > 1.f) { hx /= r; hy /= r; }      // the stick's range: the game keeps the pick inside the lock
        c.move = true; c.x = hx; c.y = hy;
    }

    Cmd step(const View& v, float dt) {
        Cmd c;
        if (!active()) return c;
        if (dt <= 0.f || dt > 0.25f) dt = 0.016f;
        t += dt;
        if (v.state == ST_SUCCESS || v.state == ST_COMMIT) { holding = false; outcome = O_OPENED; go(P_DONE); return c; }
        if (v.state == ST_BROKE) { holding = false; outcome = O_BROKE; go(P_DONE); return c; }
        if (v.state < ST_IDLE || v.state > ST_DRYRUN) { holding = false; outcome = O_ENDED; go(P_DONE); return c; }
        float da = v.angle - lastAngle; lastAngle = v.angle;
        if (holding && da > 0.f) omega = 0.8f * omega + 0.2f * (da / dt);
        float px, py; pin_at(v.bx, v.by, v.angle, px, py);

        switch (phase) {
        case P_REACT:
            if (t >= p.reaction) go(P_SEEK);
            break;
        case P_SEEK:
            seekT += dt;
            if (seekT > p.seekLimit) { finish(O_GAVE_UP_SEEK, c); break; }
            reach(px, py, p.seekSpeed, dt, c);
            if (v.state == ST_ONPLACE && std::fabs(hx - px) + std::fabs(hy - py) < 0.004f) go(P_SETTLE);
            break;
        case P_SETTLE:
            reach(px, py, p.seekSpeed, dt, c);
            if (v.state != ST_ONPLACE) { if (t > p.settle * 4) go(P_SEEK); break; }   // tremble took it off: wait
            if (t >= p.settle) {
                if (tries >= p.maxTries) { finish(O_GAVE_UP_TRIES, c); break; }
                tries++; lost = 0; holding = true; c.turn = TURN_HOLD; go(P_TURN);
            }
            break;
        case P_TURN: {
            // lead the pin by one frame of turning: the game turns the lock before it judges the pick
            float lx, ly; pin_at(v.bx, v.by, v.angle + (v.state == ST_ONPLACE ? omega * dt : 0.f), lx, ly);
            reach(lx, ly, p.trackSpeed, dt, c);
            c.turn = TURN_HOLD;
            if (v.state == ST_ONPLACE) lost = 0;
            else if ((lost += dt) > p.lostGrace) { c.turn = TURN_RELEASE; holding = false; go(P_PAUSE); }
            break;
        }
        case P_PAUSE:
            if (t >= p.pause) { seekT = 0; go(P_SEEK); }
            break;
        default: break;
        }
        return c;
    }
    // The player stops it: let go of the key, leave the pick where it is.
    Cmd cancel() { Cmd c; if (holding) { c.turn = TURN_RELEASE; holding = false; } outcome = O_NONE; phase = P_OFF; return c; }
};

inline const char* outcome_name(Outcome o) {
    switch (o) {
    case O_OPENED: return "opened";
    case O_BROKE: return "pick broke";
    case O_GAVE_UP_TRIES: return "gave up (too many tries)";
    case O_GAVE_UP_SEEK: return "gave up (pin not found)";
    case O_ENDED: return "minigame ended";
    default: return "-";
    }
}

}  // namespace lp
