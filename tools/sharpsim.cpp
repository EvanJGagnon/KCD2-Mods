// sharpsim.cpp -- offline grindstone simulator for the development build of kcd2_autosharpen.dll.
// A fake C_Sharpening in plain memory (the field offsets the mod reads) plus fake pedal / blade-position /
// blade-angle functions, playing the minigame with the rules decoded from build 15693:
//   wheel: -0.2/s - 0.3*0.3/s while sharpening; a pedal push adds 0.3 (cap 1) and costs stamina.
//   blade: velocity input like keys/sticks (|v|<0.3 -> 0, |v|>=0.7 -> 1), 0.9/s position, 0.5/s angle.
//   efficiency (0x2cf588c): ramp 0..1 on [0,A], 1 on [A,B], 1..0 on [B,C], negative above C, with
//     A=lerp(0.55,0.2,t) C=lerp(0.7,0.99,t) B=0.55-0.7+C for skill factor t; a segment stops gaining once it
//     reaches MinEff + sqrt(e)*(1-MinEff); gain per second = speed * pressure * e * 0.5.
//   bounce: every <interval> of wheel-time the angle drops by rand*0.029.
// Usage: sharpsim <dev dll> <segments 1-5> <skill t 0..1> [bounce s] [stamina regen/s] [band shift] [log]
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

static uint64_t g_now = 0;
static uint64_t sim_clock() { return g_now; }
alignas(16) static uint8_t S[0x300], ACTOR[0x700], SOUL[0x800], SEGS[0x70];
template<class T> static T& at(uint8_t* b, size_t o) { return *(T*)(b + o); }
static float& eff() { return at<float>(S, 0x84); }
static float& press() { return at<float>(S, 0x88); }
static float& pos() { return at<float>(S, 0x8C); }
static float& rot() { return at<float>(S, 0x90); }
static float& posvel() { return at<float>(S, 0x9C); }
static float& rotvel() { return at<float>(S, 0xA0); }
static float& speed() { return at<float>(S, 0xAC); }
static float* segs() { return (float*)SEGS; }
static int& nseg() { return at<int>(S, 0xD8); }
static float& segw() { return at<float>(S, 0xDC); }
static float& stam() { return at<float>(SOUL, 0x70C); }

static float g_t = 0.5f, g_bounce = 0.6f, g_regen = 20.f, g_shift = 0.f, g_minEff = 0.5f, g_pedalCost = 8.f;
static float g_btimer = 0; static uint32_t g_rng = 12345;
static float rnd() { g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) / 16777216.f; }
static int g_pedals = 0, g_damageFrames = 0, g_frames = 0; static double g_effSum = 0; static float g_rotMin = 9, g_rotMax = -9;

static float stick(float v) { if (v > 0.7f) return 1; if (v < 0.3f && v > -0.3f) return 0; if (v < -0.7f) return -1; return v; }
static bool f_pedal(void*) {
    if (!at<uint8_t>(S, 0x2C)) return false;
    if (stam() < g_pedalCost) return false;
    stam() -= g_pedalCost; g_pedals++;
    speed() += 0.3f; if (speed() > 1.f) { speed() = 1.f; return true; } return false;
}
static void f_posdir(void*, float v) { at<float>(S, 0x94) = v; posvel() = stick(v); }
static void f_rotdir(void*, float v) { at<float>(S, 0x98) = v; rotvel() = stick(v); }

static float curve(float a) {
    float C = 0.7f + (0.99f - 0.7f) * g_t, A = 0.55f + (0.2f - 0.55f) * g_t, Bv = 0.55f - 0.7f + C;
    A += g_shift; Bv += g_shift; C += g_shift;               // shifted band: tests the mod's fallback
    if (a < A) return a / A;
    if (a < Bv) return 1.f;
    if (a < C) return (C - a) / (C - Bv);
    return -(a - C) / (1.f - C);
}
static void step(float dt) {
    g_now += (uint64_t)(dt * 1000);
    speed() = std::max(0.f, speed() - dt * 0.2f - dt * 0.3f * 0.3f);
    pos() = std::min(0.5f, std::max(-0.5f, pos() + dt * 0.9f * posvel()));
    rot() = std::min(1.f, std::max(0.f, rot() + dt * 0.5f * rotvel()));
    stam() = std::min(150.f, stam() + g_regen * dt);
    int i = std::min(nseg() - 1, std::max(0, (int)((pos() + 0.5f) / segw())));
    float e = 0;
    if (speed() > 0 && rot() > 0 && press() > 0) {
        e = curve(rot()); float cap = e <= 0 ? g_minEff : g_minEff + sqrtf(e) * (1 - g_minEff);
        if (segs()[i] >= cap && e >= 0) e = 0;
        segs()[i] = std::min(1.f, std::max(0.f, segs()[i] + dt * speed() * press() * e * 0.5f));
        g_btimer += dt * speed();
        if (g_btimer > g_bounce) { g_btimer = 0; rot() = std::max(0.f, rot() - rnd() * 0.029f); }
        g_frames++; g_effSum += e; if (e < 0) g_damageFrames++;
        g_rotMin = std::min(g_rotMin, rot()); g_rotMax = std::max(g_rotMax, rot());
    }
    eff() = e;
}

int main(int argc, char** argv) {
    if (argc < 4) { printf("usage: sharpsim <dev dll> <segments 1-5> <skill 0..1> [bounce s] [stamina regen/s] [band shift] [log]\n"); return 2; }
    int n = atoi(argv[2]); g_t = (float)atof(argv[3]);
    if (argc > 4) g_bounce = (float)atof(argv[4]); if (argc > 5) g_regen = (float)atof(argv[5]); if (argc > 6) g_shift = (float)atof(argv[6]);
    const char* logPath = argc > 7 ? argv[7] : "sharpsim.log"; float stam0 = argc > 8 ? (float)atof(argv[8]) : 120.f;
    SetEnvironmentVariableA("KC_SELFTEST", "1");
    HMODULE m = LoadLibraryA(argv[1]); if (!m) { printf("cannot load %s\n", argv[1]); return 2; }
    auto init = (void(*)(uintptr_t, void*, void*, void*, uint64_t(*)(), const char*))GetProcAddress(m, "KC_SimInit");
    auto start = (void(*)())GetProcAddress(m, "KC_SimStart"); auto tick = (int(*)())GetProcAddress(m, "KC_SimTick");
    if (!init || !start || !tick) { printf("not a dev build (KC_Sim* exports missing)\n"); return 2; }

    at<void*>(S, 0x18) = ACTOR; at<void*>(ACTOR, 0x668) = SOUL; stam() = stam0;
    at<uint8_t>(S, 0x2C) = 1; at<int>(S, 0x18C) = 4; press() = 1.f;
    at<void*>(S, 0xC8) = SEGS; nseg() = n; segw() = 1.f / n;
    float seg0 = argc > 9 ? (float)atof(argv[9]) : -1.f;
    for (int i = 0; i < n; i++) segs()[i] = seg0 >= 0 ? seg0 : 0.2f + 0.5f * rnd();
    pos() = 0; rot() = 0; speed() = 0;
    char start0[64]; size_t k = 0; for (int i = 0; i < n; i++) k += sprintf_s(start0 + k, sizeof start0 - k, "%s%.2f", i ? "/" : "", segs()[i]);

    init((uintptr_t)S, (void*)f_pedal, (void*)f_posdir, (void*)f_rotdir, sim_clock, logPath);
    g_now = 1000; start();
    uint64_t t0 = g_now; int on = 1;
    int last = 1;
    for (int f = 0; f < 60 * 600 && on; f++) { step(1.f / 60.f); last = tick(); on = last == 1; }   // up to 10 minutes
    char fin[64]; k = 0; float mn = 9; for (int i = 0; i < n; i++) { k += sprintf_s(fin + k, sizeof fin - k, "%s%.3f", i ? "/" : "", segs()[i]); mn = std::min(mn, segs()[i]); }
    printf("%s ", last == 2 ? "[gets up]" : "[stays]  ");
    printf("segs=%d skill=%.1f bounce=%.1fs regen=%-4.0f shift=%+.2f  [%s] -> [%s] %s  time=%.1fs pedals=%d angle %.2f..%.2f avgEff=%.2f damageFrames=%d\n",
        n, g_t, g_bounce, g_regen, g_shift, start0, fin, mn >= 0.99f ? "SHARP" : "not sharp", (g_now - t0) / 1000.0, g_pedals, g_rotMin, g_rotMax,
        g_frames ? g_effSum / g_frames : 0.0, g_damageFrames);
    return 0;
}
