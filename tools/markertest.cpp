// markertest.cpp -- run Horse Route Follow's custom-marker regression tests (src/autotravel_tests.inc)
// inside its development build.  Usage: markertest <out\dev\kcd2_autotravel.dll>
#include <windows.h>
#include <cstdio>
#include <cstring>
typedef int (*MarkerTest_t)(const char*);
int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: markertest <dev kcd2_autotravel.dll>\n"); return 2; }
    SetEnvironmentVariableA("KC_SELFTEST", "1");
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { printf("cannot load %s (%lu)\n", argv[1], GetLastError()); return 2; }
    auto f = (MarkerTest_t)GetProcAddress(m, "KC_MarkerTest");
    if (!f) { printf("%s has no KC_MarkerTest (not a development build?)\n", argv[1]); return 2; }
    char dir[MAX_PATH]; DWORD n = GetTempPathA(MAX_PATH, dir);   // the test road is written here
    if (!n || n >= MAX_PATH) { printf("no temp folder\n"); return 2; }
    if (dir[n - 1] == '\\') dir[n - 1] = 0;
    fflush(stdout);
    int bad = f(dir);
    if (bad < 0) return 2;
    if (bad) printf("markertest: %d check(s) FAILED\n", bad);
    else printf("markertest: all checks passed\n");
    return bad != 0;
}
