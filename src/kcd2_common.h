// kcd2_common.h -- shared runtime for the KCD2 native mods (Theatrical AutoForge, Theatrical Autobrew,
// Horse Route Follow).  Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Update safety: nothing in WHGame.dll is addressed by a fixed offset.  Functions and call sites are
// found by byte signatures (rip-relative displacements and branch targets wildcarded; struct offsets
// kept literal so a layout change breaks the match), classes by their RTTI names.  Every signature
// must match exactly once.  If anything fails to resolve, the mod logs why and installs nothing,
// so the game plays as vanilla until the mod is updated.
//
// Ships as a KCSE plugin (Mods/<mod>/KCSE/Plugins/<name>.dll); also works as an .asi.
#pragma once
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include <vector>

namespace kc {

// ---------------- paths, log, config ----------------
inline char g_dir[MAX_PATH];        // folder holding this DLL (data files live here)
inline char g_name[64];
inline FILE* g_log = nullptr;
inline bool g_debug = false;        // [Options] DebugLog=1 in the .ini: verbose log for bug reports

inline void logf(const char* f, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, f); vfprintf(g_log, f, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}
inline void path(char* out, size_t cap, const char* file) { sprintf_s(out, cap, "%s\\%s", g_dir, file); }

inline void init(HMODULE self, const char* name) {
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    if (char* s = strrchr(g_dir, '\\')) *s = 0;
    strcpy_s(g_name, name);
    char p[MAX_PATH]; char f[96]; sprintf_s(f, "%s.log", name); path(p, sizeof p, f);
    g_log = _fsopen(p, "w", _SH_DENYNO);          // fresh log each session
    char ini[MAX_PATH]; sprintf_s(f, "%s.ini", name); path(ini, sizeof ini, f);
    g_debug = GetPrivateProfileIntA("Options", "DebugLog", 0, ini) != 0;
}

// Key from <name>.ini [Keys]: "F1".."F12", a single letter/digit, or a number (virtual-key code).
inline int ini_key(const char* key, int def) {
    char p[MAX_PATH]; char f[96]; sprintf_s(f, "%s.ini", g_name); path(p, sizeof p, f);
    char v[32] = {0}; GetPrivateProfileStringA("Keys", key, "", v, sizeof v, p);
    char* s = v; while (*s == ' ') s++;
    if (!*s) return def;
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') { int n = atoi(s + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    if (!s[1] && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) return s[0];
    if (!s[1] && s[0] >= 'a' && s[0] <= 'z') return s[0] - 32;
    int n = (int)strtol(s, nullptr, 0); return (n > 0 && n < 256) ? n : def;
}
inline int ini_int(const char* sec, const char* key, int def) {
    char p[MAX_PATH]; char f[96]; sprintf_s(f, "%s.ini", g_name); path(p, sizeof p, f);
    return (int)GetPrivateProfileIntA(sec, key, def, p);
}
inline const char* key_name(int vk, char* buf, size_t cap) {
    if (vk >= VK_F1 && vk <= VK_F24) sprintf_s(buf, cap, "F%d", vk - VK_F1 + 1);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) sprintf_s(buf, cap, "%c", vk);
    else sprintf_s(buf, cap, "VK 0x%02X", vk);
    return buf;
}

// Edge-triggered hotkey that ignores presses while another program has focus.
struct Hotkey {
    int vk = 0; bool down = false;
    bool pressed() {
        bool k = (GetAsyncKeyState(vk) & 0x8000) != 0;
        DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (pid != GetCurrentProcessId()) k = false;
        bool p = k && !down; down = k; return p;
    }
};

// ---------------- safe memory ----------------
inline bool readable(uintptr_t a, size_t n) {
    __try { volatile uint8_t x = 0; for (size_t i = 0; i < n; i += 64) x = *(volatile uint8_t*)(a + i); x = *(volatile uint8_t*)(a + n - 1); (void)x; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------- image + signatures ----------------
struct Image {
    uintptr_t base = 0, end = 0, text0 = 0, text1 = 0;
    uint32_t timestamp = 0;
    struct Range { uintptr_t lo, hi; };
    std::vector<Range> data;     // mapped non-code sections (RTTI, vtables)
    bool load(HMODULE h) {
        base = (uintptr_t)h;
        auto* nt = (PIMAGE_NT_HEADERS)(base + ((PIMAGE_DOS_HEADER)base)->e_lfanew);
        end = base + nt->OptionalHeader.SizeOfImage; timestamp = nt->FileHeader.TimeDateStamp;
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (!memcmp(sec[i].Name, ".text", 6)) { text0 = base + sec[i].VirtualAddress; text1 = text0 + sec[i].Misc.VirtualSize; }
            else if (!(sec[i].Characteristics & (IMAGE_SCN_MEM_DISCARDABLE | IMAGE_SCN_MEM_EXECUTE)))
                data.push_back({base + sec[i].VirtualAddress, base + sec[i].VirtualAddress + sec[i].Misc.VirtualSize});
        return text0 != 0;
    }
    bool in_text(uintptr_t a) const { return a >= text0 && a < text1; }
    bool in_image(uintptr_t a) const { return a >= base && a < end; }
};

struct Pattern {
    std::vector<int> b;   // -1 = wildcard
    size_t run0 = 0, runN = 0;   // longest literal run, used as the search needle
    explicit Pattern(const char* s) {
        while (*s) {
            while (*s == ' ') s++;
            if (!*s) break;
            if (s[0] == '?') { b.push_back(-1); s += (s[1] == '?') ? 2 : 1; }
            else { char h[3] = {s[0], s[1], 0}; b.push_back((int)strtoul(h, nullptr, 16)); s += 2; }
        }
        size_t cur = 0, len = 0;
        for (size_t i = 0; i <= b.size(); i++) {
            if (i < b.size() && b[i] >= 0) { if (!len) cur = i; len++; }
            else { if (len > runN) { runN = len; run0 = cur; } len = 0; }
        }
    }
    bool at(uintptr_t a) const {
        for (size_t i = 0; i < b.size(); i++) if (b[i] >= 0 && ((const uint8_t*)a)[i] != (uint8_t)b[i]) return false;
        return true;
    }
};

// All matches of pattern in [lo,hi).
inline int scan_all(uintptr_t lo, uintptr_t hi, const Pattern& p, uintptr_t* first, int stopAfter = 2) {
    if (p.runN == 0 || hi - lo < p.b.size()) return 0;
    uint8_t needle[256]; size_t n = p.runN < sizeof needle ? p.runN : sizeof needle;
    for (size_t i = 0; i < n; i++) needle[i] = (uint8_t)p.b[p.run0 + i];
    int count = 0;
    const uint8_t* cur = (const uint8_t*)lo + p.run0;
    const uint8_t* last = (const uint8_t*)hi - (p.b.size() - p.run0);
    while (cur <= last) {
        cur = (const uint8_t*)memchr(cur, needle[0], (size_t)(last - cur) + 1);
        if (!cur) break;
        if (!memcmp(cur, needle, n)) {
            uintptr_t start = (uintptr_t)cur - p.run0;
            if (p.at(start)) { if (!count && first) *first = start; if (++count >= stopAfter) return count; }
        }
        cur++;
    }
    return count;
}

inline uintptr_t rel32(uintptr_t field) { return field + 4 + *(const int32_t*)field; }

// Collects every lookup; any failure disables the mod.
struct Resolver {
    Image im; int fails = 0;
    void fail(const char* what, const char* why) { fails++; logf("  [FAIL] %s: %s", what, why); }
    // Unique signature scan; returns match + off.
    uintptr_t find(const char* what, const char* pat, int off = 0) {
        Pattern p(pat); uintptr_t m = 0;
        int n = scan_all(im.text0, im.text1, p, &m);
        if (n != 1) { fail(what, n ? "signature matches more than once" : "signature not found"); return 0; }
        return m + off;
    }
    // Target of the E8/E9 rel32 at `site` (must stay in .text).
    uintptr_t branch(const char* what, uintptr_t site) {
        if (!site) return 0;
        uint8_t op = *(uint8_t*)site;
        if (op != 0xE8 && op != 0xE9) { fail(what, "not a call"); return 0; }
        uintptr_t t = rel32(site + 1);
        if (!im.in_text(t)) { fail(what, "call target outside game code"); return 0; }
        return t;
    }
    // rip-relative operand: `field` points at the disp32, which must be the instruction's last 4 bytes.
    uintptr_t riprel(const char* what, uintptr_t field) {
        if (!field) return 0;
        uintptr_t t = rel32(field);
        if (!im.in_image(t)) { fail(what, "address outside game image"); return 0; }
        return t;
    }
    bool verify(const char* what, uintptr_t a, const char* pat) {
        if (!a) return false;
        Pattern p(pat);
        if (!im.in_text(a) || !readable(a, p.b.size()) || !p.at(a)) { fail(what, "code at address does not match (game updated?)"); return false; }
        return true;
    }
    // MSVC x64 RTTI: vtable of class `mangled` (".?AVName@ns@@") for the sub-object at `offset`.
    uintptr_t vtable(const char* what, const char* mangled, uint32_t offset) {
        size_t nl = strlen(mangled) + 1; uintptr_t td = 0;
        for (auto& r : im.data) {
            for (const char* c = (const char*)r.lo; !td && c + nl <= (const char*)r.hi; c++) {
                c = (const char*)memchr(c, '.', (const char*)r.hi - nl - c + 1); if (!c) break;
                if (!memcmp(c, mangled, nl)) td = (uintptr_t)c - 0x10;
            }
            if (td) break;
        }
        if (!td) { fail(what, "class not found (RTTI)"); return 0; }
        uint32_t tdr = (uint32_t)(td - im.base);
        for (auto& r : im.data)
            for (uintptr_t a = (r.lo + 3) & ~(uintptr_t)3; a + 0x18 <= r.hi; a += 4) {
                const uint32_t* col = (const uint32_t*)a;
                if (col[3] != tdr || col[0] != 1 || col[1] != offset || col[5] != (uint32_t)(a - im.base)) continue;
                for (auto& r2 : im.data)       // vtable[-1] == &COL
                    for (uintptr_t v = (r2.lo + 7) & ~(uintptr_t)7; v + 16 <= r2.hi; v += 8)
                        if (*(const uintptr_t*)v == a) return v + 8;
            }
        fail(what, "vtable not found (RTTI)");
        return 0;
    }
    uintptr_t slot(const char* what, uintptr_t vt, int i, const char* pat) {
        if (!vt) return 0;
        uintptr_t f = ((const uintptr_t*)vt)[i];
        return verify(what, f, pat) ? f : 0;
    }
    bool ok() const { return fails == 0; }
};

// True if the object's vtable belongs to `vt` (runtime type check).
inline bool is_a(uintptr_t obj, uintptr_t vt) {
    uintptr_t v = 0;
    __try { v = *(const uintptr_t*)obj; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return v == vt;
}

inline HMODULE wait_game() {
    HMODULE h = nullptr;
    for (int i = 0; i < 1200 && !h; i++) { h = GetModuleHandleA("WHGame.dll"); if (!h) Sleep(100); }
    return h;
}

// Pointer-sized vtable slot write (vtable pages are read-only).
inline void write_ptr(void** slot, void* v) {
    DWORD old; VirtualProtect(slot, 8, PAGE_READWRITE, &old); *slot = v; VirtualProtect(slot, 8, old, &old);
}

}  // namespace kc

#define KC_DLOG(...) do { if (kc::g_debug) kc::logf(__VA_ARGS__); } while (0)

// Start the mod's init thread (development builds skip it when loaded by the offline self-test).
#ifdef KC_DEVTOOLS
#define KC_START(fn) do { if (!GetEnvironmentVariableA("KC_SELFTEST", nullptr, 0)) CreateThread(nullptr, 0, fn, nullptr, 0, nullptr); } while (0)
#else
#define KC_START(fn) CreateThread(nullptr, 0, fn, nullptr, 0, nullptr)
#endif

// ---------------- KCSE plugin entry points ----------------
namespace KCSE {
struct PluginVersionData {
    uint32_t dataVersion, pluginVersion; char name[256], author[256];
    uint32_t compatibleGameVersions[16], kcseVersionRequired, versionIndependence;
};
}
#define KC_PLUGIN(pluginName, pluginAuthor, ver)                                                   \
    extern "C" __declspec(dllexport) KCSE::PluginVersionData KCSEPlugin_Version = {               \
        1, ver, pluginName, pluginAuthor, {0}, 0, 1 /* version independent: signatures */ };    \
    extern "C" __declspec(dllexport) bool KCSEPlugin_Load(const void*) { return true; }
