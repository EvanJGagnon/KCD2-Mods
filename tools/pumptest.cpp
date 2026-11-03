// pumptest.cpp -- offline test of src/msgpump.h: a callback must run on the window's own thread, driven only by the
// message loop, while the "game" does nothing else (the paused state behind the result dialog).
// The loop is the engine's: PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE); TranslateMessage; DispatchMessageA.
// Build: cl /nologo /O2 /EHsc /MT /Isrc tools\pumptest.cpp user32.lib
#include "msgpump.h"
#include <cstdio>
#include <cstdarg>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static DWORD g_mainTid = 0;
static volatile LONG g_ticks = 0, g_wrongThread = 0, g_unknownMsgs = 0, g_wantOn = 0, g_inTick = 0, g_overlap = 0;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kc::MainPump::MSG_TICK) g_unknownMsgs++;       // the window procedure sees the private message and ignores it
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}
static void tick() {
    if (GetCurrentThreadId() != g_mainTid) g_wrongThread++;
    if (InterlockedIncrement(&g_inTick) != 1) g_overlap++;  // never re-entered / parallel
    InterlockedIncrement(&g_ticks);
    Sleep(1);
    InterlockedDecrement(&g_inTick);
}
static bool want() { return g_wantOn != 0; }
static void lg(const char* f, ...) { va_list a; va_start(a, f); printf("    | "); vprintf(f, a); printf("\n"); va_end(a); }

// the engine's pump, run for `ms` while doing nothing else (no game update)
static void engine_pump(DWORD ms, bool peekNoRemoveFirst) {
    DWORD t0 = GetTickCount();
    MSG msg;
    while (GetTickCount() - t0 < ms) {
        if (peekNoRemoveFirst) PeekMessageA(&msg, NULL, 0, 0, PM_NOREMOVE);
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(5);
    }
}

int main() {
    g_mainTid = GetCurrentThreadId();
    WNDCLASSA wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "PumpTestWnd";
    RegisterClassA(&wc);
    HWND w = CreateWindowA("PumpTestWnd", "pumptest", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(w != nullptr, "window");
    kc::MainPump p; p.tick = tick; p.want = want; p.log = lg; p.periodMs = 20;
    CHECK(kc::MainPump::find_window() == w, "window search finds this process's only visible top-level window");
    CHECK(p.start((HMODULE)wc.hInstance), "start");               // finds the window itself, like in the game
    engine_pump(600, false);
    CHECK(g_ticks == 0, "no ticks while nobody wants one (%ld)", (long)g_ticks);
    g_wantOn = 1;
    engine_pump(1000, false);
    LONG t1 = g_ticks;
    CHECK(t1 >= 15, "ticks arrive while wanted, ~20 ms apart (%ld in 1 s)", (long)t1);
    CHECK(g_wrongThread == 0, "every tick ran on the window's thread (%ld wrong)", (long)g_wrongThread);
    CHECK(g_overlap == 0, "ticks never overlap");
    CHECK(g_unknownMsgs >= 1, "window procedure also got the private message and ignored it");
    g_wantOn = 0;
    engine_pump(300, false);
    LONG t2 = g_ticks; engine_pump(400, false);
    CHECK(g_ticks == t2, "ticks stop when not wanted (%ld -> %ld)", (long)t2, (long)g_ticks);
    // an engine that peeks without removing first must not double-count a message
    g_wantOn = 1; LONG posts0 = p.posts, ticks0 = g_ticks;
    engine_pump(600, true);
    LONG dp = p.posts - posts0, dt = g_ticks - ticks0;
    CHECK(dt <= dp + 1 && dt > 0, "PM_NOREMOVE peeks do not add ticks (%ld ticks for %ld posts)", (long)dt, (long)dp);
    g_wantOn = 0;
    // a stalled loop (the engine busy for a while) must not pile up messages
    g_wantOn = 1; Sleep(700); g_wantOn = 0;
    LONG q0 = g_ticks; engine_pump(200, false);
    CHECK(g_ticks - q0 <= 2, "no backlog after a stall (%ld ticks)", (long)(g_ticks - q0));
    p.shutdown();
    DestroyWindow(w);
    printf(g_fail ? "%d FAILED\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
