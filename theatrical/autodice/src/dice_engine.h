// dice_engine.h -- scoring and decision engine of Theatrical AutoDice (KCD2 dice / Farkle).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Pure C++17, no Windows or game dependencies: the plugin (autodice.cpp) feeds it the dice it reads
// from the game, and the offline simulator (tools/dicesim.cpp) feeds it simulated games.
//
// Rules (the game's own combination table, build 15693: C_DiceGameModel score setup 0x2E8DC9C):
//   single 1 = 100, single 5 = 50
//   1-2-3-4-5 = 500, 2-3-4-5-6 = 750, 1-2-3-4-5-6 = 1500
//   three of a kind: 1s = 1000, otherwise face x 100; every die beyond three doubles it
//   a joker face (devil's head die: side value 6 replaces the 1) stands in for any face
// Every die that is set aside must be part of a scoring combination. A throw with no scoring die busts
// the turn. Setting all six dice aside ("hot dice") gives the player all six to throw again.
// Faces are 0-based like the game (0 = one pip ... 5 = six pips); JOKER = 6.
//
// Decisions: for the dice on the table the engine picks which to set aside and whether to throw again
// or pass (bank). It maximises the expected value of the turn with a dynamic programme over
// (dice left to throw, turn score) using the real side weights of the player's dice, with banking worth
// min(turn, points still needed to win) and, when the opponent is close to winning, worth less (they may
// win on their next turn) - so it presses on when it has to.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <array>
#include <map>
#include <algorithm>

namespace dice {

constexpr int FACES = 6, JOKER = 6, CATS = 7;   // categories: 6 faces + joker
constexpr int NEG = -1000000;

// ---------------- scoring ----------------
inline int kind_base(int f) { return f == 0 ? 1000 : (f + 1) * 100; }

// Best score that uses ALL the given dice (counts per face + jokers), or NEG if some die cannot score.
struct Scorer {
    std::map<uint32_t, int> memo;
    static uint32_t key(const int c[6], int j) {
        uint32_t k = 0; for (int f = 0; f < 6; f++) k = k * 8 + (uint32_t)c[f]; return k * 8 + (uint32_t)j;
    }
    int best(int c[6], int j) {
        int n = j; for (int f = 0; f < 6; f++) n += c[f];
        if (!n) return 0;
        uint32_t k = key(c, j);
        auto it = memo.find(k); if (it != memo.end()) return it->second;
        int r = NEG;
        // straights: 1-6, 1-5, 2-6 (jokers fill gaps)
        static const int lo[3] = {0, 0, 1}, hi[3] = {5, 4, 5}, val[3] = {1500, 500, 750};
        for (int s = 0; s < 3; s++) {
            int need = 0; for (int f = lo[s]; f <= hi[s]; f++) if (!c[f]) need++;
            if (need > j) continue;
            int used[6] = {0}; for (int f = lo[s]; f <= hi[s]; f++) if (c[f]) { used[f] = 1; c[f]--; }
            int v = best(c, j - need);
            for (int f = 0; f < 6; f++) c[f] += used[f];
            if (v > NEG) r = std::max(r, v + val[s]);
        }
        // n of a kind (3..6 dice), real dice of the face plus jokers; at least one real die
        for (int f = 0; f < 6; f++) {
            if (!c[f]) continue;
            for (int real = 1; real <= c[f]; real++)
                for (int jk = 0; jk <= j; jk++) {
                    int m = real + jk; if (m < 3 || m > 6) continue;
                    c[f] -= real;
                    int v = best(c, j - jk);
                    c[f] += real;
                    if (v > NEG) r = std::max(r, v + (kind_base(f) << (m - 3)));
                }
        }
        // singles: 1 = 100, 5 = 50, a joker as a 1
        if (c[0]) { c[0]--; int v = best(c, j); c[0]++; if (v > NEG) r = std::max(r, v + 100); }
        if (c[4]) { c[4]--; int v = best(c, j); c[4]++; if (v > NEG) r = std::max(r, v + 50); }
        if (j)    { int v = best(c, j - 1); if (v > NEG) r = std::max(r, v + 100); }
        memo[k] = r;
        return r;
    }
    int score(const int c[6], int j) { int t[6]; memcpy(t, c, sizeof t); return best(t, j); }
};
inline Scorer& scorer() { static Scorer s; return s; }

// ---------------- dice ----------------
struct Die {
    int id = 0;            // the game's die id (C_DieModel +0x28)
    int face = 0;          // 0..5 as shown, JOKER for a devil's head
    float w[6] = {1, 1, 1, 1, 1, 1};   // side weights
    bool jokerFace0 = false;           // side 0 is a joker (devil's head die)
    double p(int cat) const {          // probability of landing in category cat
        double s = 0; for (int i = 0; i < 6; i++) s += w[i] > 0 ? w[i] : 0;
        if (s <= 0) return cat < 6 ? 1.0 / 6 : 0;
        if (cat == JOKER) return jokerFace0 ? std::max(0.f, w[0]) / s : 0;
        if (cat == 0 && jokerFace0) return 0;
        return std::max(0.f, w[cat]) / s;
    }
};
using Dist = std::array<double, CATS>;
inline Dist avg_dist(const std::vector<const Die*>& ds) {
    Dist d{}; if (ds.empty()) { for (int i = 0; i < 6; i++) d[i] = 1.0 / 6; return d; }
    for (auto* x : ds) for (int c = 0; c < CATS; c++) d[c] += x->p(c);
    for (auto& v : d) v /= ds.size();
    return d;
}

// ---------------- outcome tables (independent of the dice weights) ----------------
// For k dice: every multiset of categories, its multinomial coefficient, and every scoring keep
// (sub-multiset whose dice all score) with its score and size.
struct Keep { int score, n; };
struct Outcome { std::array<int, CATS> cnt; double coef; std::vector<Keep> keeps; };
struct Tables {
    std::vector<Outcome> out[7];
    Tables() {
        double fact[7] = {1, 1, 2, 6, 24, 120, 720};
        for (int k = 1; k <= 6; k++) {
            std::array<int, CATS> c{};
            gen(k, 0, c, k, fact);
        }
    }
    void gen(int left, int cat, std::array<int, CATS>& c, int k, const double* fact) {
        if (cat == CATS - 1) {
            c[cat] = left;
            Outcome o; o.cnt = c; o.coef = fact[k];
            for (int i = 0; i < CATS; i++) o.coef /= fact[c[i]];
            // keeps: all sub-multisets
            std::array<int, CATS> s{};
            sub(o, s, 0);
            // dominated keeps of equal size: keep only the best score per size
            std::vector<Keep> best(7, Keep{NEG, 0});
            for (auto& kp : o.keeps) if (kp.score > best[kp.n].score) best[kp.n] = kp;
            o.keeps.clear(); for (int n = 1; n <= 6; n++) if (best[n].score > 0) o.keeps.push_back(best[n]);
            out[k].push_back(o);
            return;
        }
        for (int v = 0; v <= left; v++) { c[cat] = v; gen(left - v, cat + 1, c, k, fact); }
        c[cat] = 0;
    }
    void sub(Outcome& o, std::array<int, CATS>& s, int cat) {
        if (cat == CATS) {
            int n = 0; for (int i = 0; i < CATS; i++) n += s[i];
            if (!n) return;
            int sc = scorer().score(s.data(), s[JOKER]);
            if (sc > 0) o.keeps.push_back({sc, n});
            return;
        }
        for (int v = 0; v <= o.cnt[cat]; v++) { s[cat] = v; sub(o, s, cat + 1); }
        s[cat] = 0;
    }
};
inline const Tables& tables() { static Tables t; return t; }

// ---------------- game context and value of a turn ----------------
struct Context {
    int total = 0, opp = 0, target = 4000;   // banked scores and the score to reach
    double risk = 1.0;                         // >1 bolder, <1 more careful ([Options] Risk)
    double oppThreat = 0;                      // chance the opponent wins on their next turn
    int need() const { return std::max(50, target - total); }
};
// What banking `t` points is worth.
inline double bank_value(const Context& cx, int t) {
    if (t >= cx.need()) return cx.need() * 1.25;          // winning beats any partial bank
    double v = t * (1.0 - cx.oppThreat);
    return v / cx.risk;
}

// V[k][t/50]: expected value of throwing k dice with turn score t, playing on optimally afterwards.
struct ValueTable {
    Dist d6, dk;      // distribution for six dice (hot dice) and for the remaining dice
    int maxT = 0;     // grid: 0..maxT in steps of 50
    std::vector<double> V[7];
    std::vector<double> P[7];   // outcome probabilities per k (k == 6 uses d6)
    void build(const Context& cx, const Dist& rem, const Dist& all) {
        dk = rem; d6 = all;
        for (int k = 1; k <= 6; k++) {
            const Dist& d = (k == 6) ? d6 : dk;
            P[k].clear();
            for (auto& o : tables().out[k]) {
                double p = o.coef; for (int c = 0; c < CATS; c++) for (int r = 0; r < o.cnt[c]; r++) p *= d[c];
                P[k].push_back(p);
            }
        }
        maxT = cx.need() + 50;
        int n = maxT / 50 + 1;
        for (int k = 0; k <= 6; k++) V[k].assign(n, 0.0);
        // t descending: transitions only increase t (every keep scores >= 50)
        for (int ti = n - 1; ti >= 0; ti--) {
            int t = ti * 50;
            for (int k = 1; k <= 6; k++) V[k][ti] = roll_value(cx, k, t);
        }
    }
    double cont(const Context& cx, int k, int t) const {   // value of the choice "bank or throw k more" at t
        double b = bank_value(cx, t);
        if (t >= cx.need()) return b;
        int ti = std::min((int)V[k].size() - 1, t / 50);
        return std::max(b, V[k][ti]);
    }
    double roll_value(const Context& cx, int k, int t) const {
        if (t >= cx.need()) return bank_value(cx, t);
        const auto& outs = tables().out[k];
        double ev = 0;
        for (size_t i = 0; i < outs.size(); i++) {
            double p = P[k][i]; if (p <= 0) continue;
            double best = 0;                         // bust: the turn is lost
            for (auto& kp : outs[i].keeps) {
                int t2 = t + kp.score, k2 = k - kp.n; if (!k2) k2 = 6;
                double v = (t2 >= cx.need()) ? bank_value(cx, t2) : cont(cx, k2, t2);
                if (v > best) best = v;
            }
            ev += p * best;
        }
        return ev;
    }
};
// Tables are reused while the context and the dice stay the same (a turn, usually a whole game).
inline const ValueTable& value_table(const Context& cx, const Dist& rem, const Dist& all) {
    static std::map<std::vector<long long>, ValueTable> cache;
    std::vector<long long> key = {cx.need(), llround(cx.oppThreat * 1e4), llround(cx.risk * 1e4)};
    for (double v : rem) key.push_back(llround(v * 1e6));
    for (double v : all) key.push_back(llround(v * 1e6));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 256) cache.clear();
    ValueTable vt; vt.build(cx, rem, all);
    return cache.emplace(key, std::move(vt)).first->second;
}

// Chance a player wins this turn from 0 needing `need` points with six standard-ish dice (dist d),
// playing only to reach it. Used for the opponent threat.
inline double win_this_turn(int need, const Dist& d) {
    need = std::max(50, ((need + 49) / 50) * 50);
    static std::map<std::vector<long long>, double> memo;
    std::vector<long long> key = {need}; for (double v : d) key.push_back(llround(v * 1e6));
    auto it = memo.find(key); if (it != memo.end()) return it->second;
    if (memo.size() > 4096) memo.clear();
    int n = need / 50 + 1;
    std::vector<double> W[7]; for (auto& w : W) w.assign(n, 0);
    for (int ti = n - 1; ti >= 0; ti--) {
        int t = ti * 50;
        for (int k = 1; k <= 6; k++) {
            double ev = 0;
            for (auto& o : tables().out[k]) {
                double p = o.coef; for (int c = 0; c < CATS; c++) for (int r = 0; r < o.cnt[c]; r++) p *= d[c];
                if (p <= 0) continue;
                double best = 0;
                for (auto& kp : o.keeps) {
                    int t2 = t + kp.score, k2 = k - kp.n; if (!k2) k2 = 6;
                    double v = t2 >= need ? 1.0 : W[k2][t2 / 50];
                    if (v > best) best = v;
                }
                ev += p * best;
            }
            W[k][ti] = ev;
        }
    }
    return memo[key] = W[6][0];
}

// ---------------- the decision for one throw ----------------
struct Decision {
    std::vector<int> hold;     // indices into the `table` vector: dice to set aside
    int score = 0;             // what they score (engine's count)
    bool pass = false;         // bank after setting them aside (else throw the rest)
    double value = 0, passValue = 0, throwValue = 0;
    bool bust = false;         // nothing scores
};

// table: the dice just thrown (on the table). aside: dice already set aside this turn (only their
// weights matter for hot dice). turn: points already collected this turn.
inline Decision decide(const Context& cx, const std::vector<Die>& table, const std::vector<Die>& aside, int turn) {
    Decision best; best.bust = true; best.value = -1;
    int n = (int)table.size(); if (!n) return best;
    std::vector<const Die*> all; for (auto& d : table) all.push_back(&d); for (auto& d : aside) all.push_back(&d);
    Dist dAll = avg_dist(all);
    for (int m = 1; m < (1 << n); m++) {
        int c[6] = {0}, j = 0, k = 0;
        for (int i = 0; i < n; i++) if (m >> i & 1) { k++; if (table[i].face == JOKER) j++; else c[table[i].face]++; }
        int sc = scorer().score(c, j);
        if (sc <= 0) continue;
        int t2 = turn + sc, left = n - k;
        double vPass = bank_value(cx, t2), vThrow = -1;
        if (t2 < cx.need()) {
            int rem = left ? left : 6;
            std::vector<const Die*> r;
            for (int i = 0; i < n; i++) if (!(m >> i & 1)) r.push_back(&table[i]);
            vThrow = value_table(cx, left ? avg_dist(r) : dAll, dAll).roll_value(cx, rem, t2);
        }
        double v = std::max(vPass, vThrow);
        // prefer the higher value; on (near) ties keep fewer dice aside, then more points
        bool better = best.bust || v > best.value + 1e-9 ||
            (fabs(v - best.value) <= 1e-9 && (k < (int)best.hold.size() || (k == (int)best.hold.size() && sc > best.score)));
        if (better) {
            best.bust = false; best.value = v; best.score = sc; best.pass = vPass >= vThrow;
            best.passValue = vPass; best.throwValue = vThrow;
            best.hold.clear(); for (int i = 0; i < n; i++) if (m >> i & 1) best.hold.push_back(i);
        }
    }
    return best;
}

}  // namespace dice
