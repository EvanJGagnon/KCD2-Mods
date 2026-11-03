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
#include "forge_localized_names_en.h"

namespace fq {

// HUD notices.  The game resolves every SendInfoText string as a localization key ("@key"); plain text shows up with
// a stray "@" before each word.  So the mod ships its own keys (Localization/<Language>_xml.pak, written by
// tools/package.py and tools/install_forge_loc.py from this list) and the HUD only ever gets "@taf_..." keys.
// X(enum name, key, English text).  Keep each text short; no "@" and no XML special characters.
#define FQ_NOTE_LIST(X) \
    X(N_STARTED,    "taf_started",    "AutoForge: queue started") \
    X(N_DONE,       "taf_done",       "AutoForge: queue finished") \
    X(N_PAUSED,     "taf_paused",     "AutoForge: queue paused - F9 resumes") \
    X(N_CLEARED,    "taf_cleared",    "AutoForge: queue cleared") \
    X(N_SKIPPED,    "taf_skipped",    "AutoForge: skipped - not enough materials") \
    X(N_NOTHING,    "taf_nothing",    "AutoForge: nothing left to make") \
    X(N_NO_MORE,    "taf_no_more",    "AutoForge: not enough materials for another") \
    X(N_QUEUE_FULL, "taf_queue_full", "AutoForge: the queue is full") \
    X(N_USE_FORGE,  "taf_use_forge",  "AutoForge: use the forge to continue") \
    X(N_PICK_CRAFT, "taf_pick_craft", "AutoForge: craft a queued recipe to start") \
    X(N_CLOSE_RESULT, "taf_close_result", "AutoForge: close the result screen (Esc), then press F9")
enum Note {
#define X(e, k, t) e,
    FQ_NOTE_LIST(X)
#undef X
    N_COUNT
};
inline const char* note_key(Note n) {
    static const char* K[] = {
#define X(e, k, t) k,
        FQ_NOTE_LIST(X)
#undef X
    };
    return (n >= 0 && n < N_COUNT) ? K[n] : "taf_paused";
}
// "AutoForge: item N of M" is key "taf_item_N_M" for 1 <= N <= M <= FQ_PIECE_MAX; larger queues show "taf_item_next".
#define FQ_PIECE_MAX 24
#define FQ_PIECE_TEXT "AutoForge: item %d of %d"
#define FQ_PIECE_NEXT_TEXT "AutoForge: next item"

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
            if (h >= 0 && need > h) { if (why) snprintf(why, cap, "%s %d/%d", fqlabel::display(r[k].name), need, h); return false; }
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

    // Compact summary for the game's narrow recipe-book description field.
    // All material arithmetic still uses original game localization IDs.
    void summary(char* out, size_t cap, int running) const {
        out[0]=0;
        if(running)cat(out,cap,"Forging %d / %d\n",made+1,started?started:pieces());
        cat(out,cap,"QUEUE (%d items)\n",pieces());
        const int showRecipes=n<3?n:3;
        for(int i=0;i<showRecipes;i++)cat(out,cap,"%s x%d\n",fqlabel::display(it[i].title),it[i].qty);
        if(n>showRecipes)cat(out,cap,"+%d recipes\n",n-showRecipes);
        cat(out,cap,"MATERIALS (need/have)\n");
        int shown=0,ns=0;char seen[MAXMAT][NAMELEN];
        for(int i=0;i<n;i++)for(int k=0;k<it[i].nrows;k++){
            const char* key=it[i].rows[k].name;bool dup=false;
            for(int j=0;j<ns;j++)if(!strcmp(seen[j],key))dup=true;
            if(dup||ns>=MAXMAT)continue;
            copy(seen[ns++],NAMELEN,key);
            int need=total(key),owned=have(key);
            if(shown++>=5)continue;
            cat(out,cap,"%s %d/%d",fqlabel::display(key),need,owned<0?0:owned);
            if(owned>=0 && need>owned)cat(out,cap," SHORT %d",need-owned);
            cat(out,cap,"\n");
        }
        if(shown>5)cat(out,cap,"+%d more materials\n",shown-5);
    }
    // Concise notification (avoid raw localization identifiers).
    void line(char* out, size_t cap) const {
        out[0]=0;
        for(int i=0;i<n;i++)cat(out,cap,"%s%s x%d",
            i?", ":"",fqlabel::display(it[i].title),it[i].qty);
        if(!n)cat(out,cap,"empty");
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
    virtual bool result_open() = 0;                         // finished workpiece state, not an arbitrary menu
    virtual bool dismiss_result() = 0;                     // request the game's registered native modal 'close' input handler (NOT an Esc key)
    virtual void say(const char* line) = 0;                 // log line only (never shown on the HUD)
    virtual void note(Note n) = 0;                          // short HUD notice (the mod's own localization key)
    virtual void piece_note(int n, int of) = 0;             // HUD: "item N of M"
    virtual ~Game() {}
};

enum Phase { Off, WaitMenu, Focusing, Confirmed, Forging, WaitEnd, Restarting, WaitUser };
inline const char* phase_name(Phase p) {
    static const char* N[] = { "off", "menu", "focusing", "confirmed", "forging", "piece done", "restarting", "waiting for you" };
    return N[p];
}

// Timings (ms).
constexpr uint64_t MENU_SETTLE = 700, FOCUS_TIMEOUT = 3000, CONFIRM_TIMEOUT = 2500, END_DELAY = 1500, RESTART_TIMEOUT = 6000,
    RESULT_READ_MS = 2500,      // the result dialog stays readable this long before it is closed
    RESULT_WAIT_MS = 25000,     // from the quench: how long to wait for the dialog to appear / be closable
    CLOSE_SETTLE_MS = 15000,   // after the close: how long the game may take to leave the forge session
    RESTART_WINDOW_MS = 8000,  // allow a retired forge component to reappear after the modal closes
    RESTART_POLL_MS = 900;     // don't repeatedly call the start handler every pump tick

struct Ctl {
    Queue q; Game* g = nullptr; Phase ph = Off; uint64_t t = 0; int cur = -1;
    bool autoContinue = true; bool autoDismiss = false;
    bool pieceAuto = false;  // pieceAuto: autoforge.cpp starts the forging autopilot
    bool dismissTried = false; uint64_t dismissT = 0, modalT = 0;
    bool sawInactive = false; uint64_t inactiveT = 0, lastRestartTry = 0;
    char lastPaused[160] = { 0 };

    bool running() const { return ph != Off; }
    void say(const char* fmt, ...) {
        char b[400]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a); if (g) g->say(b);
    }
    void go(Phase p) { ph = p; t = g->now(); }
    void pause(const char* why, Note n = N_PAUSED) {
        if (ph == Off) return;
        ph = Off; pieceAuto = false; dismissTried = false; modalT = 0; lastRestartTry = 0;
        copy(lastPaused, sizeof lastPaused, why);
        say("AutoForge queue: paused - %s (%d left; F9 in the recipe menu resumes, Delete clears)", why, q.pieces());
        if (g) g->note(n);
    }
    void finish() {
        say("AutoForge queue: done, %d made", q.made);
        if (g) g->note(N_DONE);
        q.clear(); ph = Off; pieceAuto = false; dismissTried = false; modalT = 0; lastRestartTry = 0;
    }
    // F9 in the recipe menu.
    bool start_in_menu() {
        if (q.pieces() <= 0) return false;
        if (ph == Off) { q.started = q.pieces(); q.made = 0; }
        char lb[300]; q.line(lb, sizeof lb);
        say("AutoForge queue: started, %d items (%s)", q.pieces(), lb);
        if (g) g->note(N_STARTED);
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
                    g->note(N_SKIPPED);
                    q.erase(cur); go(WaitMenu); t = now - MENU_SETTLE;
                    if (q.next() < 0) { say("AutoForge queue: nothing left that can be made"); g->note(N_NOTHING); q.clear(); ph = Off; }
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
            // After the quench the game opens its native item dialog ("Smithing successful") and stops updating the
            // forge while it is open, so this also runs from the message loop then (autoforge.cpp), not only from the
            // forge update.  Once the dialog is really up, wait a moment (so it can be read), then call the game's own
            // registered 'close' input handler for it - what Esc does.  No synthetic key.
            if (active || g->result_open()) {
                sawInactive = false;
                if (!dismissTried && autoContinue && autoDismiss && g->result_open()) {
                    if (!modalT) { modalT = now; say("AutoForge queue: result screen is up"); }
                    if (now - modalT >= RESULT_READ_MS) {
                        dismissTried = true; dismissT = now;
                        if (!g->dismiss_result()) { pause("could not request the game's result close; close it with Esc", N_CLOSE_RESULT); return; }
                        say("AutoForge queue: requested native result-screen close");
                    }
                    return;
                }
                uint64_t limit = dismissTried ? dismissT + CLOSE_SETTLE_MS : t + RESULT_WAIT_MS;
                if (now >= limit)
                    pause(dismissTried ? "the forge did not move on after the result screen closed; press Esc once, then F9"
                                       : "result screen did not appear or could not be closed; press Esc once, then F9", N_CLOSE_RESULT);
                return;
            }
            if (!sawInactive) { sawInactive = true; inactiveT = now; return; }
            if (now - inactiveT < END_DELAY) return;
            // Keep sawInactive/inactiveT across retries, otherwise every
            // failed start resets the timeout and loops forever.
            if (!autoContinue) { sawInactive=false; say("AutoForge queue: use the forge to go on (%d left)", q.pieces()); g->note(N_USE_FORGE); go(WaitUser); return; }
            // After the modal's completion callback, the old C_Blacksmithing
            // instance can still be gone for a few frames. Give the game time
            // to restore/recreate it; never call pStartForge on a stale pointer.
            if (!lastRestartTry || now - lastRestartTry >= RESTART_POLL_MS) {
                lastRestartTry=now;
                if (g->restart()) { sawInactive=false; go(Restarting); return; }
            }
            if (now-inactiveT < RESTART_WINDOW_MS) return;
            say("AutoForge queue: use the forge to go on (%d left) - no reachable forge after result closed", q.pieces());
            g->note(N_USE_FORGE); sawInactive=false; go(WaitUser); return;
        case Restarting:
            if (now - t > RESTART_TIMEOUT) { say("AutoForge queue: use the forge to go on (%d left)", q.pieces()); g->note(N_USE_FORGE); go(WaitUser); }
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
        if (ph == Off) { q.started = q.pieces(); q.made = 0; say("AutoForge queue: started, %d items", q.pieces()); g->note(N_STARTED); }
        cur = i; go(Forging); pieceAuto = true; modalT = 0; dismissTried = false;
        say("AutoForge queue: %d of %d - %s", q.made + 1, q.made + q.pieces(), q.it[i].title);
        g->piece_note(q.made + 1, q.made + q.pieces());
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
        sawInactive = false; dismissTried = false; modalT = 0; lastRestartTry=0; go(WaitEnd);
    }
    // F9 during a piece.
    void stopped() { if (ph != Off) pause("stopped with the key"); }
};

} // namespace fq
