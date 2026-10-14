// ppsim.cpp -- offline test of Theatrical AutoPickpocket's grid logic (src/pp_logic.h) against a model of the
// game's pickpocketing UI, transcribed from Pickpocketing.gfx (build 15693): updatePickpocket, fc_moveLeft/
// fc_moveRight/fc_doAction, the 1 s uncover/show/cover cycle, the take penalty spread over 5 frames, the
// time-out rule (success only on the exit slot) and Easy Way Out. The selector turns in whole half-degree
// frame steps like the movie (60 fps, frame time jittered, occasional hitches).
// Build: g++ -O2 -std=c++17 -Isrc tools/ppsim.cpp -o ppsim     (or cl /O2 /EHsc /Isrc tools\ppsim.cpp)
// Usage: ppsim [runs=2000] [seed=1] [-v]        exit code 1 if any run got caught
#include "pp_logic.h"
#include <cstdlib>
#include <vector>
#include <deque>
#include <string>
#include <random>
#include <algorithm>

using namespace pp;

struct Flash {                                            // the UI side, one movie frame at a time
    enum { HIDDEN = 1, UNCOVERING = 2, COVERING = 3, UNCOVER = 4, SELECT = 5, PERM = 6 };
    struct It { bool present = false; int state = 0; double cover = 1; double timePenalty = 0; };
    It it[NSLOT];
    double m_time = 0, g_maxTime = 0, selectorSpeed = 1, rot = 0; bool easy = false;
    std::deque<int> q; int dir = 0;
    double origin = 0, prev = 0;
    double tpLeft = 0; int tpCounter = -1;                // take penalty animation (timeProgress.onEnterFrame)
    bool closed = false, result = false; std::string closedItems;
    std::vector<Hint>* sink = nullptr;
    void hint(int a, bool vis, bool en) { if (sink) sink->push_back(Hint{a, vis, en}); }

    void open(const Grid& g, double now) {
        m_time = g.timeSec * 1000; g_maxTime = g.maxTimeSec * 1000; selectorSpeed = g.selectorSpeed; easy = g.easyWayOut;
        for (int s = 1; s < NSLOT; s++) {
            it[s] = It(); it[s].present = g.slot[s].present;
            if (!it[s].present) continue;
            it[s].timePenalty = g.slot[s].penalty * 1000 + 1000;
            it[s].state = g.slot[s].uncovered ? PERM : HIDDEN; it[s].cover = g.slot[s].uncovered ? 180 : 1;
        }
        q.clear(); q.push_back(EXIT); rot = 0; dir = 0; origin = prev = now; closed = false;
    }
    void close(bool ok) {
        if (closed) return;
        closed = true; result = ok; closedItems.clear();
        for (int s = 1; s < NSLOT; s++) if (it[s].present && it[s].state == SELECT) closedItems += std::to_string(s) + "|";
    }
    void moveRight() {
        if (q.size() != 1) return;
        int r1 = q.back(); hint(H_TAKE, false, false);
        if (r1 == 11) { hint(H_UNCOVER, false, false); hint(H_CLOSE, true, false); } else { hint(H_CLOSE, false, false); hint(H_UNCOVER, true, false); }
        r1++; if (r1 > 11) r1 = 0; q.push_back(r1);
    }
    void moveLeft() {
        if (q.size() != 1) return;
        int r1 = q.back(); hint(H_TAKE, false, false);
        if (r1 == 1) { hint(H_UNCOVER, false, false); hint(H_CLOSE, true, false); } else { hint(H_CLOSE, false, false); hint(H_UNCOVER, true, false); }
        r1--; if (r1 < 0) r1 = 11; q.push_back(r1);
    }
    void doAction() {
        if (q.size() != 1) return;
        int s = q[0];
        if (s == EXIT) { close(true); return; }
        if (!it[s].present) return;
        if (it[s].state == HIDDEN) { hint(H_UNCOVER, false, false); hint(H_TAKE, true, false); it[s].state = UNCOVERING; return; }
        if (it[s].state == UNCOVER || it[s].state == PERM) {
            it[s].state = SELECT; it[s].cover = 180;
            tpLeft = it[s].timePenalty; tpCounter = 0;
            hint(H_TAKE, true, false);
        }
    }
    void frame(double now) {                              // updatePickpocket + the penalty onEnterFrame
        if (closed) return;
        if (tpCounter >= 0) {
            tpLeft /= 2; m_time -= tpLeft; tpCounter++;
            if (tpCounter == 5) { m_time -= tpLeft; tpCounter = -1; }
        }
        double r4 = now;
        if (r4 - origin >= m_time) { close(q.size() == 1 && q[0] == EXIT); return; }
        if (q.size() > 1) {
            double r9 = (r4 - prev) / 1000 * 30 * selectorSpeed;
            if (easy) {
                bool c = (dir == 1 && !(q[0] < 6)) || (dir == -1 && !(q[0] > 6)) || (q[1] == 11 && q[0] == EXIT);
                if (c) r9 *= 2;
            }
            double r7 = rot; if (r7 < 0) r7 += 360;
            double r11 = std::min(fabs(360 - r7 - q[1] * 30), fabs(r7 - q[1] * 30));
            if (r11 < r9 * 2) {
                q.pop_front(); rot = q[0] * 30; dir = 0;
                if (q.size() == 1) {
                    int s = q[0];
                    if (s == EXIT) hint(H_CLOSE, true, true);
                    else if (it[s].present && it[s].state == HIDDEN) hint(H_UNCOVER, true, true);
                    else if (it[s].present && (it[s].state == UNCOVER || it[s].state == PERM)) hint(H_TAKE, true, true);
                }
            } else {
                if (dir == 0) {
                    dir = q[1] - q[0] > 0 ? 1 : -1;
                    if ((q[1] == 11 && q[0] == EXIT) || (q[1] == 0 && q[0] == 11)) dir = -dir;
                }
                double step = std::round(r9 * 2 * dir);   // half-degree frames
                rot += step / 2; if (rot >= 360) rot -= 360; if (rot < 0) rot += 360;
            }
        }
        for (int s = 1; s < NSLOT; s++) {
            It& x = it[s];
            if (!x.present || !(x.state == UNCOVERING || x.state == UNCOVER || x.state == COVERING)) continue;
            double r2 = std::round((r4 - prev) / 1000 * 60 * 2) + x.cover;
            bool on = q.size() == 1 && q[0] == s;
            if (x.state == UNCOVERING) { if (r2 >= 120) { x.state = UNCOVER; if (on) hint(H_TAKE, true, true); } }
            else if (x.state == UNCOVER) { if (r2 >= 240) { x.state = COVERING; if (on) { hint(H_TAKE, false, false); hint(H_UNCOVER, true, false); } } }
            else if (x.state == COVERING) { if (r2 >= 360) { x.state = HIDDEN; r2 = 1; if (on) hint(H_UNCOVER, true, true); } }
            x.cover = r2;
        }
        prev = r4;
    }
};

static bool g_verbose = false; static int g_only = -1;
static void logsink(const char* s) { if (g_verbose) printf("    %s\n", s); }

struct Result { bool caught; int taken; float value, possible; double left; };

static Result run(std::mt19937& rng, const Config& cfg, bool verbose, double fps, bool hitches, bool asyncHints) {
    std::uniform_real_distribution<double> U(0, 1);
    Grid g;
    g.selectorSpeed = (float)(0.7 + 2.0 * U(rng));
    g.maxTimeSec = (float)(6 + 10 * U(rng));
    g.timeSec = (float)(g.maxTimeSec * (0.3 + 0.7 * U(rng)));
    g.easyWayOut = U(rng) < 0.4;
    int n = 2 + (int)(9 * U(rng)), vis = (int)(n * U(rng));
    std::vector<int> slots; for (int s = 1; s < NSLOT; s++) slots.push_back(s);
    std::shuffle(slots.begin(), slots.end(), rng);
    float possible = 0;
    for (int k = 0; k < n; k++) {
        Item& it = g.slot[slots[k]]; it.present = true; it.uncovered = k < vis;
        it.weight = (float)(U(rng) < 0.3 ? 0.1 : 3 * U(rng) * U(rng));
        it.penalty = it.weight * 0.021f * 40 * (float)U(rng);   // weight * UncoverTimePerWeight * count-ish
        it.price = (float)(U(rng) < 0.2 ? 0.5 + 2 * U(rng) : 2 + 60 * U(rng) * U(rng));
        it.count = 1; snprintf(it.name, sizeof it.name, "item%d", slots[k]);
        possible += it.price;
    }
    Flash f; std::vector<Hint> hints; f.sink = &hints;
    Controller c; c.cfg = cfg; c.log = verbose ? logsink : nullptr; c.closeAck = !asyncHints;
    double now = 1000; f.open(g, now); c.start(g, (uint64_t)now);
    std::deque<std::pair<double, Hint>> late;              // async mode: hints arrive a frame later
    for (int frames = 0; frames < 20000 && !f.closed; frames++) {
        double dt = 1000.0 / fps * (0.85 + 0.3 * U(rng));
        if (hitches && U(rng) < 0.01) dt += 50 + 250 * U(rng);
        now += dt;
        // the game frame: the mod's tick (Update hook) ...
        while (!late.empty() && late.front().first <= now) { c.hint(late.front().second, (uint64_t)now); late.pop_front(); }
        int cmd = c.step((uint64_t)now);
        if (cmd != C_NONE) {
            c.begin(cmd); hints.clear();
            size_t qb = f.q.size();
            if (cmd == C_LEFT) f.moveLeft(); else if (cmd == C_RIGHT) f.moveRight(); else f.doAction();
            if (verbose) printf("      t=%.0f %s -> %s (flash slot %d, model %d)\n", now - 1000, cmd_name(cmd), f.q.size() != qb || f.closed || !hints.empty() ? "accepted" : "ignored", f.q.back(), c.pos);
            if (f.closed) c.closed((uint64_t)now, f.result);
            if (!asyncHints) for (auto& h : hints) c.hint(h, (uint64_t)now);
            else for (auto& h : hints) late.push_back({now + dt * 0.5, h});
            hints.clear();
            c.sent((uint64_t)now);
        }
        // ... then the movie frame
        f.frame(now);
        if (f.closed) c.closed((uint64_t)now, f.result);
        if (!asyncHints) { for (auto& h : hints) c.hint(h, (uint64_t)now); }
        else for (auto& h : hints) late.push_back({now + dt * 0.5, h});
        hints.clear();
    }
    Result r{!f.result, c.taken, c.value, possible, f.m_time - (now - f.origin)};
    if (verbose) { printf("  flash: queue="); for (int x : f.q) printf("%d ", x); printf("rot=%.1f dir=%d model pos=%d\n", f.rot, f.dir, c.pos); }
    if (verbose) printf("  -> %s taken=%d value=%.1f/%.1f left=%.0fms\n", f.result ? "success" : "CAUGHT", c.taken, c.value, possible, r.left);
    return r;
}

int main(int argc, char** argv) {
    int runs = argc > 1 ? atoi(argv[1]) : 2000; unsigned seed = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-v")) g_verbose = true; if (!strncmp(argv[i], "-run=", 5)) { g_verbose = true; g_only = atoi(argv[i] + 5); } }
    struct Variant { const char* name; double fps; bool hitches, async, fair; } vs[] = {
        {"60fps", 60, false, false, true}, {"60fps+hitches", 60, true, false, true}, {"30fps+hitches", 30, true, false, true},
        {"45fps async-hints", 45, true, true, true}, {"60fps omniscient", 60, false, false, false},
    };
    int bad = 0;
    for (auto& v : vs) {
        std::mt19937 rng(seed);
        Config cfg; cfg.fair = v.fair;
        int caught = 0, taken = 0; double value = 0, possible = 0, left = 0;
        for (int k = 0; k < runs; k++) {
            bool verbose = g_verbose && (k < 3 || k == g_only);
            if (verbose) printf("[%s run %d]\n", v.name, k);
            Result r = run(rng, cfg, verbose, v.fps, v.hitches, v.async);
            if (r.caught && !g_verbose) printf("  caught: %s run %d\n", v.name, k);
            caught += r.caught; taken += r.taken; value += r.value; possible += r.possible; left += r.left;
        }
        printf("%-20s runs=%d caught=%d items/run=%.2f value=%.1f%% of all  budget left avg=%.2fs\n",
               v.name, runs, caught, (double)taken / runs, 100 * value / possible, left / runs / 1000);
        bad += caught;
    }
    return bad ? 1 : 0;
}
