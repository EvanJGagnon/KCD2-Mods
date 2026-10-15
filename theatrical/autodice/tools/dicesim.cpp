// dicesim.cpp -- offline tests and game simulator for the Theatrical AutoDice engine.
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
//   dicesim test                 scoring rules and engine sanity checks (exit code 1 on failure)
//   dicesim duel [games] [seed]  engine vs. simple strategies over whole simulated games
//   dicesim show <faces...>      what the engine does with one throw, e.g.  dicesim show 1 5 5 3 2 6
//
// Builds anywhere:  g++ -O2 -std=c++17 -Isrc tools/dicesim.cpp -o dicesim   (or cl /O2 /EHsc /std:c++17)
#include "dice_engine.h"
#include "dice_turn.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <functional>

using namespace dice;

static int fails = 0;
static void expect(const char* what, int got, int want) {
    if (got != want) { printf("FAIL %-28s got %d want %d\n", what, got, want); fails++; }
}
static int sc(std::initializer_list<int> faces) {       // faces 1..6 as printed on the die, 0 = joker
    int c[6] = {0}, j = 0;
    for (int f : faces) { if (f == 0) j++; else c[f - 1]++; }
    return scorer().score(c, j);
}

// ---------------- the mod's per-throw bookkeeping against a model of the game's dice ----------------
static std::vector<TableDie> tbl(std::initializer_list<int> faces, std::initializer_list<int> locked) {
    std::vector<TableDie> t; int id = 0;
    for (int f : faces) { TableDie d; d.die.id = id; d.die.face = f == 0 ? JOKER : f - 1; t.push_back(d); id++; }
    for (int i : locked) t[i].state = LOCKED;
    return t;
}
static bool has(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }
// The game's scorer: only dice in state HELD count (delegate vtbl[10] collects state == 1).
static int game_score(const std::vector<TableDie>& t) {
    int c[6] = {0}, j = 0, k = 0;
    for (auto& d : t) if (d.state == HELD) { k++; if (d.die.face == JOKER) j++; else c[d.die.face]++; }
    return k ? scorer().score(c, j) : NEG;
}
// Drive the hold steps like the plugin does; the game refuses to change a LOCKED die. Returns steps or -1.
static int drive_holds(std::vector<TableDie>& t, const std::vector<int>& want, int& lockedTouches) {
    int free = 0; for (auto& d : t) free += d.state != LOCKED;
    for (int tries = 0; tries <= 3 * free + 6; tries++) {
        HoldStep st = next_hold(t, want);
        if (st.index < 0) return tries;
        if (t[st.index].state == LOCKED) { lockedTouches++; continue; }
        t[st.index].state = st.hold ? HELD : FREE;
    }
    return -1;
}
static void turn_tests() {
    // Round-1 log, 19:26:47: [1# 1 3 3 2 1], turn 100, total 600, target 1500. The locked 1 is not in play:
    // the plan may only use the two thrown 1s (the game scored them 200, not 1000).
    {
        auto t = tbl({1, 1, 3, 3, 2, 1}, {0});
        Context cx; cx.total = 600; cx.target = 1500;
        ThrowView v = view_of(t);
        expect("log A: thrown dice", (int)v.free.size(), 5);
        Decision d = decide(cx, v.free, v.aside, 100);
        auto want = wanted_ids(v, d);
        expect("log A: locked die not planned", has(want, 0), 0);
        int touches = 0; int steps = drive_holds(t, want, touches);
        expect("log A: holds converge", steps >= 0, 1);
        expect("log A: no locked die touched", touches, 0);
        expect("log A: game score == plan score", game_score(t), d.score);
        expect("log A: plan uses only thrown 1s (100 or 200)", d.score == 100 || d.score == 200, 1);
    }
    // Round-1 log, 19:27:14: [3 3 5# 3 3 2], turn 50, totals 900/900, target 1500. It tried to release the
    // locked 5 22 times. Now: hold the four 3s (600) and never touch die 2.
    {
        auto t = tbl({3, 3, 5, 3, 3, 2}, {2});
        Context cx; cx.total = 900; cx.opp = 900; cx.target = 1500;
        ThrowView v = view_of(t);
        Decision d = decide(cx, v.free, v.aside, 50);
        auto want = wanted_ids(v, d);
        expect("log B: locked 5 not planned", has(want, 2), 0);
        int touches = 0; int steps = drive_holds(t, want, touches);
        expect("log B: holds converge", steps >= 0, 1);
        expect("log B: no locked die touched", touches, 0);
        expect("log B: plan = four 3s", d.score, 600);
        expect("log B: game agrees", game_score(t), 600);
        expect("log B: 600 + 50 + 900 wins, so pass", d.pass, 1);
    }
    // A die the player held by hand before pressing the key (state HELD) is still free to release.
    {
        auto t = tbl({2, 2, 3, 4, 6, 1}, {});
        t[0].state = HELD;
        std::vector<int> want = {5};
        int touches = 0; drive_holds(t, want, touches);
        expect("manual hold released", t[0].state, FREE);
        expect("1 held", t[5].state, HELD);
    }
    // The game scores a set differently (a badge): pass / throw is decided again with the game's count.
    {
        auto t = tbl({1, 2, 3, 4, 6, 6}, {});
        Context cx; cx.total = 0; cx.target = 4000;
        ThrowView v = view_of(t); Decision d = decide(cx, v.free, v.aside, 0);
        expect("badge: reaching the target passes", pass_with(cx, v, d, 0, 4000), 1);
        expect("badge: 100 at the start throws on", pass_with(cx, v, d, 0, 100), 0);
    }
    // Whole turns against the game model: free dice are thrown, held dice become locked on "throw again",
    // all six come back after hot dice. Engine and game must agree on every set, and no locked die is touched.
    {
        std::mt19937_64 r(7); int mism = 0, touches = 0, stuck = 0, hot = 0, turns = 0;
        auto throwf = [&](std::vector<TableDie>& t) { for (auto& d : t) if (d.state == FREE) d.die.face = (int)(r() % 6); };
        for (int game = 0; game < 300; game++) {
            int total = 0, opp = (int)(r() % 30) * 50;
            for (int tn = 0; tn < 40 && total < 4000; tn++) {
                std::vector<TableDie> t(6); for (int i = 0; i < 6; i++) t[i].die.id = 100 + i;
                if (tn % 3 == 2) t[1].die.jokerFace0 = true;
                throwf(t); for (auto& d : t) if (d.die.jokerFace0 && d.die.face == 0) d.die.face = JOKER;
                int turn = 0; turns++;
                for (int g = 0; g < 50; g++) {
                    Context cx; cx.total = total; cx.opp = opp; cx.target = 4000;
                    ThrowView v = view_of(t);
                    Decision d = decide(cx, v.free, v.aside, turn);
                    if (d.bust) break;
                    auto want = wanted_ids(v, d);
                    if (drive_holds(t, want, touches) < 0) { stuck++; break; }
                    int gs = game_score(t);
                    if (gs != d.score) { mism++; break; }
                    turn += gs;
                    if (d.pass) { total += turn; break; }
                    for (auto& x : t) if (x.state == HELD) x.state = LOCKED;
                    bool all = std::all_of(t.begin(), t.end(), [](const TableDie& x) { return x.state == LOCKED; });
                    if (all) { hot++; for (auto& x : t) x.state = FREE; }
                    throwf(t); for (auto& x : t) if (x.state == FREE && x.die.jokerFace0 && x.die.face == 0) x.die.face = JOKER;
                }
            }
        }
        expect("model: engine and game scores agree", mism, 0);
        expect("model: locked dice never touched", touches, 0);
        expect("model: holds always converge", stuck, 0);
        expect("model: hot dice happened", hot > 0, 1);
        printf("game model: %d turns, %d hot-dice rethrows\n", turns, hot);
    }
}

static void tests() {
    expect("1", sc({1}), 100);
    expect("5", sc({5}), 50);
    expect("2 (no score)", sc({2}), NEG);
    expect("1 5", sc({1, 5}), 150);
    expect("1 1 1", sc({1, 1, 1}), 1000);
    expect("2 2 2", sc({2, 2, 2}), 200);
    expect("6 6 6", sc({6, 6, 6}), 600);
    expect("2 2 2 2", sc({2, 2, 2, 2}), 400);
    expect("4x5 (5 5 5 5)", sc({5, 5, 5, 5}), 1000);
    expect("6x1", sc({1, 1, 1, 1, 1, 1}), 8000);
    expect("1-5", sc({1, 2, 3, 4, 5}), 500);
    expect("2-6", sc({2, 3, 4, 5, 6}), 750);
    expect("1-6", sc({1, 2, 3, 4, 5, 6}), 1500);
    expect("1-5 + 1", sc({1, 2, 3, 4, 5, 1}), 600);
    expect("1-5 + 2 (2 unused)", sc({1, 2, 3, 4, 5, 2}), NEG);
    expect("3 3 3 + 1", sc({3, 3, 3, 1}), 400);
    expect("joker alone", sc({0}), 100);
    expect("joker 2 2", sc({0, 2, 2}), 200);
    expect("joker 2 3 4 5", sc({0, 2, 3, 4, 5}), 750);
    expect("joker 1 1 1", sc({0, 1, 1, 1}), 2000);
    expect("1 1 5 5 (two singles each)", sc({1, 1, 5, 5}), 300);

    // table sanity: one die -> keeps only for 1, 5 and joker
    int n1 = 0; for (auto& o : tables().out[1]) n1 += !o.keeps.empty();
    expect("scoring single-die outcomes", n1, 3);

    // a bust is a bust
    Context cx; cx.target = 4000;
    std::vector<Die> t(4); int f4[4] = {1, 2, 3, 5}; for (int i = 0; i < 4; i++) t[i].face = f4[i];
    Decision d = decide(cx, t, {}, 0);
    expect("2 3 4 6 busts", d.bust, 1);
    // win when possible: 1 1 1 with 1000 needed -> pass
    cx.total = 3000; std::vector<Die> w(3); for (auto& x : w) x.face = 0;
    d = decide(cx, w, {}, 0);
    expect("winning throw passes", d.pass, 1);
    expect("winning throw holds all", (int)d.hold.size(), 3);
    // start of a game, 1 5 and four junk: keep only the 1 and throw again
    cx.total = 0; std::vector<Die> s(6); int f6[6] = {0, 4, 1, 2, 3, 5}; for (int i = 0; i < 6; i++) s[i].face = f6[i];
    s[2].face = 1; s[3].face = 2; s[4].face = 2; s[5].face = 5;   // 1 5 2 3 3 6
    d = decide(cx, s, {}, 0);
    expect("1 5 2 3 3 6: keep one die", (int)d.hold.size(), 1);
    expect("1 5 2 3 3 6: keep the 1", s[d.hold[0]].face, 0);
    expect("1 5 2 3 3 6: throw again", d.pass, 0);
    turn_tests();
    printf("%s (%d failures)\n", fails ? "TESTS FAILED" : "all tests passed", fails);
}

// ---------------- simulated games ----------------
static std::mt19937_64 rng;
static int roll(const Die& d) {
    double s = 0; for (float w : d.w) s += w;
    double r = std::uniform_real_distribution<double>(0, s)(rng);
    for (int i = 0; i < 6; i++) { r -= d.w[i]; if (r < 0) return (i == 0 && d.jokerFace0) ? JOKER : i; }
    return 5;
}
using Strategy = std::function<Decision(const Context&, const std::vector<Die>&, const std::vector<Die>&, int)>;

// Greedy: keep the highest-scoring set (most points), bank once the turn reaches `stopAt` or <= `minDice` left.
static Strategy greedy(int stopAt, int minDice) {
    return [=](const Context& cx, const std::vector<Die>& tbl, const std::vector<Die>&, int turn) {
        Decision best; best.bust = true; int n = (int)tbl.size();
        for (int m = 1; m < (1 << n); m++) {
            int c[6] = {0}, j = 0, k = 0;
            for (int i = 0; i < n; i++) if (m >> i & 1) { k++; if (tbl[i].face == JOKER) j++; else c[tbl[i].face]++; }
            int s = scorer().score(c, j);
            if (s > 0 && (best.bust || s > best.score)) {
                best.bust = false; best.score = s; best.hold.clear();
                for (int i = 0; i < n; i++) if (m >> i & 1) best.hold.push_back(i);
            }
        }
        if (!best.bust) {
            int t = turn + best.score, left = n - (int)best.hold.size();
            best.pass = t >= cx.need() || t >= stopAt || (left && left <= minDice);
        }
        return best;
    };
}

struct Stats { long turns = 0, points = 0, busts = 0; };
// One turn; returns points banked.
static int play_turn(const Strategy& st, Context cx, const std::vector<Die>& set, Stats& s) {
    std::vector<int> onTable; for (int i = 0; i < 6; i++) onTable.push_back(i);
    std::vector<Die> asideDice; int turn = 0;
    s.turns++;
    for (int guard = 0; guard < 200; guard++) {
        std::vector<Die> tbl; for (int i : onTable) { Die d = set[i]; d.face = roll(d); tbl.push_back(d); }
        Decision d = st(cx, tbl, asideDice, turn);
        if (d.bust) { s.busts++; return 0; }
        turn += d.score;
        std::vector<int> rest;
        for (size_t i = 0; i < onTable.size(); i++) {
            bool held = std::find(d.hold.begin(), d.hold.end(), (int)i) != d.hold.end();
            if (held) asideDice.push_back(tbl[i]); else rest.push_back(onTable[i]);
        }
        if (d.pass) { s.points += turn; return turn; }
        onTable = rest;
        if (onTable.empty()) { for (int i = 0; i < 6; i++) onTable.push_back(i); asideDice.clear(); }
    }
    return turn;
}
// Whole game, players alternate; returns 0 if player A wins.
static int play_game(const Strategy& a, const Strategy& b, const std::vector<Die>& setA, const std::vector<Die>& setB,
                     int target, bool aFirst, Stats& sa, Stats& sb, const Dist& oppDist) {
    int score[2] = {0, 0}; int p = aFirst ? 0 : 1;
    for (int turns = 0; turns < 400; turns++) {
        Context cx; cx.target = target; cx.total = score[p]; cx.opp = score[1 - p];
        cx.oppThreat = win_this_turn(target - cx.opp, oppDist);
        score[p] += play_turn(p == 0 ? a : b, cx, p == 0 ? setA : setB, p == 0 ? sa : sb);
        if (score[p] >= target) return p;
        p = 1 - p;
    }
    return score[0] >= score[1] ? 0 : 1;
}

static std::vector<Die> dice_set(const char* kind) {
    std::vector<Die> s(6);
    if (!strcmp(kind, "lucky")) {   // a mixed bag a player might carry: two weighted toward 1s/5s, one devil
        float lucky[6] = {6, 1, 2, 3, 4, 6}; for (int i = 0; i < 2; i++) memcpy(s[i].w, lucky, sizeof lucky);
        s[2].jokerFace0 = true;
    }
    return s;
}

static void duel(int games, uint64_t seed) {
    rng.seed(seed);
    struct Opp { const char* name; Strategy s; } opps[] = {
        {"greedy, bank at 300", greedy(300, 0)},
        {"greedy, bank at 500 or <=2 dice", greedy(500, 2)},
        {"greedy, bank at 1000", greedy(1000, 0)},
    };
    Strategy engine = [](const Context& cx, const std::vector<Die>& t, const std::vector<Die>& a, int turn) { return decide(cx, t, a, turn); };
    for (const char* kind : {"normal", "lucky"}) for (int target : {2000, 4000}) for (auto& o : opps) {
        std::vector<Die> mine = dice_set(kind), theirs = dice_set("normal");
        std::vector<const Die*> tp; for (auto& d : theirs) tp.push_back(&d);
        Dist od = avg_dist(tp);
        Stats se, so; int wins = 0;
        for (int g = 0; g < games; g++) wins += play_game(engine, o.s, mine, theirs, target, g % 2 == 0, se, so, od) == 0;
        printf("%-6s dice, target %4d vs %-32s engine wins %5.1f%%  (pts/turn %4.0f vs %4.0f, busts %4.1f%% vs %4.1f%%)\n",
               kind, target, o.name, 100.0 * wins / games, (double)se.points / se.turns, (double)so.points / so.turns,
               100.0 * se.busts / se.turns, 100.0 * so.busts / so.turns);
        fflush(stdout);
    }
}

static void show(int argc, char** argv) {
    std::vector<Die> t;
    for (int i = 0; i < argc; i++) { Die d; int f = atoi(argv[i]); d.face = f == 0 ? JOKER : f - 1; t.push_back(d); }
    Context cx; Decision d = decide(cx, t, {}, 0);
    if (d.bust) { printf("bust\n"); return; }
    printf("hold:"); for (int i : d.hold) printf(" %d", t[i].face == JOKER ? 0 : t[i].face + 1);
    printf("  (%d points) then %s   [pass %.0f / throw %.0f]\n", d.score, d.pass ? "PASS" : "THROW", d.passValue, d.throwValue);
}

int main(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "test";
    if (cmd == "test") { tests(); return fails ? 1 : 0; }
    if (cmd == "duel") { duel(argc > 2 ? atoi(argv[2]) : 2000, argc > 3 ? strtoull(argv[3], 0, 10) : 1); return 0; }
    if (cmd == "show") { show(argc - 2, argv + 2); return 0; }
    printf("usage: dicesim test | duel [games] [seed] | show <faces 1-6, 0=joker>...\n");
    return 2;
}
