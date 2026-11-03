// forgequeuetest.cpp -- offline test of the AutoForge 2.0 queue (src/forge_queue2.h) against a fake forge.
// The fake forge behaves like the game code the queue relies on (WHGame.dll build 15693, static analysis):
//   - the recipe menu (state 1) draws a page for the focused recipe; Flash FocusRecipe moves the focus a few frames
//     later and the page is redrawn then; the craft handler crafts the FOCUSED recipe (whatever the queue wanted);
//   - the craft check is "enough of every material"; materials are taken when the piece is set up (state 2);
//   - after the quench the forge session ends (inactive); the forge start reopens the menu, unless refused.
// Every check failure prints FAIL; the last line is "ALL PASSED" when all scenarios pass.
// Build: cl /nologo /O2 /EHsc /Isrc tools\forgequeuetest.cpp   or   g++ -std=c++17 -Isrc tools/forgequeuetest.cpp
#include "forge_queue2.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Recipe { std::string id, title; std::vector<std::pair<std::string, int>> mats; };
static std::vector<Recipe> R = {
    { "hunting_sword", "Hunting Sword", { { "Scrap Steel", 2 }, { "Fastening", 1 } } },
    { "falchion",      "Falchion",      { { "Scrap Steel", 3 }, { "Fastening", 2 } } },
    { "basilard",      "Basilard",      { { "Scrap Steel", 1 }, { "Scrap Metal", 2 } } },
    { "horseshoe",     "Horseshoe",     { { "Scrap Metal", 1 } } },
};
static const Recipe* rec(const std::string& id) { for (auto& r : R) if (r.id == id) return &r; return nullptr; }

struct Fake : fq::Game {
    uint64_t t = 1000; std::map<std::string, int> inv;
    int state = 0; bool active = false; std::string focusedId = "horseshoe", pageId; uint64_t pageT = 0;
    bool simulateResultScreen = false, resultWaiting = false, closeWorks = true;
    int closeRequests = 0;
    // What the 2026-11-03 live log showed: no forge update runs while the result dialog is open.  With
    // pumpDrives the message-loop tick (autoforge.cpp pump_tick) calls the controller then instead.
    bool updateSilentOnResult = false, pumpDrives = false, updateResumesAfterClose = true, updateAlive = true;
    bool forgeObjectRetiredOnResult = false; // 01:46 live bug: bs_valid() false, modal still active
    bool closeLeavesSessionOpen = false;                 // the close does not make the game leave the forge session
    std::vector<fq::Note> notes; std::vector<std::pair<int,int>> pieceNotes; int pumpTicks = 0, updateTicks = 0;
    std::string pendingFocus; int focusDelay = 0; bool flashWorks = true, restartWorks = true;
    int restartFailsFirstCalls = 0, restartCalls = 0;
    bool componentRetired = false, reacquireWorks = true;
    int reacquireCalls = 0, staleStarts = 0;
    std::string chosen; int pieceTicks = 0; int crafts = 0, wrongCrafts = 0; std::string wantAtCraft;
    std::vector<std::string> made, lines; fq::Ctl* ctl = nullptr; std::string lastQueuedWanted;
    uint64_t now() override { return t; }
    bool focus(const char* id) override { if (!flashWorks) return true; pendingFocus = id; focusDelay = 3; return true; }
    bool focused(char* out, size_t cap) override { fq::copy(out, cap, focusedId.c_str()); return true; }
    const char* page_id() override { return pageId.c_str(); }
    uint64_t page_time() override { return pageT; }
    bool enough(const Recipe& r) { for (auto& m : r.mats) if (inv[m.first] < m.second) return false; return true; }
    int can_craft(void* recipe) override { auto* r = (const Recipe*)recipe; return r && enough(*r) ? 1 : 0; }
    bool craft() override {
        if (state != 1) return false;
        crafts++;
        int i = ctl->cur; if (i >= 0 && i < ctl->q.n && ctl->q.it[i].id != focusedId) wrongCrafts++;
        const Recipe* r = rec(focusedId); if (!r || !enough(*r)) { wrongCrafts++; return true; }
        for (auto& m : r->mats) inv[m.first] -= m.second;      // the game takes the materials
        chosen = focusedId; state = 2; pieceTicks = 0; return true;
    }
    bool restart() override {
        restartCalls++;
        if(restartFailsFirstCalls>0){restartFailsFirstCalls--;return false;}
        if (componentRetired) {
            reacquireCalls++;
            if (!reacquireWorks || resultWaiting) return false;
            componentRetired = false; // vanilla lookup obtains a NEW component; the old one never revives
        }
        if (!restartWorks || active) return false;
        if (componentRetired) { staleStarts++; return false; }
        open_menu(); return true;
    }
    bool result_open() override { return simulateResultScreen && resultWaiting && state==18; }
    bool dismiss_result() override {
        if (!result_open() || !closeWorks) return false;
        closeRequests++; resultWaiting=false;
        if (!closeLeavesSessionOpen) { active=false; state=22; }
        updateAlive = updateResumesAfterClose;
        return true;
    }
    void say(const char* line) override { lines.push_back(line); if (getenv("FQ_VERBOSE")) printf("    | %s\n", line); }
    void note(fq::Note n) override { notes.push_back(n); }
    void piece_note(int n, int of) override { pieceNotes.push_back({ n, of }); }
    // ---- the fake game ----
    void draw_page() {
        pageId = focusedId; pageT = t;
        const Recipe* r = rec(pageId); if (!r) return;
        std::vector<fq::Row> rows; std::vector<int> have;
        for (auto& m : r->mats) { fq::Row x{}; fq::copy(x.name, fq::NAMELEN, m.first.c_str()); x.need = m.second; rows.push_back(x); have.push_back(inv[m.first]); }
        ctl->q.page(r->id.c_str(), r->title.c_str(), rows.data(), (int)rows.size(), have.data(), (void*)r);
    }
    void open_menu() { active = true; state = 1; resultWaiting = false; updateAlive = true; pageT = 0; draw_page(); }
    void close_menu() { state = 3; active = false; pageT = 0; ctl->menu_closed(); }
    void user_pick(const std::string& id) { focusedId = id; draw_page(); }
    // + on the page for the recipe the user looks at
    int user_plus(const std::string& id, char* why = nullptr) {
        user_pick(id); const Recipe* r = rec(id);
        std::vector<fq::Row> rows; for (auto& m : r->mats) { fq::Row x{}; fq::copy(x.name, fq::NAMELEN, m.first.c_str()); x.need = m.second; rows.push_back(x); }
        char w[200] = { 0 }; int q = ctl->q.add(id.c_str(), r->title.c_str(), rows.data(), (int)rows.size(), (void*)r, w, sizeof w);
        if (why) strcpy(why, w);
        return q;
    }
    int seen = -1;                                       // what the mod saw last tick (autoforge.cpp g_lastSt)
    void step() {
        t += 50;
        if (focusDelay > 0 && --focusDelay == 0) { focusedId = pendingFocus; if (state == 1) draw_page(); }
        if (seen == 1 && state != 1 && state >= 2 && state <= 20) ctl->chosen(chosen.c_str());
        seen = state;
        if (state == 2) { state = 5; }
        else if (state == 18) {
            if (simulateResultScreen && resultWaiting) { /* result UI waits for the Close event */ }
            else if (++pieceTicks > 60) { state = 22; active = false; }
        }
        else if (state >= 4 && state <= 20) {
            if (++pieceTicks == 40) {
                made.push_back(chosen); ctl->quenched(chosen.c_str());
                state = 18; resultWaiting = simulateResultScreen;
                if (forgeObjectRetiredOnResult) componentRetired = true;
                if (resultWaiting && updateSilentOnResult) updateAlive = false;
            }
        }
        // who calls the controller this frame: the forge update when it runs; the message-loop tick when it does not
        // (pump_tick only acts in the post-quench phases)
        if (updateAlive) { updateTicks++; ctl->tick(state, active); }
        else if (pumpDrives && (ctl->ph == fq::WaitEnd || ctl->ph == fq::Restarting)) {
            pumpTicks++;
            // In-game pump cannot safely read the retired forge object.
            // The popup is independently captured, so simulate s=-1 and
            // active=false while its native owner stays alive.
            ctl->tick(componentRetired ? -1 : state,
                      componentRetired ? false : active);
        }
    }
    void run(int n) { for (int i = 0; i < n; i++) step(); }
};

static void setup(Fake& f, fq::Ctl& c) { c = fq::Ctl(); c.g = &f; f.ctl = &c; }
static bool said(Fake& f, const char* part) { for (auto& l : f.lines) if (l.find(part) != std::string::npos) return true; return false; }

static void t_localization() {
    printf("localized item names: original game identifiers remain untouched, visible names never show @ keys\\n");
    CHECK(!strcmp(fqlabel::display("@ui_nm_huntingSwordBasic"), "Hunting sword"), "hunting sword translated");
    CHECK(!strcmp(fqlabel::display("@ui_nm_axeBattle01"), "Bearded axe"), "battle axe display name");
    CHECK(!strcmp(fqlabel::display("@ui_nm_bsmt_ironBad"), "Metal"), "simple metal label");
    CHECK(!strcmp(fqlabel::display("@ui_nm_bsmt_hardware"), "Fasteners"), "simple fasteners label");
    const char* unknown=fqlabel::display("@ui_nm_unknownNamedThing");
    CHECK(unknown[0] != '@' && strstr(unknown, "unknown"), "unknown keys do not leak raw tokens: %s",unknown);
}

static void t_model() {
    printf("model: totals, shared materials, refusal, summary\n");
    Fake f; fq::Ctl c; setup(f, c);
    f.inv = { { "Scrap Steel", 11 }, { "Fastening", 5 }, { "Scrap Metal", 6 } };
    f.open_menu();
    CHECK(f.user_plus("hunting_sword") == 1, "hs 1"); CHECK(f.user_plus("hunting_sword") == 2, "hs 2");
    CHECK(f.user_plus("falchion") == 1, "falchion 1");
    CHECK(f.user_plus("basilard") == 1 && f.user_plus("basilard") == 2 && f.user_plus("basilard") == 3, "basilard 3");
    // steel: 2*2+3+3 = 10 of 11; fastening 2+2 = 4 of 5; metal 6 of 6
    CHECK(c.q.total("Scrap Steel") == 10, "steel total %d", c.q.total("Scrap Steel"));
    CHECK(c.q.total("Fastening") == 4, "fastening total");
    CHECK(c.q.total("Scrap Metal") == 6, "metal total");
    char why[200] = { 0 };
    CHECK(f.user_plus("falchion", why) == -1, "falchion 2 must be refused (steel 13/11)");
    CHECK(strstr(why, "Scrap Steel") != nullptr, "why names steel: %s", why);
    CHECK(f.user_plus("horseshoe", why) == -1, "horseshoe refused: metal 7/6 (%s)", why);
    CHECK(c.q.pieces() == 6, "6 pieces");
    char s[1600]; c.q.summary(s, sizeof s, 0);
    CHECK(strstr(s, "Hunting Sword x2") && strstr(s, "Falchion x1") && strstr(s, "Basilard x3"), "summary lists items: %s", s);
    CHECK(strstr(s, "Scrap Steel 10/11") != nullptr, "summary steel: %s", s);
    CHECK(strstr(s, "SHORT") == nullptr, "no shortage yet");
    f.inv["Fastening"] = 3; f.user_pick("falchion");   // a page shows fewer than queued: shortage shown
    c.q.summary(s, sizeof s, 0);
    CHECK(strstr(s, "Fastening 4/3 SHORT 1") != nullptr, "shortage shown: %s", s);
    CHECK(c.q.remove_one("falchion") == 0 && c.q.find("falchion") < 0, "- removes");
    CHECK(c.q.pieces() == 5, "5 pieces");
}

static void t_full_run() {
    printf("full run: 2 + 1 + 3 pieces, each focused, checked and crafted by the game in order\n");
    Fake f; fq::Ctl c; setup(f, c);
    f.inv = { { "Scrap Steel", 11 }, { "Fastening", 5 }, { "Scrap Metal", 6 } };
    f.open_menu();
    f.user_plus("hunting_sword"); f.user_plus("hunting_sword"); f.user_plus("falchion");
    f.user_plus("basilard"); f.user_plus("basilard"); f.user_plus("basilard");
    f.user_pick("horseshoe");                           // the list rests on something else when F9 is pressed
    CHECK(c.start_in_menu(), "start");
    f.run(2000);
    std::vector<std::string> want = { "hunting_sword", "hunting_sword", "falchion", "basilard", "basilard", "basilard" };
    CHECK(f.made == want, "made %zu pieces in order", f.made.size());
    CHECK(f.wrongCrafts == 0, "never crafted a recipe that was not focused/affordable");
    CHECK(f.crafts == 6, "6 crafts (%d)", f.crafts);
    CHECK(!c.running() && c.q.n == 0, "queue finished");
    CHECK(said(f, "done, 6 made"), "done line");
    CHECK(f.inv["Scrap Steel"] == 1 && f.inv["Fastening"] == 1 && f.inv["Scrap Metal"] == 0, "materials used as the game does");
}

static void t_run_out() {
    printf("materials run out mid-queue: that recipe is skipped by the game's check, the rest goes on\n");
    Fake f; fq::Ctl c; setup(f, c);
    f.inv = { { "Scrap Steel", 20 }, { "Fastening", 5 }, { "Scrap Metal", 6 } };
    f.open_menu();
    f.user_plus("falchion"); f.user_plus("falchion"); f.user_plus("basilard");
    c.start_in_menu();
    f.run(60);                                           // first falchion under way
    f.inv["Fastening"] = 0;                              // sold / used elsewhere
    f.run(2000);
    CHECK(f.made.size() == 2 && f.made[0] == "falchion" && f.made[1] == "basilard", "made falchion, basilard (%zu)", f.made.size());
    CHECK(said(f, "skipped Falchion - not enough materials"), "skip line");
    CHECK(f.wrongCrafts == 0, "no craft without materials");
    CHECK(!c.running(), "finished");
}

static void t_focus_fails() {
    printf("the game's list does not switch: queue pauses, nothing is crafted\n");
    Fake f; fq::Ctl c; setup(f, c);
    f.inv = { { "Scrap Steel", 20 }, { "Fastening", 5 }, { "Scrap Metal", 6 } };
    f.open_menu(); f.user_plus("falchion"); f.user_pick("horseshoe");
    f.flashWorks = false;
    c.start_in_menu(); f.run(200);
    CHECK(f.crafts == 0, "no craft (%d)", f.crafts);
    CHECK(!c.running() && c.q.pieces() == 1, "paused, queue kept");
    CHECK(said(f, "could not switch to Falchion"), "pause line");
    // you pick it yourself and craft it: the queue goes on from there
    f.flashWorks = true; f.user_pick("falchion"); f.craft(); f.run(400);
    CHECK(f.made.size() == 1, "manual craft of a queued recipe is forged as part of the queue");
    CHECK(!c.running() && c.q.n == 0, "done");
}

static void t_restart_refused() {
    printf("forge start refused (Henry walked away): waits, then picks the next recipe when you use the forge\n");
    Fake f; fq::Ctl c; setup(f, c);
    f.inv = { { "Scrap Steel", 20 }, { "Fastening", 5 }, { "Scrap Metal", 6 } };
    f.open_menu(); f.user_plus("hunting_sword"); f.user_plus("basilard");
    f.restartWorks = false; c.start_in_menu(); f.run(400);
    CHECK(f.made.size() == 1 && c.ph == fq::WaitUser, "1 made, waiting (phase %s)", fq::phase_name(c.ph));
    CHECK(said(f, "use the forge to go on"), "says use the forge");
    f.open_menu(); f.run(400);
    CHECK(f.made.size() == 2 && f.made[1] == "basilard", "second piece after you open the forge");
}

static void t_cancel() {
    printf("cancel: Esc in the menu pauses; F9 during a piece pauses and the piece is not counted; F9 resumes\n");
    Fake f; fq::Ctl c; setup(f, c);
    f.inv = { { "Scrap Steel", 20 }, { "Fastening", 5 }, { "Scrap Metal", 6 } };
    f.open_menu(); f.user_plus("hunting_sword"); f.user_plus("hunting_sword"); f.user_pick("horseshoe");
    c.start_in_menu(); f.step(); f.step();
    f.close_menu();
    CHECK(!c.running() && c.q.pieces() == 2 && f.crafts == 0, "Esc paused, nothing crafted");
    f.open_menu(); f.run(30);
    CHECK(f.crafts == 0, "a paused queue does not pick by itself");
    c.start_in_menu(); f.run(12);
    CHECK(f.crafts == 1, "resumed with F9 (%d)", f.crafts);
    c.stopped(); f.run(400);
    CHECK(c.q.pieces() == 2 && !c.running(), "F9 during the piece: queue kept at 2 (%d)", c.q.pieces());
}

static void t_no_autocontinue() {
    printf("AutoContinue=0: after each piece you open the forge, the mod picks and crafts the next\n");
    Fake f; fq::Ctl c; setup(f, c); c.autoContinue = false;
    f.inv = { { "Scrap Steel", 20 }, { "Fastening", 5 }, { "Scrap Metal", 6 } };
    f.open_menu(); f.user_plus("hunting_sword"); f.user_plus("horseshoe");
    c.start_in_menu(); f.run(400);
    CHECK(f.made.size() == 1 && c.ph == fq::WaitUser, "waits for you");
    f.open_menu(); f.run(400);
    CHECK(f.made.size() == 2, "second made");
}

static void t_other_recipe() {
    printf("manual crafting stays: a recipe outside the queue is crafted normally and pauses a running queue\n");
    Fake f; fq::Ctl c; setup(f, c);
    f.inv = { { "Scrap Steel", 20 }, { "Fastening", 5 }, { "Scrap Metal", 6 } };
    f.open_menu(); f.user_plus("falchion");
    f.user_pick("horseshoe"); f.craft();                // not running: just a normal piece
    f.run(1);
    CHECK(!c.running() && c.q.pieces() == 1, "queue untouched by a normal piece");
}

static void t_native_result_close() {
    printf("native result modal close: one request only after queued quench; otherwise manual Esc fallback\n");
    Fake f; fq::Ctl c; setup(f, c);
    f.simulateResultScreen = true; c.autoDismiss = true;
    f.inv = { { "Scrap Steel", 8 }, { "Fastening", 5 }, { "Scrap Metal", 6 } };
    f.open_menu(); f.user_plus("hunting_sword"); f.user_plus("hunting_sword");
    c.start_in_menu(); f.run(1000);
    CHECK(f.made.size() == 2 && c.q.pieces() == 0, "queued 2 complete with result screen (%zu)", f.made.size());
    CHECK(f.closeRequests == 1, "only the FIRST result auto-dismissed (%d requests)", f.closeRequests);
    CHECK(said(f, "requested native result-screen close"), "native close handler requested");

    Fake fail; fq::Ctl pause; setup(fail,pause);
    fail.simulateResultScreen=true; fail.closeWorks=false; pause.autoDismiss=true;
    fail.inv={ { "Scrap Steel", 8 }, { "Fastening", 5 }, { "Scrap Metal", 6 } };
    fail.open_menu(); fail.user_plus("hunting_sword");fail.user_plus("hunting_sword");
    pause.start_in_menu(); fail.run(300);
    CHECK(!pause.running() && pause.q.pieces()==1,
          "failed native close pauses with 1 left, leaving player in charge");
    CHECK(said(fail,"close it with Esc"),"explicit manual Esc fallback logged");
}

static void setup_silent(Fake& f, fq::Ctl& c, bool pump, bool resumes) {
    setup(f, c);
    f.simulateResultScreen = true; c.autoDismiss = true;
    f.updateSilentOnResult = true; f.pumpDrives = pump; f.updateResumesAfterClose = resumes;
    f.inv = { { "Scrap Steel", 30 }, { "Fastening", 8 }, { "Scrap Metal", 8 } };
}

static void t_root_cause_regression() {
    printf("root cause (2026-11-03 log): the forge update is silent while the result dialog is open -> without the message-loop tick the queue never moves on\n");
    Fake f; fq::Ctl c; setup_silent(f, c, /*pump*/false, true);
    f.open_menu(); f.user_plus("hunting_sword"); f.user_plus("hunting_sword");
    c.start_in_menu(); f.run(3000);
    CHECK(f.made.size() == 1, "only the first piece was forged (%zu)", f.made.size());
    CHECK(c.ph == fq::WaitEnd && f.closeRequests == 0, "stuck in 'piece done' with the result screen up, as in the log (%s)", fq::phase_name(c.ph));
}

static void t_pump_full_queue() {
    printf("message-loop tick: 2 + 1 + 3 pieces, forge update silent on every result screen, completes unattended\n");
    for (int resumes = 0; resumes < 2; resumes++) {
        Fake f; fq::Ctl c; setup_silent(f, c, /*pump*/true, resumes != 0);
        f.open_menu();
        f.user_plus("hunting_sword"); f.user_plus("hunting_sword"); f.user_plus("falchion");
        f.user_plus("basilard"); f.user_plus("basilard"); f.user_plus("basilard");
        f.user_pick("horseshoe");
        CHECK(c.start_in_menu(), "start");
        f.run(6000);
        std::vector<std::string> want = { "hunting_sword", "hunting_sword", "falchion", "basilard", "basilard", "basilard" };
        CHECK(f.made == want, "[update resumes after close: %d] made all 6 in order (%zu)", resumes, f.made.size());
        CHECK(f.closeRequests == 5, "every result but the last closed by the native close (%d)", f.closeRequests);
        CHECK(f.wrongCrafts == 0 && f.crafts == 6, "6 game crafts, none wrong (%d, %d)", f.crafts, f.wrongCrafts);
        CHECK(!c.running() && c.q.n == 0, "queue finished");
        CHECK(f.pumpTicks > 0, "the message-loop tick did the post-quench work (%d ticks)", f.pumpTicks);
        CHECK(said(f, "done, 6 made"), "done line");
        int paused = 0; for (auto n : f.notes) if (n == fq::N_PAUSED || n == fq::N_CLOSE_RESULT) paused++;
        CHECK(paused == 0, "no pause notices");
    }
}

static void t_retired_forge_object() {
    printf("regression: retired C_Blacksmithing pointer must not block native result close\\n");
    Fake f; fq::Ctl c; setup_silent(f,c,/*pump*/true,/*resumes*/true);
    f.forgeObjectRetiredOnResult=true;
    f.open_menu(); f.user_plus("hunting_sword"); f.user_plus("hunting_sword");
    CHECK(c.start_in_menu(), "start");
    f.run(3000);
    CHECK(f.closeRequests==1, "first result closed while forge object unreachable (%d)",f.closeRequests);
    CHECK(f.made.size()==2, "second weapon completed after forge update resumes (%zu)",f.made.size());
    CHECK(!c.running(), "queue finished");
    CHECK(f.wrongCrafts==0, "no unintended crafting");
    CHECK(f.pumpTicks>0, "native close driven by pump");

    Fake noReopen; fq::Ctl stop; setup_silent(noReopen,stop,/*pump*/true,/*resumes*/false);
    noReopen.forgeObjectRetiredOnResult=true;
    noReopen.restartWorks=false; // stale object cannot be used to restart
    noReopen.open_menu();noReopen.user_plus("hunting_sword");noReopen.user_plus("hunting_sword");
    CHECK(stop.start_in_menu(), "start unreopenable");
    noReopen.run(1200);
    CHECK(noReopen.closeRequests==1, "modal still closes even with no forge restart (%d)",noReopen.closeRequests);
    CHECK(noReopen.made.size()==1 && stop.q.pieces()==1, "only one crafted; remaining item preserved");
    CHECK(stop.ph==fq::WaitUser, "waits for manual forge, never guesses a stale component (%s)",fq::phase_name(stop.ph));

    Fake delayed; fq::Ctl c3; setup_silent(delayed,c3,/*pump*/true,/*resumes*/false);
    delayed.forgeObjectRetiredOnResult=true;
    delayed.restartFailsFirstCalls=3; // component becomes reachable after several retries
    delayed.open_menu();delayed.user_plus("hunting_sword");delayed.user_plus("hunting_sword");
    c3.start_in_menu(); delayed.run(2000);
    CHECK(delayed.closeRequests==1 && delayed.made.size()==2,
          "native close followed by delayed component reacquisition completes two crafts (%zu)",delayed.made.size());
    CHECK(delayed.restartCalls>=4,"retry is attempted when game restores forge component (%d)",delayed.restartCalls);
    CHECK(!c3.running(),"delayed restart still finishes the queue");
}
static void t_fresh_component_queue() {
    printf("fresh component lookup after every native close: six queued recipes, no update until restart\n");
    Fake f; fq::Ctl c; setup_silent(f,c,true,false);
    f.forgeObjectRetiredOnResult=true;
    f.open_menu();
    f.user_plus("hunting_sword"); f.user_plus("hunting_sword"); f.user_plus("falchion");
    f.user_plus("basilard"); f.user_plus("basilard"); f.user_plus("basilard");
    f.user_pick("horseshoe"); c.start_in_menu(); f.run(6000);
    std::vector<std::string> want={"hunting_sword","hunting_sword","falchion","basilard","basilard","basilard"};
    CHECK(f.made==want,"all six made in order after repeated component retirement");
    CHECK(f.reacquireCalls==5 && f.closeRequests==5,"one lookup/close between pieces (%d/%d)",f.reacquireCalls,f.closeRequests);
    CHECK(f.staleStarts==0 && f.wrongCrafts==0,"no retired object start or unintended craft");
    CHECK(!c.running() && c.q.pieces()==0,"queue completed");

    Fake missing; fq::Ctl paused; setup_silent(missing,paused,true,false);
    missing.forgeObjectRetiredOnResult=true; missing.reacquireWorks=false;
    missing.open_menu(); missing.user_plus("hunting_sword"); missing.user_plus("basilard");
    paused.start_in_menu(); missing.run(1500);
    CHECK(missing.closeRequests==1 && missing.reacquireCalls>1,"close succeeds independently; missing component retried");
    CHECK(paused.ph==fq::WaitUser && paused.q.pieces()==1 && missing.crafts==1,"lookup failure preserves remaining queue without craft");
    CHECK(missing.staleStarts==0,"never starts with retired component");
    missing.reacquireWorks=true; missing.componentRetired=false; missing.open_menu();
    paused.tick(1,true); missing.run(2000);
    CHECK(missing.made==std::vector<std::string>({"hunting_sword","basilard"}),"manual recovery continues exact remaining recipe");
}
static void t_pump_close_slow_exit() {
    printf("the close does not end the session at once: the queue waits for the game to leave it, then goes on\n");
    Fake f; fq::Ctl c; setup_silent(f, c, true, true);
    f.closeLeavesSessionOpen = true;
    f.open_menu(); f.user_plus("hunting_sword"); f.user_plus("basilard");
    c.start_in_menu(); f.run(4000);
    CHECK(f.made.size() == 2 && !c.running(), "both made (%zu)", f.made.size());
    CHECK(f.closeRequests == 1, "one close request (%d)", f.closeRequests);
}

static void t_result_never_closes() {
    printf("result dialog never becomes closable (not captured): paused with the remaining items kept, after RESULT_WAIT_MS\n");
    Fake g; fq::Ctl d; setup(g, d); d.autoDismiss = true;     // the fake never reports a dialog; the session just stays open
    g.inv = { { "Scrap Steel", 30 }, { "Fastening", 8 }, { "Scrap Metal", 8 } };
    g.open_menu(); g.user_plus("hunting_sword"); g.user_plus("hunting_sword");
    d.start_in_menu(); while (d.ph != fq::WaitEnd && g.t < 100000) { g.step(); }
    g.active = true; g.state = 18; g.pieceTicks = -1000000;     // session stays open, no dialog
    g.run(700);                                           // 35 s
    CHECK(!d.running() && d.q.pieces() == 1, "paused with 1 left (%d, %s)", d.q.pieces(), fq::phase_name(d.ph));
    CHECK(said(g, "result screen did not appear or could not be closed"), "pause reason logged");
    bool closeNote = false; for (auto n : g.notes) if (n == fq::N_CLOSE_RESULT) closeNote = true;
    CHECK(closeNote, "HUD says to close the result screen");
    CHECK(g.closeRequests == 0, "nothing was closed");
}

static void t_notes() {
    printf("HUD notices: only the mod's own localization keys (no free text, no '@'), short\n");
    for (int i = 0; i < fq::N_COUNT; i++) {
        const char* k = fq::note_key((fq::Note)i);
        CHECK(!strncmp(k, "taf_", 4) && !strchr(k, '@') && !strchr(k, ' '), "key %d '%s' is a plain taf_ key", i, k);
    }
    Fake f; fq::Ctl c; setup_silent(f, c, true, true);
    f.open_menu(); f.user_plus("hunting_sword"); f.user_plus("basilard"); f.user_plus("basilard");
    c.start_in_menu(); f.run(6000);
    CHECK(!f.notes.empty() && f.notes.front() == fq::N_STARTED, "starts with the 'started' notice");
    CHECK(f.notes.back() == fq::N_DONE, "ends with the 'finished' notice");
    std::vector<std::pair<int,int>> want = { { 1, 3 }, { 2, 3 }, { 3, 3 } };
    CHECK(f.pieceNotes == want, "item notices 1/3 2/3 3/3 (%zu)", f.pieceNotes.size());
    for (auto& l : f.lines) CHECK(l.find('@') == std::string::npos || l.find("ui_") != std::string::npos, "log line has no stray '@': %s", l.c_str());
}

int main() {
    t_localization();
    t_native_result_close();
    t_root_cause_regression(); t_pump_full_queue(); t_retired_forge_object(); t_pump_close_slow_exit(); t_result_never_closes(); t_notes();
    t_fresh_component_queue();
    t_model(); t_full_run(); t_run_out(); t_focus_fails(); t_restart_refused(); t_cancel(); t_no_autocontinue(); t_other_recipe();
    printf(g_fail ? "%d FAILED\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
