// autodice.cpp -- Theatrical AutoDice for Kingdom Come: Deliverance II (kcd2_autodice.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Henry plays his own turns at the dice table: after every throw he looks the dice over, sets aside the
// ones worth keeping one by one, then either throws the rest again or passes - through the same game
// functions the player's keys call (dice_hold, dice_score, dice_pass). The game itself throws the dice
// (with each die's own weights), scores, busts and moves the money; the mod never writes game state.
// The choice of dice and when to stop comes from dice_engine.h (expected-value play with the real
// weights of Henry's dice, the target score and how close the opponent is to winning).
// Badges are left to the player. Use: sit down to dice, press F3 (kcd2_autodice.ini); F3 again stops.
// Game code is found by RTTI + signature at startup (kcd2_common.h); on a mismatch nothing is hooked.
// Log: kcd2_autodice.log next to the DLL.
//
// How the dice game is organised (build 15693, see NOTES.md):
//   C_Dice (minigame, vtbl[14] = per-frame update) +0x498 input state (5 = the player may act),
//     +0x4A0 C_DiceGameModel*.
//   C_DiceGameModel: +0x18 delegate (second vtable; slot 10 scores the dice currently held),
//     +0x20 / +0x30 player models (0 = Henry), +0x100 target score, +0x108 current state.
//   State C_PlayerTurnDiceGameState = a player chooses dice; state +0x18 = that player's model.
//   C_DiceGamePlayerModel: +0x18 points this turn, +0x30 banked total, +0x58 index,
//     +0xD8/+0xE0 dice on the table (vector of shared_ptr<C_DieModel>, 16 bytes each).
//   C_DieModel: +0x28 id, +0x40 state (0 free, 1 held this throw, 2 set aside earlier this turn), +0x58 face 0..5,
//     +0x88 six side weights (float), +0xA0 first side value (6 = joker face: devil's head die).
#include "kcd2_common.h"
#include "dice_engine.h"
#include "dice_turn.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <random>
#include <string>

using kc::logf;
template<class T> static T& F(uintptr_t p, uintptr_t o) { return *(T*)(p + o); }
static uint64_t (*g_clock)() = GetTickCount64;

// ---------------- signatures (build 15693 addresses in comments; struct offsets stay literal) ----------------
static const char* SIG_UPDATE   = "F3 0F 10 89 60 05 00 00 E9 ?? ?? ?? ??";                       // C_Dice vtbl[14] 0x2E9272C
static const char* SIG_SCORE    = "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 90 78 01 00 00 84 C0 74 ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 83 C4 20 5B C3";   // dice_score handler 0x2E91B3C
static const char* SIG_PASS     = "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 90 78 01 00 00 84 C0 74 ?? 48 8B CB E8 ?? ?? ?? ?? 45 33 C0 48 8B 88 08 01 00 00 48 8D 50 18 48 8B 01 FF 90 B0 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 83 C4 20 5B C3";   // dice_pass handler 0x2E919E0
static const char* SIG_HOLDDIE  = "4C 8B 91 08 01 00 00 44 8B DA 48 8D 51 18 45 8A C8 45 8B C3 49 8B CA 49 8B 02 48 FF 60 60";   // C_DiceGameModel::HoldDie(id, hold) 0x2E97F64
static const char* SIG_HOLD_KEY = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B 01 48 8B F9 FF 90 78 01 00 00 84 C0 74 ?? 48 8D 54 24 20 48 8B CF E8 ?? ?? ?? ?? 48 8B 74 24 20 48 85 F6 74 ?? 48 8B 46 48 48 8B CF 8A 58 40 E8 ?? ?? ?? ?? 48 8B 56 48 80 FB 01 48 8B C8 41 0F 95 C0 8B 52 28 E8 ?? ?? ?? ??";   // dice_hold handler 0x2E8FA84 (calls HoldDie with die +0x28 / +0x40)
static const char* SIG_EVAL     = "4C 8B DC 48 83 EC 78 48 8D 05 ?? ?? ?? ?? 49 89 43 B8";          // C_DiceGameModel delegate vtbl[10] 0x2E947F4
static const char* SIG_GATE     = "83 B9 98 04 00 00 05 0F 94 C0 C3";                               // C_Dice vtbl[47] 0x2E8E45C: input state 5
static const char* SIG_GETMODEL = "40 53 48 83 EC 20 48 83 B9 A0 04 00 00 00 48 8B D9 75 ?? E8 ?? ?? ?? ?? 48 8B 83 A0 04 00 00 48 83 C4 20 5B C3";   // 0x2E8B054: C_Dice +0x4A0
static const char* SIG_CURPM    = "40 53 48 83 EC 20 48 8B 81 A0 04 00 00 48 8B D9 48 8B 88 08 01 00 00 48 8B 01 FF 50 20 48 63 40 58";   // 0x2E8A604: model +0x108 state, vtbl[4] player, +0x58
static const char* SIG_COLLECT  = "48 89 5C 24 18 55 56 57 48 83 EC 20 48 8B 02 41 8A E8 48 8B F2 48 3B 42 08 74 ?? 48 89 42 08 48 8B 99 D8 00 00 00 48 8B B9 E0 00 00 00 48 3B DF 74 ?? 48 8B 03 40 38 68 40 75 ?? 83 B8 A0 00 00 00 06 8B 48 58 75 ??";   // 0x2E89FF4: player dice +0xD8/+0xE0, die +0x40/+0xA0/+0x58
static const char* SIG_TURNPTS  = "4C 8B DC 49 89 4B 08 53 48 81 EC 80 00 00 00 41 8B 58 18";       // 0x2E8B508: player +0x18 points this turn
static const char* SIG_TOTAL    = "48 83 EC 28 44 8B 41 30 4C 8B CA 48 8B D1";                      // 0x2E95554: player +0x30 total
static const char* SIG_PLAYERS  = "48 89 5C 24 08 44 89 4C 24 20 44 89 44 24 18 57 48 83 EC 20 48 8B DA E8 ?? ?? ?? ?? 83 7C 24 40 00 48 8B F8 7C ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8D 54 24 40 48 8B 48 20 E8 ?? ?? ?? ?? 83 7C 24 48 00 7C ?? 48 8B CF E8 ?? ?? ?? ?? 48 8D 54 24 48 48 8B 48 30";   // Dice.SetScore 0x2ECF8C8: model +0x20/+0x30
static const char* SIG_TARGET   = "B8 A0 86 01 00 3B C8 0F 4C C1 48 8D B3 08 01 00 00 89 83 00 01 00 00";   // C_DiceGameModel ctor 0x2E7F4C8: +0x100 target, +0x108 state
static const char* SIG_WEIGHTS  = "45 88 48 40 49 89 48 48 49 8D 88 88 00 00 00 66 45 89 50 50 66 45 89 50 52 45 89 48 58";   // C_DieModel ctor 0x2E7F7C1: +0x40, +0x58, weights +0x88

// ---------------- layout ----------------
static const uintptr_t D_INPUT = 0x498, D_MODEL = 0x4A0;
static const uintptr_t M_DELEGATE = 0x18, M_PM0 = 0x20, M_PM1 = 0x30, M_TARGET = 0x100, M_STATE = 0x108;
static const uintptr_t S_PLAYER = 0x18;
static const uintptr_t P_TURN = 0x18, P_TOTAL = 0x30, P_DICE0 = 0xD8, P_DICE1 = 0xE0;
static const uintptr_t Z_ID = 0x28, Z_HELD = 0x40, Z_FACE = 0x58, Z_W = 0x88, Z_SIDE0 = 0xA0;
static const int INPUT_PLAYER = 5;

typedef void (*Update_t)(void*, float);
typedef void (*Action_t)(void*);                          // dice_score / dice_pass handlers (C_Dice*)
typedef void (*HoldDie_t)(void*, uint32_t, bool);         // C_DiceGameModel*, die id, hold
typedef bool (*Eval_t)(void*, bool, int*, void*, bool);   // delegate, held dice?, out score, functor, flag
static Update_t pOrigUpdate = nullptr;
static Action_t pScore = nullptr, pPass = nullptr;
static HoldDie_t pHoldDie = nullptr;
static Eval_t pEval = nullptr;
static uintptr_t vtPlayerTurn = 0, vtDie = 0, vtPlayer = 0, vtModel = 0;

// ---------------- settings ----------------
static kc::Hotkey g_key;
static double g_speed = 1.0, g_risk = 1.0;

// ---------------- reading the game ----------------
struct Snap {
    uintptr_t dice = 0, model = 0, state = 0, pm = 0, opp = 0;
    bool turn = false;        // Henry may choose dice now
    int target = 0, turnPts = 0, total = 0, oppTotal = 0;
    std::vector<uintptr_t> dieObj;
    std::vector<dice::Die> table;     // all dice in the player's table vector
    std::vector<uint8_t> held;        // die state: dice::FREE / HELD / LOCKED
    std::vector<dice::Die> oppDice;
};
static bool read_dice(uintptr_t pm, std::vector<uintptr_t>& obj, std::vector<dice::Die>& out, std::vector<uint8_t>* held) {
    uintptr_t b = F<uintptr_t>(pm, P_DICE0), e = F<uintptr_t>(pm, P_DICE1);
    if (e < b || (e - b) % 16 || (e - b) / 16 > 6) return false;
    for (uintptr_t p = b; p < e; p += 16) {
        uintptr_t z = F<uintptr_t>(p, 0);
        if (!z || !kc::is_a(z, vtDie)) return false;
        dice::Die d;
        d.id = F<int>(z, Z_ID);
        int face = F<int>(z, Z_FACE);
        if (face < 0 || face > 5) return false;
        d.jokerFace0 = F<int>(z, Z_SIDE0) == 6;
        d.face = (d.jokerFace0 && face == 0) ? dice::JOKER : face;
        float s = 0; bool ok = true;
        for (int i = 0; i < 6; i++) { float w = F<float>(z, Z_W + 4 * i); if (!(w >= 0.f && w < 1000.f)) ok = false; d.w[i] = w; s += w; }
        if (!ok || s <= 0.f) for (float& w : d.w) w = 1.f;
        obj.push_back(z); out.push_back(d);
        uint8_t st = F<uint8_t>(z, Z_HELD); if (st > dice::LOCKED) return false;
        if (held) held->push_back(st);
    }
    return true;
}
static bool snap(uintptr_t dice, Snap& s) {
    s = Snap{}; s.dice = dice;
    s.model = F<uintptr_t>(dice, D_MODEL);
    if (!s.model || !kc::is_a(s.model, vtModel)) return false;
    s.state = F<uintptr_t>(s.model, M_STATE);
    uintptr_t pm0 = F<uintptr_t>(s.model, M_PM0), pm1 = F<uintptr_t>(s.model, M_PM1);
    if (!s.state || !pm0 || !pm1 || !kc::is_a(pm0, vtPlayer) || !kc::is_a(pm1, vtPlayer)) return false;
    s.pm = pm0; s.opp = pm1;
    s.target = F<int>(s.model, M_TARGET);
    s.turnPts = F<int>(pm0, P_TURN); s.total = F<int>(pm0, P_TOTAL); s.oppTotal = F<int>(pm1, P_TOTAL);
    s.turn = F<int>(dice, D_INPUT) == INPUT_PLAYER && kc::is_a(s.state, vtPlayerTurn) && F<uintptr_t>(s.state, S_PLAYER) == pm0;
    if (!read_dice(pm0, s.dieObj, s.table, &s.held)) return false;
    std::vector<uintptr_t> o; read_dice(pm1, o, s.oppDice, nullptr);
    return true;
}
static void faces_str(const Snap& s, char* out, size_t cap) {
    size_t k = 0; out[0] = 0;
    for (size_t i = 0; i < s.table.size() && k + 8 < cap; i++)
        k += sprintf_s(out + k, cap - k, "%s%s%s", i ? " " : "", s.table[i].face == dice::JOKER ? "J" : std::to_string(s.table[i].face + 1).c_str(),
                       s.held[i] == dice::LOCKED ? "#" : s.held[i] == dice::HELD ? "*" : "");
}
static std::vector<dice::TableDie> table_of(const Snap& s) {
    std::vector<dice::TableDie> t(s.table.size());
    for (size_t i = 0; i < t.size(); i++) { t[i].die = s.table[i]; t[i].state = s.held[i]; }
    return t;
}

// ---------------- the decision runs on a worker thread (the first one of a game can take a moment) ----------------
static std::mutex g_mx;
static std::atomic<int> g_job{0};          // 0 idle, 1 running, 2 done
static dice::Decision g_result;
static void think(dice::Context cx, std::vector<dice::Die> table, std::vector<dice::Die> aside, std::vector<dice::Die> opp, int turn) {
    std::thread([=]() {
        std::vector<const dice::Die*> op; for (auto& x : opp) op.push_back(&x);
        dice::Context c = cx;
        c.oppThreat = dice::win_this_turn(std::max(50, c.target - c.opp), dice::avg_dist(op));
        dice::Decision d = dice::decide(c, table, aside, turn);
        { std::lock_guard<std::mutex> l(g_mx); g_result = d; }
        g_job = 2;
    }).detach();
}

// ---------------- Henry's turn ----------------
enum Phase { IDLE, LOOK, THINK, HOLD, CHECK, ACT, WAIT };
static const char* PH[] = {"idle", "look", "think", "hold", "check", "act", "wait"};
static bool g_on = false, g_dead = false;
static Phase g_ph = IDLE;
static uint64_t g_at = 0, g_since = 0;
static uintptr_t g_dice = 0, g_waitState = 0;
static dice::Decision g_plan;
static dice::ThrowView g_view;             // the dice the plan was made for (free = just thrown, aside = locked)
static dice::Context g_cx; static int g_turnAtLook = 0;
static uintptr_t g_logState = 0; static int g_logInput = -1;
static uintptr_t g_base = 0;
static std::vector<int> g_wantIds;         // die ids to be held
static int g_holdTries = 0, g_throws = 0, g_turns = 0, g_lastTotal = 0;
static std::mt19937 g_rng(GetTickCount());
static uint64_t pause_ms(int lo, int hi) { return (uint64_t)(std::uniform_int_distribution<int>(lo, hi)(g_rng) / g_speed); }

static void stop(const char* why) { if (g_on) logf("stop: %s", why); g_on = false; g_ph = IDLE; }
static void enter(Phase p, uint64_t delay) { g_ph = p; g_at = g_clock() + delay; g_since = g_clock(); }

static void tick(uintptr_t dice) {
    uint64_t now = g_clock();
    if (g_key.pressed()) {
        if (g_on) { stop("user key"); return; }
        g_on = true; g_ph = IDLE; logf("on: Henry plays his own turns (%s again stops)", "key");
    }
    if (!g_on) return;
    if (dice != g_dice) { g_dice = dice; g_ph = IDLE; }
    Snap s;
    if (!snap(dice, s)) { if (g_ph != IDLE) KC_DLOG("dice table not readable right now - waiting"); g_ph = IDLE; return; }

    {   // trace for the first test session: every state change with the dice as the game holds them
        uintptr_t sv = F<uintptr_t>(s.state, 0); int in = F<int>(dice, D_INPUT);
        if (sv != g_logState || in != g_logInput) {
            g_logState = sv; g_logInput = in;
            char fs0[64]; faces_str(s, fs0, sizeof fs0);
            uintptr_t a0 = F<uintptr_t>(s.pm, 0x168), a1 = F<uintptr_t>(s.pm, 0x170);
            KC_DLOG("state vtbl +0x%llX input %d player %s turn-state %d | table [%s] | 2nd vector %lld | turn %d total %d opp %d",
                    (unsigned long long)(sv - g_base), in, F<uintptr_t>(s.state, S_PLAYER) == s.pm ? "Henry" : "other", (int)s.turn, fs0,
                    (a1 >= a0 && a1 - a0 < 0x200) ? (long long)((a1 - a0) / 16) : -1LL, s.turnPts, s.total, s.oppTotal);
        }
    }
    if (!s.turn) {                                          // opponent's turn, dice rolling, scoring, badges...
        if (s.total != g_lastTotal) { logf("score: Henry %d, opponent %d (target %d)", s.total, s.oppTotal, s.target); g_lastTotal = s.total; }
        if ((g_ph == THINK || g_ph == HOLD || g_ph == CHECK) && now - g_since < 4000) return;   // e.g. a die being picked up
        if (g_ph != IDLE && g_ph != WAIT) KC_DLOG("turn left Henry's hands in phase %s", PH[g_ph]);
        g_ph = IDLE;
        return;
    }
    if (g_ph == WAIT) {                                     // our action has not taken effect yet
        if (s.state != g_waitState) { g_ph = IDLE; }
        else if (now - g_since > 5000) { logf("the game did not react to the action - stopping (press the key to try again)"); stop("no reaction"); }
        return;
    }
    if (g_ph == IDLE) { enter(LOOK, pause_ms(700, 1300)); return; }   // the dice have just landed: take a look
    if (now < g_at) return;

    char fs[64]; faces_str(s, fs, sizeof fs);
    switch (g_ph) {
    case LOOK: {
        if (s.table.empty()) { logf("no dice on the table in Henry's turn - stopping"); stop("no dice"); return; }
        if (g_job == 1) return;
        g_view = dice::view_of(table_of(s));
        if (g_view.free.empty()) {                          // every die set aside: the game is about to hand all six back
            if (now - g_since > 4000) { logf("no free dice on the table [%s] - stopping", fs); stop("no free dice"); }
            return;
        }
        dice::Context cx; cx.total = s.total; cx.opp = s.oppTotal; cx.target = s.target > 0 ? s.target : 4000; cx.risk = g_risk;
        logf("throw: [%s]  turn %d, total %d, opponent %d, target %d (%d dice thrown, %d set aside)", fs, s.turnPts, s.total,
             s.oppTotal, cx.target, (int)g_view.free.size(), (int)g_view.aside.size());
        g_cx = cx; g_turnAtLook = s.turnPts;
        g_job = 1; think(cx, g_view.free, g_view.aside, s.oppDice, s.turnPts);
        enter(THINK, 0);
        return;
    }
    case THINK: {
        if (g_job != 2) return;
        g_job = 0;
        { std::lock_guard<std::mutex> l(g_mx); g_plan = g_result; }
        if (g_plan.bust) { logf("nothing scores - waiting for the game to call the bust"); g_waitState = s.state; enter(WAIT, 0); return; }
        g_wantIds = dice::wanted_ids(g_view, g_plan);
        char hs[64]; size_t k = 0; hs[0] = 0;
        for (int i : g_plan.hold) k += sprintf_s(hs + k, sizeof hs - k, "%s%s", k ? " " : "", g_view.free[i].face == dice::JOKER ? "J" : std::to_string(g_view.free[i].face + 1).c_str());
        logf("plan: set aside [%s] for %d, then %s  (pass %.0f / throw on %.0f)", hs, g_plan.score, g_plan.pass ? "PASS" : "THROW AGAIN", g_plan.passValue, g_plan.throwValue);
        g_holdTries = 0;
        enter(HOLD, pause_ms(300, 600));
        return;
    }
    case HOLD: {                                            // one die per step, like a hand reaching out
        dice::HoldStep st = dice::next_hold(table_of(s), g_wantIds);   // never touches dice set aside earlier
        if (st.index >= 0) {
            size_t i = (size_t)st.index;
            if (++g_holdTries > 3 * (int)g_view.free.size() + 6) { logf("dice do not respond to holding [%s] - stopping", fs); stop("hold failed"); return; }
            pHoldDie((void*)s.model, (uint32_t)s.table[i].id, st.hold);
            KC_DLOG("%s die %d (%s)", st.hold ? "hold" : "release", s.table[i].id, s.table[i].face == dice::JOKER ? "joker" : std::to_string(s.table[i].face + 1).c_str());
            enter(HOLD, pause_ms(380, 720));
            return;
        }
        enter(CHECK, pause_ms(150, 300));
        return;
    }
    case CHECK: {                                           // the game's own scoring of the held dice
        int sc = 0; bool ok = pEval((void*)(s.model + M_DELEGATE), true, &sc, nullptr, false);
        if (!ok || sc <= 0) {
            logf("the game does not accept [%s] as a scoring set (%d) - releasing them and stopping", fs, sc);
            for (size_t i = 0; i < s.table.size(); i++) if (s.held[i] == dice::HELD) pHoldDie((void*)s.model, (uint32_t)s.table[i].id, false);
            stop("selection rejected"); return;
        }
        if (sc != g_plan.score) {                           // the game's count wins: decide pass / throw again with it
            bool pass = dice::pass_with(g_cx, g_view, g_plan, g_turnAtLook, sc);
            logf("note: the game scores this set %d (engine %d) - badge or rule difference; now %s", sc, g_plan.score, pass ? "PASS" : "THROW AGAIN");
            g_plan.score = sc; g_plan.pass = pass;
        }
        enter(ACT, pause_ms(500, 1000));
        return;
    }
    case ACT: {
        g_waitState = s.state; g_throws++;
        if (g_plan.pass) { logf("pass: banking %d + %d", s.turnPts, g_plan.score); g_turns++; pPass((void*)dice); }
        else { int left = (int)(g_view.free.size() - g_plan.hold.size()); logf("throw again with %d dice", left ? left : 6); pScore((void*)dice); }
        enter(WAIT, 0);
        return;
    }
    default: return;
    }
}

static void UpdateHook(void* self, float dt) {
    pOrigUpdate(self, dt);
    if (g_dead) return;
    __try { tick((uintptr_t)self); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf("exception in tick - AutoDice disabled for this session"); g_on = false; g_dead = true; }
}

// ---------------- startup ----------------
struct Found { uintptr_t vt, update, score, pass, holddie, eval, vtPT, vtDie, vtPlayer, vtModel; };
static bool resolve(kc::Resolver& R, Found& f) {
    f.vt = R.vtable("C_Dice", ".?AVC_Dice@playermodule@wh@@", 0);
    f.update = R.slot("C_Dice::Update", f.vt, 14, SIG_UPDATE);
    R.slot("C_Dice input gate", f.vt, 47, SIG_GATE);
    f.score = R.find("dice_score handler", SIG_SCORE);
    f.pass = R.find("dice_pass handler", SIG_PASS);
    f.holddie = R.find("HoldDie", SIG_HOLDDIE);
    uintptr_t key = R.find("dice_hold handler", SIG_HOLD_KEY);
    if (key && f.holddie && R.branch("dice_hold -> HoldDie", key + 0x56) != f.holddie) R.fail("dice_hold handler", "does not call HoldDie");
    f.vtModel = R.vtable("C_DiceGameModel", ".?AVC_DiceGameModel@playermodule@wh@@", 0);
    uintptr_t vtDel = R.vtable("C_DiceGameModel delegate", ".?AVC_DiceGameModel@playermodule@wh@@", 0x18);
    f.eval = R.slot("score held dice", vtDel, 10, SIG_EVAL);
    f.vtPT = R.vtable("C_PlayerTurnDiceGameState", ".?AVC_PlayerTurnDiceGameState@playermodule@wh@@", 0);
    f.vtDie = R.vtable("C_DieModel", ".?AVC_DieModel@playermodule@wh@@", 0);
    f.vtPlayer = R.vtable("C_DiceGamePlayerModel", ".?AVC_DiceGamePlayerModel@playermodule@wh@@", 0);
    // layout checks: these functions use the offsets the mod reads
    R.find("layout: C_Dice model", SIG_GETMODEL);
    R.find("layout: current player", SIG_CURPM);
    R.find("layout: dice on the table", SIG_COLLECT);
    R.find("layout: points this turn", SIG_TURNPTS);
    R.find("layout: banked total", SIG_TOTAL);
    R.find("layout: players", SIG_PLAYERS);
    R.find("layout: target score", SIG_TARGET);
    R.find("layout: die weights", SIG_WEIGHTS);
    return R.ok();
}
#ifdef KC_DEVTOOLS
extern "C" __declspec(dllexport) int KC_SelfTest(HMODULE game, uintptr_t* out) {   // offline check (tools/selftest)
    kc::Resolver R; R.im.load(game); Found f{}; bool ok = resolve(R, f);
    uintptr_t* p = (uintptr_t*)&f; for (size_t i = 0; i < sizeof f / 8; i++) out[i] = p[i];
    return ok ? (int)(sizeof f / 8) : -R.fails;
}
#endif

static HMODULE g_self = nullptr;
static DWORD WINAPI Init(LPVOID) {
    kc::init(g_self, "kcd2_autodice");
    g_key.vk = kc::ini_key("Play", VK_F3);
    g_speed = std::min(4.0, std::max(0.25, kc::ini_int("Options", "SpeedPercent", 100) / 100.0));
    g_risk = std::min(2.0, std::max(0.5, kc::ini_int("Options", "RiskPercent", 100) / 100.0));
    HMODULE h = kc::wait_game(); if (!h) { logf("abort: WHGame.dll not loaded"); return 0; }
    kc::Resolver R; R.im.load(h); g_base = R.im.base;
    logf("start v1.0.0 (game ts 0x%08X)", R.im.timestamp);
    Found f{};
    if (!resolve(R, f)) { logf("disabled: this game version is not supported yet - nothing was changed"); return 0; }
    pScore = (Action_t)f.score; pPass = (Action_t)f.pass; pHoldDie = (HoldDie_t)f.holddie; pEval = (Eval_t)f.eval;
    vtPlayerTurn = f.vtPT; vtDie = f.vtDie; vtPlayer = f.vtPlayer; vtModel = f.vtModel;
    (void)dice::tables();                                   // build the outcome tables now, not at the table
    pOrigUpdate = (Update_t)f.update;
    kc::write_ptr(&((void**)f.vt)[14], (void*)&UpdateHook);
    char kn[16]; logf("ready (%s at the dice table; speed %.0f%%, risk %.0f%%)", kc::key_name(g_key.vk, kn, sizeof kn), g_speed * 100, g_risk * 100);
    return 0;
}
KC_PLUGIN("Theatrical AutoDice", KC_AUTHOR, 10)
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) { if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); g_self = h; KC_START(Init); } return TRUE; }
