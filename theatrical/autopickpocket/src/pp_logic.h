// pp_logic.h -- Theatrical AutoPickpocket decision logic, free of Windows and game code so the offline
// simulator (tools/ppsim.cpp) runs exactly what the mod runs.
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// How the minigame works (build 15693; details and addresses in NOTES.md):
//  Charge: Henry holds the pickpocket key behind the victim. C_Pickpocketing adds time to its "boost" every
//   frame (+0x84, seconds) and once a second checks  notFail(boost) >= roll  (the roll is drawn when the
//   charge starts); notFail falls as the boost grows. The ring the player watches is SetBoostProgress's
//   last argument, safety = remap(notFail, roll + IndicatorSharpness .. 1 -> 0 .. 1): 1 green, 0 red.
//   At 0 the check still passes (sharpness is the margin); below roll the victim notices.
//   Releasing the key ("charge", onRelease) before the minimum (+0x8C) gives up, after it opens the grid
//   with the boost as the time budget; at the maximum (+0x90) the grid opens by itself.
//  Grid (Pickpocketing.gfx): slot 0 is the exit, 11 item slots around a ring. The selector moves one slot
//   per MoveLeft/MoveRight at SelectorSpeed slots/s (twice as fast towards the exit with the Easy Way Out
//   perk); a move or an action is ignored while the selector is still moving. DoAction on a covered item
//   uncovers it (1 s), it stays uncovered for 1 s and covers again (1 s); DoAction on an uncovered item
//   takes it and costs (penalty + 1) s of the budget; DoAction on the exit ends with success. When the
//   budget runs out the game ends the minigame: success if the selector rests on the exit, caught if not.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cmath>

namespace pp {

static const int NSLOT = 12, EXIT = 0;

// ---------------- hints: the UI's OnActionHintState(action, visible, enable) ----------------
enum HintAction { H_TAKE = 0, H_UNCOVER = 1, H_CLOSE = 2, H_OTHER = 3 };
inline int hint_action(const char* n) {
    if (!n) return H_OTHER;
    if (!strcmp(n, "pickpocketing_take")) return H_TAKE;
    if (!strcmp(n, "pickpocketing_uncover")) return H_UNCOVER;
    if (!strcmp(n, "pickpocketing_close")) return H_CLOSE;
    return H_OTHER;
}
struct Hint { int action; bool visible, enable; };

// ---------------- what the grid offers ----------------
struct Item {
    bool present = false;
    bool uncovered = false;   // shown from the start (skill)
    float penalty = 0;        // s; taking costs penalty + 1 s of the budget
    float price = -1;         // -1 = unknown
    float weight = 0;
    int count = 0;
    char name[48] = {0};
};
struct Grid {
    float timeSec = 0, maxTimeSec = 0, selectorSpeed = 1;   // budget, full ring, slots per second
    bool easyWayOut = false;
    Item slot[NSLOT];
};

struct Config {
    // charge
    float releaseSafety = 0.35f;   // let go of the key once the ring's safety falls to this (0..1)
    float maxChargeSec = 0;        // 0 = charge as long as it is safe (the game opens the grid at its maximum)
    float abortSafety = -1;        // >= 0: give up while still below the minimum charge if safety falls to this
    // grid
    int safetyMs = 1500;           // budget always kept in hand beyond the way back to the exit
    float minValue = 0;            // do not take items worth less than this
    bool uncover = true;           // look under covered tiles when there is time
    bool fair = true;              // decide only on what the player can see (covered items are unknown)
    int lagPerSlotMs = 70;         // frame granularity and input latency per selector step
    int uncoverWaitMs = 1700;      // give up on an uncover that did not show the item by then
};

enum Cmd { C_NONE = 0, C_LEFT, C_RIGHT, C_ACTION };
inline const char* cmd_name(int c) { return c == C_LEFT ? "MoveLeft" : c == C_RIGHT ? "MoveRight" : c == C_ACTION ? "DoAction" : "-"; }

// ---------------- charge ----------------
enum ChargeDecision { CD_WAIT = 0, CD_RELEASE, CD_ABORT };
// state: C_Pickpocketing +0x78 (1 = below the minimum, 2 = above it). safety < 0: no ring value seen yet.
inline int charge_decide(const Config& c, int state, float boost, float minimum, float maximum, float safety) {
    if (state == 2) {
        if (c.maxChargeSec > 0 && boost >= c.maxChargeSec) return CD_RELEASE;
        if (safety >= 0 && safety <= c.releaseSafety) return CD_RELEASE;
        if (safety < 0 && maximum > minimum && boost >= minimum + 0.5f * (maximum - minimum)) return CD_RELEASE;  // no ring: half way
        return CD_WAIT;
    }
    if (state == 1 && c.abortSafety >= 0 && safety >= 0 && safety <= c.abortSafety) return CD_ABORT;
    return CD_WAIT;
}

// ---------------- grid controller ----------------
// The mod calls step(now) once a frame; a returned command is sent to the UI and every OnActionHintState the
// UI raises while it runs is passed to hint(); then sent(cmd, now) closes the call. A command the UI ignored
// (selector still moving) raises no hint and is simply tried again later.
enum SlotState { S_EMPTY = 0, S_HIDDEN, S_UNCOVERING, S_UNCOVER, S_COVERING, S_SELECT, S_PERM };

struct Controller {
    Config cfg; Grid g;
    typedef void (*Log_t)(const char*);
    Log_t log = nullptr;

    int pos = EXIT;                 // selector slot (confirmed by the UI)
    int st[NSLOT] = {0};
    uint64_t t0 = 0, stateAt[NSLOT] = {0};
    double spentMs = 0;             // budget spent on taken items
    int target = -1, goal = 0;      // goal: 0 none, 1 take, 2 uncover, 3 exit
    uint64_t nextTry = 0, waitUntil = 0, arriveAt = 0, closeAt = 0;
    bool done = false, closing = false;
    bool closeAck = true;           // the mod sees the UI's OnClose (else: assume an exit action worked)
    int taken = 0, uncovers = 0, moves = 0, rejected = 0;
    float value = 0;
    // during one sent command
    int callCmd = C_NONE; bool ackMove = false, ackUncover = false, ackTake = false, ackClose = false;

    void logf(const char* f, ...) {
        if (!log) return;
        char b[512]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); log(b);
    }
    // Selector step time: the UI's nominal speed until steps have been timed (the UI turns the selector in
    // whole half-degree frame steps, so the real speed depends on the frame rate and rounding).
    double measured[2] = {0, 0}; int nMeasured[2] = {0, 0};
    uint64_t lastMoveAt = 0, lastRejectAt = 0; int lastMoveDir = 0; bool retriedSinceMove = false;
    bool moving = false;            // a move was accepted and the UI has not said the selector stopped yet
    double slotMs() const { return 1000.0 / (g.selectorSpeed > 0.05f ? g.selectorSpeed : 0.05f); }
    bool fastStep(int s, int d) const { return g.easyWayOut && ((d > 0 && s >= 6) || (d < 0 && s <= 6) || (d < 0 && s == EXIT)); }
    // One step from slot s in direction d (Easy Way Out doubles the speed towards the exit).
    double stepMs(int s, int d) const {
        int f = fastStep(s, d) ? 1 : 0;
        if (nMeasured[f]) return measured[f] + 20;
        return slotMs() * (f ? 0.5 : 1.0) + cfg.lagPerSlotMs;
    }
    double travelMs(int from, int to, int d) const {
        double t = 0; int s = from;
        while (s != to) { t += stepMs(s, d); s = (s + d + NSLOT) % NSLOT; }
        return t;
    }
    int bestDir(int from, int to) const {
        if (from == to) return 0;
        return travelMs(from, to, +1) <= travelMs(from, to, -1) ? +1 : -1;
    }
    double travel(int from, int to) const { int d = bestDir(from, to); return d ? travelMs(from, to, d) : 0; }
    double homeMs(int from) const { return travel(from, EXIT); }
    double remainingMs(uint64_t now) const { return g.timeSec * 1000.0 - spentMs - (double)(now - t0); }
    static double costMs(const Item& it) { return it.penalty * 1000.0 + 1000.0; }
    bool visible(int s) const { return st[s] == S_UNCOVER || st[s] == S_PERM; }
    bool known(int s) const { return !cfg.fair || st[s] == S_PERM || st[s] == S_UNCOVER || st[s] == S_COVERING || seen[s]; }
    bool seen[NSLOT] = {false};
    float valueOf(int s) const { const Item& it = g.slot[s]; return it.price >= 0 ? it.price : 0.f; }
    double expectedCost() const {            // fair: what a covered tile probably costs, from what is visible
        double m = 0; int n = 0;
        for (int s = 1; s < NSLOT; s++) if (st[s] != S_EMPTY && known(s)) { m = fmax(m, costMs(g.slot[s])); n++; }
        return n ? fmax(m, 2500.0) : 3500.0;
    }
    double expectedValue() const {
        double v = 0; int n = 0;
        for (int s = 1; s < NSLOT; s++) if (st[s] != S_EMPTY && known(s) && g.slot[s].price >= 0) { v += g.slot[s].price; n++; }
        return n ? v / n : 10.0;
    }

    void start(const Grid& grid, uint64_t now) {
        Config c = cfg; Log_t l = log; bool ca = closeAck; *this = Controller(); cfg = c; log = l; closeAck = ca;
        g = grid; t0 = now;
        for (int s = 1; s < NSLOT; s++) {
            const Item& it = g.slot[s];
            st[s] = !it.present ? S_EMPTY : it.uncovered ? S_PERM : S_HIDDEN;
            seen[s] = it.uncovered;
        }
        int n = 0, vis = 0; for (int s = 1; s < NSLOT; s++) { n += st[s] != S_EMPTY; vis += st[s] == S_PERM; }
        logf("grid: time=%.2fs max=%.2fs selector=%.2f slots/s easyWayOut=%d items=%d visible=%d", g.timeSec, g.maxTimeSec, g.selectorSpeed, (int)g.easyWayOut, n, vis);
        for (int s = 1; s < NSLOT; s++) if (st[s] != S_EMPTY) {
            const Item& it = g.slot[s];
            logf("  slot %2d %-7s %-28s price=%.1f weight=%.2f count=%d cost=%.1fs", s, it.uncovered ? "visible" : "covered", it.name, it.price, it.weight, it.count, costMs(it) / 1000);
        }
    }

    // Pick what to do next from the slot the selector rests on: the best whole route, not the best next item.
    // Every route (any subset of the candidate tiles, in any order, then the exit) is scored by an exact DP over
    // subsets: most value first (each item also counts a little, so worthless items are still taken when they
    // fit), then least time. A route only counts if  travel + item costs + way home <= time left - SafetyMs.
    // Its first tile becomes the target; the route is planned again after every take or uncover.
    struct Node { int slot, goal; double cost, value; };
    int routeLen = 0, route[NSLOT] = {0};
    void plan(uint64_t now) {
        double rem = remainingMs(now) - cfg.safetyMs;
        Node n[NSLOT]; int k = 0;
        for (int s = 1; s < NSLOT; s++) {
            if (st[s] == S_PERM || st[s] == S_UNCOVER) {
                if (valueOf(s) < cfg.minValue) continue;
                n[k++] = Node{s, 1, costMs(g.slot[s]), valueOf(s)};
            } else if ((st[s] == S_HIDDEN || st[s] == S_COVERING) && cfg.uncover) {
                bool kn = known(s);
                if (kn && valueOf(s) < cfg.minValue) continue;
                double wait = (st[s] == S_COVERING ? 1000.0 : 0.0) + 1000.0 + 2 * cfg.lagPerSlotMs;
                n[k++] = Node{s, 2, wait + (kn ? costMs(g.slot[s]) : expectedCost()), kn ? valueOf(s) : expectedValue() * 0.7};
            }
        }
        const double INF = 1e18; const int M = 1 << k;
        static double T[1 << (NSLOT - 1)][NSLOT - 1];     // least time to cover mask, ending at node j (holding j's cost)
        static signed char P[1 << (NSLOT - 1)][NSLOT - 1];
        for (int m = 0; m < M; m++) for (int j = 0; j < k; j++) T[m][j] = INF;
        for (int j = 0; j < k; j++) { T[1 << j][j] = travel(pos, n[j].slot) + n[j].cost; P[1 << j][j] = -1; }
        int bestM = 0, bestJ = -1; double bestV = 0, bestT = homeMs(pos);
        for (int m = 1; m < M; m++) {
            double v = 0; for (int j = 0; j < k; j++) if (m >> j & 1) v += n[j].value + 1.0;
            for (int j = 0; j < k; j++) {
                double t = T[m][j]; if (t >= INF) continue;
                double total = t + homeMs(n[j].slot);
                if (total > rem) continue;                  // ends too late: no extension can fit either (times only grow)
                if (v > bestV + 1e-6 || (fabs(v - bestV) <= 1e-6 && total < bestT)) { bestV = v; bestT = total; bestM = m; bestJ = j; }
                for (int i = 0; i < k; i++) if (!(m >> i & 1)) {
                    double t2 = t + travel(n[j].slot, n[i].slot) + n[i].cost;
                    int m2 = m | 1 << i;
                    if (t2 < T[m2][i]) { T[m2][i] = t2; P[m2][i] = (signed char)j; }
                }
            }
        }
        routeLen = 0;
        for (int m = bestM, j = bestJ; j >= 0; ) { route[routeLen++] = j; int pj = P[m][j]; m &= ~(1 << j); j = pj; }
        for (int a = 0, b = routeLen - 1; a < b; a++, b--) { int t = route[a]; route[a] = route[b]; route[b] = t; }
        if (!routeLen) { target = EXIT; goal = 3; logf("plan: home (%.1fs left, %.1fs to the exit)", remainingMs(now) / 1000, homeMs(pos) / 1000); return; }
        char buf[96]; int o = 0;
        for (int r = 0; r < routeLen && o < 80; r++) o += snprintf(buf + o, sizeof buf - o, "%s%s%d", r ? " " : "", n[route[r]].goal == 2 ? "?" : "", n[route[r]].slot);
        const Node& f = n[route[0]]; target = f.slot; goal = f.goal;
        logf("plan: %s slot %d (%s) %.1fs left; route %s exit, %.1fs of %.1fs", goal == 1 ? "take" : "uncover", target, g.slot[target].name,
             remainingMs(now) / 1000, buf, bestT / 1000, (rem + cfg.safetyMs) / 1000);
        for (int r = 0; r < routeLen; r++) route[r] = n[route[r]].slot;
    }

    // Covered/uncovered timing of tiles the selector has left, as the UI animates them.
    void age(uint64_t now) {
        for (int s = 1; s < NSLOT; s++) {
            uint64_t dt = now - stateAt[s];
            if (st[s] == S_UNCOVERING && dt >= 1000 && s != pos) { st[s] = S_UNCOVER; stateAt[s] = stateAt[s] + 1000; dt = now - stateAt[s]; }
            if (st[s] == S_UNCOVER && dt >= 1000) { st[s] = S_COVERING; stateAt[s] += 1000; dt = now - stateAt[s]; }
            if (st[s] == S_COVERING && dt >= 1000) { st[s] = S_HIDDEN; stateAt[s] += 1000; }
        }
    }

    int step(uint64_t now) {
        if (done) return C_NONE;
        if (closing) {                                  // no OnClose seen: the exit action may have been ignored
            if (closeAck || now < closeAt + 1500) return C_NONE;
            closing = false; logf("still open 1.5s after the exit action - trying again");
        }
        age(now);
        if (pending(now)) return C_NONE;
        if (now < nextTry || now < waitUntil) return C_NONE;
        // Out of time on the way: head home whatever the plan.
        if (goal != 3 && remainingMs(now) < homeMs(pos) + cfg.safetyMs) { target = EXIT; goal = 3; logf("time is short (%.1fs) - home", remainingMs(now) / 1000); }
        // On the way: is the target still worth the trip with what the steps really take?
        if ((goal == 1 || goal == 2) && pos != target) {
            double cost = goal == 1 || known(target) ? costMs(g.slot[target]) : expectedCost();
            if (goal == 2) cost += 1000.0 + 2 * cfg.lagPerSlotMs;
            if (remainingMs(now) - travel(pos, target) - cost - homeMs(target) < cfg.safetyMs) { logf("slot %d no longer fits in the time left", target); goal = 0; }
        }
        if (goal == 0 || (goal == 1 && st[target] != S_PERM && st[target] != S_UNCOVER) ||
            (goal == 2 && st[target] != S_HIDDEN && st[target] != S_COVERING && st[target] != S_UNCOVERING && st[target] != S_UNCOVER))
            plan(now);
        if (pos != target) return bestDir(pos, target) > 0 ? C_RIGHT : C_LEFT;
        if (now < arriveAt) return C_NONE;                // still turning to it: an action now would be ignored
        // at the target
        if (goal == 3) return C_ACTION;
        if (goal == 1) {
            if (remainingMs(now) - costMs(g.slot[pos]) - homeMs(pos) < cfg.safetyMs) { goal = 0; plan(now); return C_NONE; }
            return C_ACTION;
        }
        if (goal == 2) {
            if (st[pos] == S_HIDDEN) return C_ACTION;
            if (st[pos] == S_COVERING) return C_NONE;          // wait until it is covered, then uncover again
            if (st[pos] == S_UNCOVERING) {
                if (now - stateAt[pos] > (uint64_t)cfg.uncoverWaitMs) { logf("slot %d: no item shown after %dms - leaving it", pos, cfg.uncoverWaitMs); st[pos] = S_SELECT; goal = 0; }
                return C_NONE;                                 // the UI tells when it shows the item
            }
            if (st[pos] == S_UNCOVER) {                       // shown: decide on what is visible now
                seen[pos] = true;
                const Item& it = g.slot[pos];
                bool worth = valueOf(pos) >= cfg.minValue;
                bool fits = remainingMs(now) - costMs(it) - homeMs(pos) >= cfg.safetyMs;
                if (worth && fits) return C_ACTION;
                logf("slot %d: %s (price %.1f, cost %.1fs) - %s", pos, it.name, it.price, costMs(it) / 1000, worth ? "no time for it" : "not worth it");
                st[pos] = S_SELECT; goal = 0;                  // never again
                return C_NONE;
            }
        }
        goal = 0; return C_NONE;
    }

    // ---- talking to the UI ----
    // Every accepted command raises a hint at once (move: take hidden; uncover: uncover hidden; take: take
    // shown but disabled). If the UI delivers its hints during our call ("sync", what CryEngine normally does)
    // a command without one was ignored and is retried next frame; if they turn up later ("async") the
    // command stays pending until its hint arrives or pendingMs passes.
    bool sync = false;                       // an in-call hint has been seen
    // A pending command times out only after pendingMs AND a few ticks (a hitch must not eat the wait). Its
    // hint can still turn up after that: until the next command it is kept as lateCmd and counted then.
    int pendCmd = C_NONE, pendTicks = 0, lateCmd = C_NONE; uint64_t pendAt = 0, lateAt = 0;
    static const int pendingMs = 150, pendingTicks = 3;

    void begin(int cmd) { callCmd = cmd; lateCmd = C_NONE; ackMove = ackUncover = ackTake = ackClose = false; }
    void closed(uint64_t now, bool ok) {               // the UI's OnClose (or the minigame ended)
        if (callCmd != C_NONE) ackClose = true;
        if (!done) logf("closed by the game: %s, taken=%d value=%.1f, %.1fs of budget left", ok ? "success" : "CAUGHT (time ran out away from the exit)", taken, value, remainingMs(now) / 1000);
        done = true;
    }
    bool classify(int cmd, const Hint& h, bool& mv, bool& un, bool& tk) const {
        if ((cmd == C_LEFT || cmd == C_RIGHT) && h.action == H_TAKE && !h.visible && !h.enable) { mv = true; return true; }
        if (cmd == C_ACTION && h.action == H_UNCOVER && !h.visible && !h.enable) { un = true; return true; }
        if (cmd == C_ACTION && h.action == H_TAKE && h.visible && !h.enable && !un) { tk = true; return true; }
        return false;
    }
    // A hint the UI raised (during our call, or later on its own).
    void hint(const Hint& h, uint64_t now) {
        if (callCmd != C_NONE) { if (classify(callCmd, h, ackMove, ackUncover, ackTake)) sync = true; return; }
        if (pendCmd != C_NONE) {
            bool mv = false, un = false, tk = false;
            if (classify(pendCmd, h, mv, un, tk)) { int c = pendCmd; pendCmd = C_NONE; accept(c, mv, un, tk, pendAt, now); return; }
        }
        if (lateCmd != C_NONE) {
            bool mv = false, un = false, tk = false;
            if (classify(lateCmd, h, mv, un, tk)) {
                int c = lateCmd; lateCmd = C_NONE; rejected--; retriedSinceMove = false;
                logf("late hint %.0fms after the command - it went through after all", (double)(now - lateAt));
                accept(c, mv, un, tk, lateAt, now); return;
            }
        }
        // On its own: the selector came to rest on the slot of the last move. The UI says so on the exit and on
        // every slot holding an item (close / uncover / take, visible and enabled), not on empty slots. That is
        // the moment the next command is accepted, and the exact time the step took.
        if (moving && h.visible && h.enable && pendCmd == C_NONE && lateCmd == C_NONE &&
            ((pos == EXIT && h.action == H_CLOSE) || (pos != EXIT && st[pos] != S_EMPTY && (h.action == H_UNCOVER || h.action == H_TAKE)))) {
            moving = false;
            int d = lastMoveDir, f = fastStep((pos - d + NSLOT) % NSLOT, d) ? 1 : 0;
            double nom = slotMs() * (f ? 0.5 : 1.0), smp = (double)(now - lastMoveAt);
            if (smp >= nom * 0.6 && smp <= nom * 1.6 + 60) {
                measured[f] = nMeasured[f] ? 0.7 * measured[f] + 0.3 * smp : smp;
                if (!nMeasured[f]++) logf("selector step measured: %.0fms%s (nominal %.0fms)", smp, f ? " towards the exit" : "", nom);
            }
            if (nextTry > now) nextTry = now;
            if (arriveAt > now) arriveAt = now;
            if (!(st[pos] == S_UNCOVERING && h.action == H_TAKE)) return;
        }
        // On its own: the tile under the selector finished uncovering.
        if (h.action == H_TAKE && h.visible && h.enable && pos != EXIT && st[pos] == S_UNCOVERING) {
            logf("slot %d uncovered after %.0fms: %s price=%.1f cost=%.1fs", pos, (double)(now - stateAt[pos]), g.slot[pos].name, g.slot[pos].price, costMs(g.slot[pos]) / 1000);
            st[pos] = S_UNCOVER; stateAt[pos] = now; seen[pos] = true;
        }
    }
    void sent(uint64_t now) {
        int cmd = callCmd; callCmd = C_NONE;
        if (cmd == C_ACTION && pos == EXIT) {
            if (closeAck && sync && !ackClose && !done) { reject(cmd, now); return; }
            closing = true; closeAt = now; logf("exit: taken=%d value=%.1f %.1fs left", taken, value, remainingMs(now) / 1000); return;
        }
        if (ackMove || ackUncover || ackTake) { accept(cmd, ackMove, ackUncover, ackTake, now, now); return; }
        if (sync) { reject(cmd, now); return; }
        pendCmd = cmd; pendAt = now; pendTicks = 0;     // wait for a late hint
    }
    bool pending(uint64_t now) {                       // step(): still waiting for a late hint?
        if (pendCmd == C_NONE) return false;
        if (++pendTicks < pendingTicks || now - pendAt < (uint64_t)pendingMs) return true;
        int c = pendCmd; pendCmd = C_NONE; lateCmd = c; lateAt = pendAt; logf("no hint %.0fms after the command - treated as ignored", (double)(now - pendAt)); reject(c, now, pendAt); return false;
    }
    void reject(int cmd, uint64_t now, uint64_t sentAt = 0) {
        rejected++; nextTry = now + (sync ? 16 : 40);
        if (cmd == C_LEFT || cmd == C_RIGHT) { retriedSinceMove = true; lastRejectAt = sentAt ? sentAt : now; }
    }
    void accept(int cmd, bool mv, bool un, bool tk, uint64_t at, uint64_t now) {
        if (mv) {
            int d = cmd == C_RIGHT ? 1 : -1;
            // The previous step, timed: it ended between the last refused try and this accepted one. Only a
            // tight bracket counts (a hitch or a slow retry would overstate it), and a sample far from the
            // nominal speed is dropped rather than trusted: an overstated step slows the mod down for nothing.
            if (lastMoveDir == d && retriedSinceMove && lastMoveAt && lastRejectAt > lastMoveAt && at >= lastRejectAt && at - lastRejectAt <= 60) {
                int f = fastStep((pos - d + NSLOT) % NSLOT, d) ? 1 : 0;
                double nom = slotMs() * (f ? 0.5 : 1.0);
                double smp = 0.5 * (double)(lastRejectAt - lastMoveAt) + 0.5 * (double)(at - lastMoveAt);
                if (smp >= nom * 0.6 && smp <= nom * 1.6 + 60) {
                    measured[f] = nMeasured[f] ? 0.7 * measured[f] + 0.3 * smp : smp;
                    if (!nMeasured[f]++) logf("selector step measured: %.0fms%s (nominal %.0fms)", smp, f ? " towards the exit" : "", nom);
                }
            }
            double ms = stepMs(pos, d); pos = (pos + d + NSLOT) % NSLOT; moves++;
            double nomStep = slotMs() * (fastStep((pos - d + NSLOT) % NSLOT, d) ? 0.5 : 1.0);
            lastMoveAt = at; lastMoveDir = d; retriedSinceMove = false; moving = true;
            nextTry = at + (uint64_t)(fmin(ms, nomStep) * (sync ? 0.7 : 0.9)); arriveAt = at + (uint64_t)ms;   // early retries are harmless
            // ... except when hints come after the call (the game on 2026-10-17): then every refused try waits
            // pendingMs for nothing. On the exit and on a slot with an item the UI says when the selector stops,
            // so wait for that (with a late fallback in case it never comes); elsewhere (also on slots already taken)
            // try once it should be there.
            if (!sync) nextTry = at + (uint64_t)(pos == EXIT || (st[pos] != S_EMPTY && st[pos] != S_SELECT) ? nomStep * 1.6 + 60 : ms);
            return;
        }
        if (un) { st[pos] = S_UNCOVERING; stateAt[pos] = at; uncovers++; logf("slot %d: uncovering", pos); return; }
        if (tk) {
            spentMs += costMs(g.slot[pos]); st[pos] = S_SELECT; taken++; value += valueOf(pos);
            logf("slot %d: took %s (price %.1f, cost %.1fs) %.1fs left", pos, g.slot[pos].name, g.slot[pos].price, costMs(g.slot[pos]) / 1000, remainingMs(now) / 1000);
            goal = 0; waitUntil = now + 120;
        }
    }
};

}  // namespace pp
