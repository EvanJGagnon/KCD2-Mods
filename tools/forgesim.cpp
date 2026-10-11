// forgesim.cpp -- offline forge simulator for the development build of kcd2_autoforge.dll.
// Builds a fake C_Blacksmithing / model / workpiece in plain memory with the field offsets the mod reads, and plays
// the minigame with the rules measured or decoded from build 15693:
//   rhythm (0x11e0064): hits keep the last 2 intervals; when off it turns on once their std-dev < 0.28 s
//     (period = mean); when on, a gap > 0.28 s off the period resets it, and so does no hit within period + 0.28^2 s;
//     a hit is scored with the rhythm state from before it.
//   effectivity ~ 0.7 + 0.7*work (+0.8 with rhythm), clamped to 0..1; completion += 0.02*eff;
//     quality -= 0.017*(1-eff)^1.3; a miss = eff 0 (-0.017); a full hit wears the work map by 0.6.
//   flip = 1.25 s at the idle anvil (state 12/13), then the other side's work map is used.
// The numbers are approximations - this checks the mod's decisions and flow, not the game's exact results.
// Usage: forgesim <dev kcd2_autoforge.dll> <piece: sword|longsword|axe|horseshoe> <FlipAt %> [side1: same|mirror] [log]
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

static uint64_t g_now = 0;
static uint64_t sim_clock() { return g_now; }

// ---- fake objects (offsets as used by autoforge.cpp) ----
alignas(16) static uint8_t BS[0x200], MODEL[0x400], WP[0x200], DEF[0x100], ACTOR[0x700], SOUL[0x800];
alignas(16) static uint8_t ZONE[8][0x20];
static void* ZPTR[8];
static void* ACT_VT[12];
template<class T> static T& at(uint8_t* b, size_t o) { return *(T*)(b + o); }
static float& comp() { return at<float>(MODEL, 0x38); }
static float& qual() { return at<float>(MODEL, 0x60); }
static uint8_t& state() { return at<uint8_t>(MODEL, 0x88); }
static uint8_t& hard() { return at<uint8_t>(MODEL, 0xB0); }
static uint8_t& bellows() { return at<uint8_t>(MODEL, 0x100); }
static float& fire() { return at<float>(MODEL, 0x128); }
static int& zone() { return at<int>(MODEL, 0x150); }
static float* pos() { return &at<float>(MODEL, 0x1A0); }
static float& bpm() { return at<float>(MODEL, 0x220); }
static float& charge() { return at<float>(MODEL, 0x270); }
static float& ztemp(int i) { return at<float>(ZONE[i], 8); }

// ---- piece ----
static const char* g_piece = "sword"; static bool g_mirror1 = false; static int g_nz = 3; static bool g_1d = true;
static int g_side = 0; static float g_cool = 6.f;   // anvil cooling, C/s (raise it to force reheats)
static const int MX = 40, MY = 20;
static float g_map[2][MX][MY];
static float bx0, by0, bx1, by1;   // current clamp bounds (same lerp as the game)
static void cur_bounds() {
    float c = comp(); auto L = [&](size_t a, size_t b, int k) { float s = at<float>(DEF, a + 4 * k), e = at<float>(DEF, b + 4 * k); return s + (e - s) * c; };
    bx0 = L(0x5C, 0x64, 0); by0 = L(0x5C, 0x64, 1); bx1 = L(0x6C, 0x74, 0); by1 = L(0x6C, 0x74, 1);
}
static int cxi(float x) { return std::min(MX - 1, std::max(0, (int)(x * MX))); }
static int cyi(float y) { return std::min(MY - 1, std::max(0, (int)(y * MY))); }
// 2D hit geometry (side 0 as traced; side 1 optionally mirrored along x)
struct P2 { float x, y; };
static const P2 SHOE[] = { {0.82f,0.34f},{0.12f,0.34f},{0.06f,0.45f},{0.02f,0.55f},{0.03f,0.65f},{0.12f,0.74f},{0.12f,0.77f},{0.82f,0.80f} };
static float seg_d(P2 a, P2 b, float x, float y) {
    float dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy, t = l > 0 ? ((x - a.x) * dx + (y - a.y) * dy) / l : 0; t = std::min(1.f, std::max(0.f, t));
    float px = a.x + t * dx - x, py = a.y + t * dy - y; return sqrtf(px * px + py * py);
}
static bool on_piece(float x, float y) {
    if (g_1d) return true;
    if (g_side == 1 && g_mirror1) x = 0.84f - x;
    if (!strcmp(g_piece, "horseshoe")) { float d = 9; for (int i = 0; i + 1 < (int)(sizeof SHOE / sizeof SHOE[0]); i++) d = std::min(d, seg_d(SHOE[i], SHOE[i + 1], x, y)); return d < 0.055f; }
    float cy = g_side ? 0.55f : 0.45f; float ex = (x - 0.5f) / 0.33f, ey = (y - cy) / 0.28f; return ex * ex + ey * ey < 1.f;   // axe head
}
static float workmap(void* /*wp+0x40*/, const float* p) { return g_map[g_side][cxi(p[0])][cyi(g_1d ? 0.f : p[1])]; }

// ---- rhythm (decoded 0x11e0064 / 0x11e0710) ----
static bool r_on = false; static double r_last = 0, r_period = 0; static std::vector<double> r_iv;
static void r_reset() { r_on = false; r_iv.clear(); r_last = 0; bpm() = 0; }
static void r_hit(double t) {
    if (r_last > 0) {
        double iv = t - r_last; r_iv.push_back(iv);
        if ((int)r_iv.size() >= 2) {
            if (!r_on) { double m = 0, v = 0; for (double x : r_iv) m += x; m /= r_iv.size(); for (double x : r_iv) v += (x - m) * (x - m); v = sqrt(v / r_iv.size());
                if (v < 0.28) { r_on = true; r_period = m; bpm() = (float)(60.0 / m); } }
            else if (fabs(iv - r_period) > 0.28) { r_reset(); return; }
        }
        while ((int)r_iv.size() > 2) r_iv.erase(r_iv.begin());
    }
    r_last = t;
}

// ---- timed events ----
static uint64_t ev_anvil = 0, ev_forge = 0, ev_flip = 0, ev_hit = 0, ev_idle = 0, ev_hard = 0;
static int g_strokes = 0, g_hitsN = 0, g_missN = 0, g_flipsN = 0;
static void a_toanvil(void*) { if (state() == 8) { state() = 6; zone() = -1; ev_anvil = g_now + 2000; } }
static void a_toforge(void*) { if (state() == 11) { state() = 5; ev_forge = g_now + 2000; } }
static void a_toharden(void*) { if (state() == 20 && !hard()) { hard() = 1; ev_hard = g_now + 3000; } }
static void a_nop(void*) {}
static void a_bellows(void*, bool on) { bellows() = on; }
static void a_move(void*, uint64_t packed) { float d[2]; memcpy(d, &packed, 8); cur_bounds();
    pos()[0] = std::min(bx1, std::max(bx0, pos()[0] + d[0])); pos()[1] = std::min(by1, std::max(by0, pos()[1] + d[1])); }
static void a_flip(void*) { if (state() == 11) { state() = g_side ? 13 : 12; ev_flip = g_now + 1250; } }
static void a_start(void*) { if (state() == 11 || state() == 16) state() = 14; }
static void a_finish(void*) { if (state() == 14) { state() = 16; ev_hit = g_now + 300; } }
static void a_zone(void*, int* z) { zone() = std::min(g_nz - 1, std::max(0, *z)); }

static void hit() {
    float x = pos()[0], y = pos()[1]; g_strokes++;
    int z = std::min(g_nz - 1, std::max(0, (int)(x * g_nz)));
    bool on = on_piece(x, y) && ztemp(z) > 600.f;
    float w = g_map[g_side][cxi(x)][cyi(g_1d ? 0.f : y)];
    float eff = on ? std::min(1.f, std::max(0.f, 0.7f + 0.7f * w + (r_on ? 0.8f : 0.f))) : 0.f;
    comp() = std::min(1.f, comp() + 0.02f * eff); qual() -= 0.017f * powf(1.f - eff, 1.3f); charge() = eff;
    if (on) { g_hitsN++; int i = cxi(x), j = cyi(g_1d ? 0.f : y);
        for (int di = -1; di <= 1; di++) for (int dj = -1; dj <= 1; dj++) { int a = i + di, b = j + dj; if (a < 0 || a >= MX || b < 0 || b >= MY) continue;
            float k = (di == 0 && dj == 0) ? 0.6f : 0.25f; g_map[g_side][a][b] = std::max(-1.f, g_map[g_side][a][b] - k * eff); } }
    else g_missN++;
    r_hit(g_now / 1000.0);
    static int grown = 0; int g = (int)(comp() * 10);                    // the map regrows as the piece takes shape
    for (; grown < g; grown++) for (int s = 0; s < 2; s++) for (int i = 0; i < MX; i++) for (int j = 0; j < MY; j++)
        g_map[s][i][j] = std::min(1.f, g_map[s][i][j] + 0.3f);
    if (comp() >= 0.999f) { ev_idle = 0; state() = 20; } else ev_idle = g_now + 250;
}

static void sim_step(uint64_t dt) {
    g_now += dt; float s = dt / 1000.f;
    if (ev_anvil && g_now >= ev_anvil) { ev_anvil = 0; state() = 11; }
    if (ev_forge && g_now >= ev_forge) { ev_forge = 0; state() = 8; zone() = 0; }
    if (ev_flip && g_now >= ev_flip) { ev_flip = 0; g_side ^= 1; g_flipsN++; state() = 11; }
    if (ev_hit && g_now >= ev_hit) { ev_hit = 0; hit(); }
    if (ev_idle && g_now >= ev_idle) { ev_idle = 0; if (state() == 16) state() = 11; }
    if (ev_hard && g_now >= ev_hard) { ev_hard = 0; hard() = 2; }
    if (r_on && g_now / 1000.0 - r_last > r_period + 0.28 * 0.28) r_reset();     // rhythm timer
    fire() += (state() == 8 && bellows()) ? 60.f * s : -15.f * s; fire() = std::min(1500.f, std::max(200.f, fire()));
    for (int i = 0; i < g_nz; i++) {
        if (state() == 8) { float k = (i == zone()) ? 0.4f : (abs(i - zone()) == 1 ? 0.12f : 0.04f); ztemp(i) += (fire() - ztemp(i)) * k * s; }
        else ztemp(i) = std::max(30.f, ztemp(i) - g_cool * s);
    }
}

int main(int argc, char** argv) {
    if (argc < 4) { printf("usage: forgesim <dev dll> <sword|longsword|axe|horseshoe> <FlipAt%%> [same|mirror] [log]\n"); return 2; }
    g_piece = argv[2]; int flipAt = atoi(argv[3]); g_mirror1 = argc > 4 && !strcmp(argv[4], "mirror");
    const char* logPath = argc > 5 ? argv[5] : "forgesim.log"; if (argc > 6) g_cool = (float)atof(argv[6]);
    SetEnvironmentVariableA("KC_SELFTEST", "1");                        // no startup thread in the DLL
    HMODULE m = LoadLibraryA(argv[1]); if (!m) { printf("cannot load %s\n", argv[1]); return 2; }
    auto init = (void(*)(uintptr_t, void*, uint64_t(*)(), int, const char*))GetProcAddress(m, "KC_SimInit");
    auto start = (void(*)())GetProcAddress(m, "KC_SimStart"); auto tick = (int(*)())GetProcAddress(m, "KC_SimTick");
    if (!init || !start || !tick) { printf("not a dev build (KC_Sim* exports missing)\n"); return 2; }

    // wire the fake objects
    at<void*>(BS, 0x18) = ACTOR; at<void*>(ACTOR, 0x668) = SOUL; at<float>(SOUL, 0x70C) = 60.f;
    at<void*>(BS, 0x68) = ACT_VT; at<void*>(BS, 0x98) = MODEL; at<void*>(MODEL, 0x2F8) = WP; at<void*>(WP, 0x18) = DEF;
    void* fns[12] = { (void*)a_toanvil, (void*)a_toforge, (void*)a_toharden, (void*)a_nop, (void*)a_bellows, (void*)a_move,
                      (void*)a_flip, (void*)a_start, (void*)a_finish, (void*)a_zone, (void*)a_nop, (void*)a_nop };
    memcpy(ACT_VT, fns, sizeof fns);
    static char id[32], tag[32]; strcpy_s(id, g_piece); strcpy_s(tag, strstr(g_piece, "sword") ? "sword" : g_piece);
    at<char*>(DEF, 0x08) = id; at<char*>(DEF, 0x18) = tag;
    auto setb = [](float smx, float smy, float emx, float emy, float sXx, float sXy, float eXx, float eXy) {
        float v[8] = { smx, smy, emx, emy, sXx, sXy, eXx, eXy }; memcpy(&at<float>(DEF, 0x5C), v, 16); memcpy(&at<float>(DEF, 0x6C), v + 4, 16); };
    if (!strcmp(g_piece, "sword")) { g_nz = 3; setb(0, 0, 0, 0, 0.85f, 0, 0.95f, 0); }
    else if (!strcmp(g_piece, "longsword")) { g_nz = 3; setb(0, 0, 0, 0, 1, 1, 1, 1); }
    else if (!strcmp(g_piece, "horseshoe")) { g_nz = 1; setb(0, 0.2f, 0, 0.2f, 0.85f, 0.9f, 0.85f, 0.95f); }
    else { g_nz = 1; setb(0, 0, 0, 0, 1, 1, 1, 1); }
    g_1d = strstr(g_piece, "sword") != nullptr;
    for (int i = 0; i < g_nz; i++) { ZPTR[i] = ZONE[i]; ztemp(i) = 40.f; }
    at<void*>(WP, 0x28) = &ZPTR[0]; at<void*>(WP, 0x30) = &ZPTR[g_nz];
    // logged swords read 0.0 along most of the blade at the start and ~0.9-1.0 near the tip; 2D pieces mixed
    for (int s = 0; s < 2; s++) for (int i = 0; i < MX; i++) for (int j = 0; j < MY; j++)
        g_map[s][i][j] = g_1d ? ((i + 0.5f) / MX >= 0.72f ? 1.f : 0.f) : (((i * 7 + j * 3) % 5) < 2 ? 0.f : 1.f);
    comp() = 0; qual() = 1; state() = 8; fire() = 200; zone() = 0; pos()[0] = 0.5f; pos()[1] = 0.5f;

    init((uintptr_t)BS, (void*)workmap, sim_clock, flipAt, logPath);
    g_now = 1000; start();
    int ph = 0; uint64_t t0 = g_now;
    for (int i = 0; i < 400000; i++) {                                   // 20 ms ticks, up to ~2 h of game time
        sim_step(20); ph = tick();
        if (ph == 0 && g_now - t0 > 3000) break;                          // mod went idle (done or stopped)
    }
    printf("%-9s FlipAt=%-2d side1=%-6s  q=%.3f comp=%.3f strokes=%d hits=%d misses=%d flips=%d time=%.1fs hard=%d\n",
        g_piece, flipAt, g_mirror1 ? "mirror" : "same", qual(), comp(), g_strokes, g_hitsN, g_missN, g_flipsN, (g_now - t0) / 1000.0, hard());
    return 0;
}
