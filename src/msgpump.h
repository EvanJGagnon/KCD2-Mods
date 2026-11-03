// msgpump.h -- run a callback on the game's main (window) thread, even while the game is paused.
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Why: while the game's item dialog ("Smithing successful") is open, the game stops updating the forge, so nothing
// hooked into the forge update runs and nothing there can close the dialog.  The engine's message loop
// (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) + DispatchMessageA, WHGame.dll 0x8643DA) keeps running, so:
//   - a small helper thread posts a private message to the game window while somebody wants a tick,
//   - a WH_GETMESSAGE hook installed on the window's own thread sees it as the loop retrieves it and calls back there.
// The callback therefore runs on the same thread as the game's own input and UI code, between frames, never in
// parallel with it.  Nothing is subclassed; the message itself is ignored by the window procedure.
#pragma once
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <utility>

namespace kc {

struct MainPump {
    typedef void (*Tick)();
    typedef bool (*Want)();
    typedef void (*Log)(const char* fmt, ...);

    // configuration (set before start)
    Tick  tick = nullptr;           // runs on the window thread
    Want  want = nullptr;           // polled by the helper thread: post a tick now?
    Log   log = nullptr;
    DWORD periodMs = 40;
    DWORD findTimeoutMs = 120000;   // how long to wait for the game window to exist
    // state
    HMODULE self = nullptr; HWND wnd = nullptr; DWORD tid = 0; HHOOK hook = nullptr; HANDLE thread = nullptr;
    volatile LONG pending = 0, stopFlag = 0, ticks = 0, posts = 0;
    DWORD hookThreadTid = 0;
    static constexpr UINT MSG_TICK = WM_APP + 0x3A7;

    static MainPump*& instance() { static MainPump* p = nullptr; return p; }

    static BOOL CALLBACK enumWnd(HWND h, LPARAM lp) {
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
        RECT r; if (!GetClientRect(h, &r)) return TRUE;
        long area = (r.right - r.left) * (r.bottom - r.top);
        auto* best = (std::pair<HWND, long>*)lp;
        if (area > best->second) { best->first = h; best->second = area; }
        return TRUE;
    }
    // The biggest visible top-level window of this process.
    static HWND find_window() {
        std::pair<HWND, long> best(nullptr, 0);
        EnumWindows(enumWnd, (LPARAM)&best);
        return best.second > 0 ? best.first : nullptr;
    }

    static LRESULT CALLBACK hookProc(int code, WPARAM w, LPARAM l) {
        MainPump* p = instance();
        if (p && code == HC_ACTION && (w & PM_REMOVE) && l) {
            const MSG* m = (const MSG*)l;
            if (m->message == MSG_TICK) {
                InterlockedExchange(&p->pending, 0);
                InterlockedIncrement(&p->ticks);
                if (p->tick) p->tick();
            }
        }
        return CallNextHookEx(nullptr, code, w, l);
    }

    static DWORD WINAPI threadMain(LPVOID lp) {
        MainPump* p = (MainPump*)lp;
        DWORD waited = 0;
        while (!p->stopFlag && !p->wnd && !(p->wnd = find_window())) { Sleep(500); waited += 500; if (waited > p->findTimeoutMs) break; }
        if (!p->wnd) { if (p->log) p->log("pump: no game window found - main-thread ticks off"); return 0; }
        p->tid = GetWindowThreadProcessId(p->wnd, nullptr);
        p->hook = SetWindowsHookExW(WH_GETMESSAGE, hookProc, p->self, p->tid);
        if (!p->hook) { if (p->log) p->log("pump: could not hook the window thread (error %lu) - main-thread ticks off", GetLastError()); return 0; }
        if (p->log) p->log("pump: main-thread tick ready (window %p, thread %lu)", (void*)p->wnd, p->tid);
        DWORD stuck = 0;
        while (!p->stopFlag) {
            Sleep(p->periodMs);
            if (!p->want || !p->want()) { stuck = 0; continue; }
            if (p->pending) { stuck += p->periodMs; if (stuck < 1500) continue; }   // not picked up for a while: send again
            stuck = 0; InterlockedExchange(&p->pending, 1); InterlockedIncrement(&p->posts);
            if (!PostMessageW(p->wnd, MSG_TICK, 0, 0)) PostThreadMessageW(p->tid, MSG_TICK, 0, 0);
        }
        return 0;
    }

    bool start(HMODULE mod) {
        self = mod; instance() = this;
        thread = CreateThread(nullptr, 0, threadMain, this, 0, nullptr);
        return thread != nullptr;
    }
    // Same thing for a window the caller already has (tests): skips the window search.
    bool start_for(HMODULE mod, HWND w) {
        self = mod; instance() = this; wnd = w;
        thread = CreateThread(nullptr, 0, threadMain, this, 0, nullptr);
        return thread != nullptr;
    }
    void shutdown() {
        stopFlag = 1;
        if (thread) { WaitForSingleObject(thread, 2000); CloseHandle(thread); thread = nullptr; }
        if (hook) { UnhookWindowsHookEx(hook); hook = nullptr; }
    }
};

}  // namespace kc
