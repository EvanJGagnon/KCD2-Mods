// dice_turn.h -- the per-throw bookkeeping AutoDice does on top of the game's dice (no game access here,
// so tools/dicesim can replay real game situations).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// The game keeps all six dice of a player in one vector for the whole turn. Each die model has a state
// byte (+0x40): 0 = free on the table, 1 = held (picked this throw), 2 = set aside on an earlier throw of
// this turn (locked: it cannot be released, the game's scorer ignores it, and it is not thrown again).
// Round-1 test bug: the mod treated every non-zero state as "held", so a locked die joined the plan
// ([1# 1 3 3 2 1] planned as 1 1 1 = 1000; the game scored 200) and it tried to release a locked 5
// until it gave up. Everything below keeps the locked dice out of the plan and out of hold/release.
#pragma once
#include "dice_engine.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace dice {

enum DieState : uint8_t { FREE = 0, HELD = 1, LOCKED = 2 };

// One throw as the mod sees it: every die of the player, in the game's order.
struct TableDie { Die die; uint8_t state = FREE; };

// The dice the plan is about. `free` are the dice just thrown (state FREE or HELD: a held die can still
// be released this throw); `aside` are the dice set aside earlier this turn.
struct ThrowView {
    std::vector<Die> free, aside;
    std::vector<int> freeIdx;      // free[i] is table[freeIdx[i]]
};
inline ThrowView view_of(const std::vector<TableDie>& t) {
    ThrowView v;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i].state == LOCKED) v.aside.push_back(t[i].die);
        else { v.free.push_back(t[i].die); v.freeIdx.push_back((int)i); }
    }
    return v;
}

// The die ids a plan (indices into view.free) wants held.
inline std::vector<int> wanted_ids(const ThrowView& v, const Decision& d) {
    std::vector<int> ids; for (int i : d.hold) ids.push_back(v.free[i].id); return ids;
}

// Next hold/release step towards `want`, or index -1 when the free dice match. Locked dice are never touched.
struct HoldStep { int index = -1; bool hold = false; };
inline HoldStep next_hold(const std::vector<TableDie>& t, const std::vector<int>& want) {
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i].state == LOCKED) continue;
        bool w = std::find(want.begin(), want.end(), t[i].die.id) != want.end();
        if (w != (t[i].state == HELD)) return {(int)i, w};
    }
    return {};
}

// After the game has scored the held dice itself: decide again whether to pass or throw on with the
// game's score (badges or rules can make it differ from the engine's count).
inline bool pass_with(const Context& cx, const ThrowView& v, const Decision& d, int turn, int gameScore) {
    int t2 = turn + gameScore;
    if (t2 >= cx.need()) return true;
    double vPass = bank_value(cx, t2);
    std::vector<const Die*> all, rest;
    for (auto& x : v.free) all.push_back(&x);
    for (auto& x : v.aside) all.push_back(&x);
    for (size_t i = 0; i < v.free.size(); i++)
        if (std::find(d.hold.begin(), d.hold.end(), (int)i) == d.hold.end()) rest.push_back(&v.free[i]);
    Dist dAll = avg_dist(all);
    int left = (int)rest.size();
    double vThrow = value_table(cx, left ? avg_dist(rest) : dAll, dAll).roll_value(cx, left ? left : 6, t2);
    return vPass >= vThrow;
}

}  // namespace dice
