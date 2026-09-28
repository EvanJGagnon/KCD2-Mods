// routetest.cpp -- run the mod's own planner offline: routetest <dll> <graph dir> <level> x y hx hy mx my [uturn]
#include <windows.h>
#include <cstdio>
#include <cstdlib>
typedef int (*RT)(const char*, const char*, float, float, float, float, float, float, float, float*);
int main(int c, char** v) {
    SetEnvironmentVariableA("KC_SELFTEST", "1");
    HMODULE m = LoadLibraryA(v[1]); RT f = (RT)GetProcAddress(m, "KC_RouteTest");
    float a[7]; for (int i = 0; i < 7; i++) a[i] = (float)atof(v[4 + i < c ? 4 + i : 0]);
    float u = c > 10 ? (float)atof(v[10]) : 400.f, len = 0;
    int r = f(v[2], v[3], a[0], a[1], a[2], a[3], a[4], a[5], u, &len);
    printf("uturn %4.0f: %s len %.0f m\n", u, r < 0 ? "FAIL" : (r ? "turn around," : "ahead,"), len);
    return r < 0;
}
