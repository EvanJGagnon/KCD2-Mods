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
    std::string pendingFocus; int focusDelay = 0; bool flashWorks = true, restartWorks = true;
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
    bool restart() override { if (!restartWorks || active) return false; open_menu(); return true; }
    bool result_open() override { return simulateResultScreen && resultWaiting && active && state==18; }
    bool dismiss_result() override {
        if (!result_open() || !closeWorks) return false;
        closeRequests++; resultWaiting=false; active=false; state=22; return true;
    }
    void say(const char* line) override { lines.push_back(line); if (getenv("FQ_VERBOSE")) printf("    | %s\n", line); }
    // ---- the fake game ----
    void draw_page() {
        pageId = focusedId; pageT = t;
        const Recipe* r = rec(pageId); if (!r) return;
        std::vector<fq::Row> rows; std::vector<int> have;
        for (auto& m : r->mats) { fq::Row x{}; fq::copy(x.name, fq::NAMELEN, m.first.c_str()); x.need = m.second; rows.push_back(x); have.push_back(inv[m.first]); }
        ctl->q.page(r->id.c_str(), r->title.c_str(), rows.data(), (int)rows.size(), have.data(), (void*)r);
    }
    void open_menu() { active = true; state = 1; resultWaiting = false; pageT = 0; draw_page(); }
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
            }
        }
        ctl->tick(state, active);
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
    CHECK(said(fail,"dismiss manually with Esc"),"explicit manual Esc fallback logged");
}
int main() {
    t_localization();
    t_native_result_close();
    t_model(); t_full_run(); t_run_out(); t_focus_fails(); t_restart_refused(); t_cancel(); t_no_autocontinue(); t_other_recipe();
    printf(g_fail ? "%d FAILED\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
