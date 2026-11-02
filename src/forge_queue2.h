// forge_queue2.h -- AutoForge 2.0: multi-recipe queue model and controller.
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// No game or Windows code here: autoforge.cpp feeds it what the game shows and does (recipe pages, menu state,
// quench, minigame end) and gives it the game's own actions through fq::Game.  tools/forgequeuetest.cpp drives the
// same code offline.  The controller never crafts on its own: every piece goes through the game's craft handler,
// and only after the game's own craft check said yes for the recipe the game's list has focused.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fq {

constexpr int MAXQ = 16, MAXROWS = 12, MAXMAT = 64, IDLEN = 96, NAMELEN = 96;

inline void copy(char* d, size_t cap, const char* s) { if (!cap) return; size_t i = 0; if (s) for (; s[i] && i + 1 < cap; i++) d[i] = s[i]; d[i] = 0; }
inline void cat(char* d, size_t cap, const char* fmt, ...) {
    size_t k = strlen(d); if (k + 1 >= cap) return;
    va_list a; va_start(a, fmt); vsnprintf(d + k, cap - k, fmt, a); va_end(a);
}

struct Row { char name[NAMELEN]; int need; };          // one material row of a recipe page, for ONE piece
struct Item { char id[IDLEN]; char title[NAMELEN]; int qty; Row rows[MAXROWS]; int nrows; void* recipe; };
struct Mat { char name[NAMELEN]; int have; };

struct Queue {
    Item it[MAXQ]; int n = 0;
    Mat mats[MAXMAT]; int nm = 0;                      // owned counts, as the game's pages last showed them
    int made = 0, started = 0;                          // pieces forged / pieces at the start of this run

    int find(const char* id) const { for (int i = 0; i < n; i++) if (!strcmp(it[i].id, id)) return i; return -1; }
    int qty(const char* id) const { int i = find(id); return i < 0 ? 0 : it[i].qty; }
    int pieces() const { int s = 0; for (int i = 0; i < n; i++) s += it[i].qty; return s; }
    int next() const { for (int i = 0; i < n; i++) if (it[i].qty > 0) return i; return -1; }
    void clear() { n = 0; made = 0; started = 0; }

    int mat(const char* name) const { for (int i = 0; i < nm; i++) if (!strcmp(mats[i].name, name)) return i; return -1; }
    int have(const char* name) const { int i = mat(name); return i < 0 ? -1 : mats[i].have; }
    void note_have(const char* name, int have) {
        int i = mat(name);
        if (i < 0) { if (nm >= MAXMAT) return; i = nm++; copy(mats[i].name, NAMELEN, name); }
        mats[i].have = have;
    }
    // A recipe page was shown: remember owned counts, and refresh the rows of a queued recipe.
    void page(const char* id, const char* title, const Row* r, int nr, const int* have, void* recipe) {
        for (int k = 0; k < nr; k++) note_have(r[k].name, have[k]);
        int i = find(id); if (i < 0) return;
        set(it[i], id, title, r, nr, recipe);
    }
    static void set(Item& t, const char* id, const char* title, const Row* r, int nr, void* recipe) {
        copy(t.id, IDLEN, id); copy(t.title, NAMELEN, title); t.recipe = recipe;
        t.nrows = nr < MAXROWS ? nr : MAXROWS; for (int k = 0; k < t.nrows; k++) t.rows[k] = r[k];
    }
    // Total need of one material over the whole queue, plus `extra` pieces of a recipe with rows r.
    int total(const char* name, const char* exId = nullptr, int extra = 0, const Row* r = nullptr, int nr = 0) const {
        int s = 0;
        for (int i = 0; i < n; i++) for (int k = 0; k < it[i].nrows; k++) if (!strcmp(it[i].rows[k].name, name)) s += it[i].qty * it[i].rows[k].need;
        (void)exId;
        if (extra && r) for (int k = 0; k < nr; k++) if (!strcmp(r[k].name, name)) s += extra * r[k].need;
        return s;
    }
    // Would one more piece of this recipe still leave every material of the whole queue covered?
    bool fits_one_more(const char* id, const Row* r, int nr, char* why, size_t cap) const {
        for (int k = 0; k < nr; k++) {
            int h = have(r[k].name), need = total(r[k].name, id, 1, r, nr);
            if (h >= 0 && need > h) { if (why) snprintf(why, cap, "%s %d/%d", r[k].name, need, h); return false; }
        }
        return true;
    }
    // + on the recipe page.  Returns the new amount, or -1 (full / short; `why` says what).
    int add(const char* id, const char* title, const Row* r, int nr, void* recipe, char* why, size_t cap) {
        if (nr < 1) { if (why) snprintf(why, cap, "no material list"); return -1; }
        if (!fits_one_more(id, r, nr, why, cap)) return -1;
        int i = find(id);
        if (i < 0) { if (n >= MAXQ) { if (why) snprintf(why, cap, "the queue holds %d recipes", MAXQ); return -1; }
                     i = n++; it[i].qty = 0; }
        set(it[i], id, title, r, nr, recipe);
        return ++it[i].qty;
    }
    // - on the recipe page.  Returns the new amount (0 = no longer queued).
    int remove_one(const char* id) {
        int i = find(id); if (i < 0) return 0;
        int q = --it[i].qty; if (q <= 0) erase(i);
        return q < 0 ? 0 : q;
    }
    void erase(int i) { for (int k = i; k + 1 < n; k++) it[k] = it[k + 1]; n--; }

    // Text added to the game's recipe description while something is queued.
    void summary(char* out, size_t cap, int running) const {
        out[0] = 0;
        if (running) cat(out, cap, "Forging %d / %d\n", made + 1, made + pieces());
        cat(out, cap, "CRAFT QUEUE (%d items)\n", pieces());
        const int recipeLines = n < 6 ? n : 6;
        for (int i = 0; i < recipeLines; i++) cat(out, cap, "  x%d %s\n", it[i].qty, it[i].title);
        if (n > recipeLines) cat(out, cap, "  +%d more recipes\n", n - recipeLines);
        cat(out, cap, "\nMATERIALS (need / have)\n");
        int shown = 0; char seen[MAXMAT][NAMELEN]; int ns = 0;
        for (int i = 0; i < n; i++) for (int k = 0; k < it[i].nrows; k++) {
            const char* m = it[i].rows[k].name; bool dup = false;
            for (int j = 0; j < ns; j++) if (!strcmp(seen[j], m)) dup = true;
            if (dup || ns >= MAXMAT) continue;
            copy(seen[ns++], NAMELEN, m);
            int need = total(m), h = have(m);
            if (shown++ >= 8) continue; // keep the narrow book description readable
            cat(out, cap, "  %s: %d / %d", m, need, h < 0 ? 0 : h);
            if (h >= 0 && need > h) cat(out, cap, " (SHORT %d)", need - h);
            cat(out, cap, "\n");
        }
        if (shown > 8) cat(out, cap, "  +%d more materials\n", shown - 8);
    }
    // One line for the log / on-screen notice.
    void line(char* out, size_t cap) const {
        out[0] = 0;
        for (int i = 0; i < n; i++) cat(out, cap, "%s%s x%d", i ? ", " : "", it[i].title, it[i].qty);
        if (!n) cat(out, cap, "empty");
    }
};

// What the controller needs from the game (autoforge.cpp: the game's own functions; tests: a fake forge).
struct Game {
    virtual uint64_t now() = 0;
    virtual bool focus(const char* id) = 0;                 // the game's list: focus this recipe (Flash FocusRecipe)
    virtual bool focused(char* out, size_t cap) = 0;        // the recipe the game's list has focused (craft uses it)
    virtual const char* page_id() = 0;                      // recipe of the last page the game drew ...
    virtual uint64_t page_time() = 0;                       // ... and when
    virtual int can_craft(void* recipe) = 0;                // the game's craft check: 1 yes, 0 no, -1 unknown
    virtual bool craft() = 0;                               // the game's craft handler (what the craft key calls)
    virtual bool restart() = 0;                             // the game's forge start (what using the forge calls)
    virtual void say(const char* line) = 0;                 // log + optional notice
    virtual ~Game() {}
};

enum Phase { Off, WaitMenu, Focusing, Confirmed, Forging, WaitEnd, Restarting, WaitUser };
inline const char* phase_name(Phase p) {
    static const char* N[] = { "off", "menu", "focusing", "confirmed", "forging", "piece done", "restarting", "waiting for you" };
    return N[p];
}

// Timings (ms).
constexpr uint64_t MENU_SETTLE = 700, FOCUS_TIMEOUT = 3000, CONFIRM_TIMEOUT = 2500, END_DELAY = 1500, RESTART_TIMEOUT = 6000;

struct Ctl {
    Queue q; Game* g = nullptr; Phase ph = Off; uint64_t t = 0; int cur = -1;
    bool autoContinue = true; bool pieceAuto = false;  // pieceAuto: autoforge.cpp starts the forging autopilot
    bool sawInactive = false; uint64_t inactiveT = 0;
    char lastPaused[160] = { 0 };

    bool running() const { return ph != Off; }
    void say(const char* fmt, ...) {
        char b[400]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); if (g) g->say(b);
    }
    void go(Phase p) { ph = p; t = g->now(); }
    void pause(const char* why) {
        if (ph == Off) return;
        ph = Off; pieceAuto = false; copy(lastPaused, sizeof lastPaused, why);
        say("AutoForge queue: paused - %s (%d left; F9 in the recipe menu resumes, Delete clears)", why, q.pieces());
    }
    void finish() {
        say("AutoForge queue: done, %d made", q.made);
        q.clear(); ph = Off; pieceAuto = false;
    }
    // F9 in the recipe menu.
    bool start_in_menu() {
        if (q.pieces() <= 0) return false;
        if (ph == Off) { q.started = q.pieces(); q.made = 0; }
        char lb[300]; q.line(lb, sizeof lb);
        say("AutoForge queue: started, %d items (%s)", q.pieces(), lb);
        ph = WaitMenu; t = g->now() - MENU_SETTLE; return true;
    }

    // Every tick of the forge.  s = forge state (1 = recipe menu), active = minigame running.
    void tick(int s, bool active) {
        if (ph == Off) return;
        uint64_t now = g->now();
        if (s == 1 && (ph == WaitEnd || ph == Restarting || ph == WaitUser)) { go(WaitMenu); return; }
        switch (ph) {
        case WaitMenu: {
            if (s != 1) return;
            if (now - t < MENU_SETTLE || !g->page_time()) return;   // the game has drawn a recipe page in this menu
            int i = q.next(); if (i < 0) { finish(); return; }
            cur = i; char f[IDLEN] = { 0 }; g->focused(f, sizeof f);
            if (strcmp(f, q.it[i].id) || strcmp(g->page_id(), q.it[i].id)) {
                if (!g->focus(q.it[i].id)) { pause("the game's recipe list would not switch recipe"); return; }
            }
            go(Focusing); return;
        }
        case Focusing: {
            if (s != 1) return;
            Item& it = q.it[cur];
            char f[IDLEN] = { 0 }; bool fo = g->focused(f, sizeof f);
            if (fo && !strcmp(f, it.id) && !strcmp(g->page_id(), it.id)) {
                int c = g->can_craft(it.recipe);
                if (c == 1) {
                    if (!g->craft()) { pause("the game's craft action did not run"); return; }
                    go(Confirmed); return;
                }
                if (c == 0) {
                    say("AutoForge queue: skipped %s - not enough materials (%d not made)", it.title, it.qty);
                    q.erase(cur); go(WaitMenu); t = now - MENU_SETTLE;
                    if (q.next() < 0) { say("AutoForge queue: nothing left that can be made"); q.clear(); ph = Off; }
                    return;
                }
                pause("the game's craft check could not be read"); return;
            }
            if (now - t > FOCUS_TIMEOUT) {
                char w[200]; snprintf(w, sizeof w, "could not switch to %s - pick it and craft it to go on", it.title);
                pause(w);
            }
            return;
        }
        case Confirmed:
            if (s == 1 && now - t > CONFIRM_TIMEOUT) pause("the piece did not start");
            return;
        case Forging: return;
        case WaitEnd:
            if (active) { sawInactive = false; return; }
            if (!sawInactive) { sawInactive = true; inactiveT = now; return; }
            if (now - inactiveT < END_DELAY) return;
            sawInactive = false;
            if (!autoContinue) { say("AutoForge queue: use the forge to go on (%d left)", q.pieces()); go(WaitUser); return; }
            if (g->restart()) { go(Restarting); return; }
            say("AutoForge queue: use the forge to go on (%d left) - the game did not reopen it", q.pieces());
            go(WaitUser); return;
        case Restarting:
            if (now - t > RESTART_TIMEOUT) { say("AutoForge queue: use the forge to go on (%d left)", q.pieces()); go(WaitUser); }
            return;
        case WaitUser: case Off: return;
        }
    }
    // The game left its menu with a recipe chosen (through the game's own craft, by us or by you).
    // Returns true when the autopilot should forge this piece.
    bool chosen(const char* id) {
        int i = q.find(id);
        if (i < 0 || q.it[i].qty <= 0) {
            if (ph != Off) pause("another recipe was chosen");
            return false;
        }
        if (ph == Off) { q.started = q.pieces(); q.made = 0; say("AutoForge queue: started, %d items", q.pieces()); }
        cur = i; go(Forging); pieceAuto = true;
        say("AutoForge queue: %d of %d - %s", q.made + 1, q.made + q.pieces(), q.it[i].title);
        return true;
    }
    // The menu closed without a recipe (Esc / walked away).
    void menu_closed() { if (ph == WaitMenu || ph == Focusing) pause("the recipe menu was closed"); }
    // The current piece was quenched.
    void quenched(const char* id) {
        if (ph != Forging) return;
        int i = q.find(id); if (i < 0) return;
        q.made++; if (--q.it[i].qty <= 0) q.erase(i);
        pieceAuto = false;
        if (q.pieces() <= 0) { finish(); return; }
        sawInactive = false; go(WaitEnd);
    }
    // F9 during a piece.
    void stopped() { if (ph != Off) pause("stopped with the key"); }
};

} // namespace fq
