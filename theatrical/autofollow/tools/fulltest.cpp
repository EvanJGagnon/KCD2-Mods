// fulltest.cpp -- run the mod's real startup against a WHGame.dll mapped without its dependencies (no game
// running): resolve, patch, set the cvar, toggle off and on, and print the bytes the game would execute.
// The printed addresses are those of build 15693.
// Usage: fulltest <WHGame.dll> <dev kcd2_autofollow.dll>      (works under Wine too)
#include <windows.h>
#include <cstdio>
#include <cstdint>
static void show(const char* when, uint8_t* g){
    uint8_t* p = g + 0x129db22; int32_t* v = (int32_t*)(g + 0x55dc2a0);
    printf("%-12s patch site: %02X %02X %02X %02X   wh_player_FollowAlwaysCanFollow=%d\n", when, p[0], p[1], p[2], p[3], *v);
}
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    HMODULE g = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!g) { printf("cannot map %s (%lu)\n", argv[1], GetLastError()); return 2; }
    show("mapped", (uint8_t*)g);
    HMODULE m = LoadLibraryA(argv[2]);
    if (!m) { printf("cannot load %s (%lu)\n", argv[2], GetLastError()); return 2; }
    Sleep(1500);
    show("after start", (uint8_t*)g);
    auto tog = (void (*)())GetProcAddress(m, "KC_DevToggle");
    tog(); Sleep(600); show("toggled off", (uint8_t*)g);
    tog(); Sleep(600); show("toggled on", (uint8_t*)g);
    *(int32_t*)((uint8_t*)g + 0x55dc2a0) = 0; Sleep(600); show("game reset", (uint8_t*)g);   // cvar registration
    return 0;
}
