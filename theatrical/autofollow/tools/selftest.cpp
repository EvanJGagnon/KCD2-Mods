// selftest.cpp -- resolve every signature of each mod DLL against the WHGame.dll on disk and
// compare with the reference addresses of build 15693.  Usage: selftest <WHGame.dll> <mod.dll> <rva>...
#include <windows.h>
#include <cstdio>
#include <cstdlib>
typedef int (*SelfTest_t)(HMODULE, uintptr_t*);
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    SetEnvironmentVariableA("KC_SELFTEST", "1");
    HMODULE g = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!g) { printf("cannot map %s (%lu)\n", argv[1], GetLastError()); return 2; }
    HMODULE m = LoadLibraryA(argv[2]);
    if (!m) { printf("cannot load %s (%lu)\n", argv[2], GetLastError()); return 2; }
    auto st = (SelfTest_t)GetProcAddress(m, "KC_SelfTest");
    uintptr_t out[64] = {0};
    DWORD t0 = GetTickCount();
    int n = st(g, out);
    printf("%s: %s in %lu ms\n", argv[2], n > 0 ? "resolved" : "FAILED", GetTickCount() - t0);
    int bad = n > 0 ? 0 : 1;
    for (int i = 0; i < (n > 0 ? n : 0); i++) {
        uintptr_t rva = out[i] ? out[i] - (uintptr_t)g : 0;
        uintptr_t want = (3 + i < argc) ? strtoull(argv[3 + i], nullptr, 16) : 0;
        bool ok = !want || rva == want;
        if (!ok) bad++;
        printf("  [%d] 0x%llx %s\n", i, (unsigned long long)rva, ok ? (want ? "ok" : "") : "MISMATCH");
    }
    return bad;
}
