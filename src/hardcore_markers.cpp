// hardcore_markers.cpp -- Hardcore Map Markers for Kingdom Come: Deliverance II (kcd2_hardcore_markers.dll).
// Optional companion of Horse Route Follow.  Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3.
//
// Hardcore mode blocks custom map markers in two places, both a plain "game mode == 2 (hardcore)" check:
//   * when the map opens, the custom-marker input ("checkpoint" in action map apse_map_checkpoint) is
//     enabled only if the game mode is not hardcore (map Activate, 0x178F7C1);
//   * the map's marker refresh (SetCheckpoint/ResetCheckpoint on the map screen, 0xDCB1A0) returns early
//     in hardcore.
// This plugin removes those two checks, so custom markers can be placed and are shown on the map in
// hardcore.  Nothing else changes: the compass has its own code and still shows no marker, and the
// player's position stays hidden.  If either check is not found exactly, nothing is patched.
// Log: kcd2_hardcore_markers.log (next to the DLL).

#include "kcd2_common.h"

// The "je" after cmp eax,2 is at +0x12 (input) and +0x15 (map refresh).  The jump bytes are wildcarded so
// an already patched copy (NOPs) still matches.
static const char* SIG_HC_INPUT = "48 8B 0F 4C 8B 89 E8 00 00 00 40 84 ED 74 07 83 F8 02 ?? ?? B3 01 44 8A C3 48 8D 15";                      // 0x178F7C1
static const char* SIG_HC_DRAW  = "48 8B 01 FF 90 18 01 00 00 48 8B C8 48 8B 10 FF 52 10 83 F8 02 ?? ?? ?? ?? ?? ?? 48 8D 54 24 20 48 8B CB E8"; // 0xDCB1C6

using kc::logf;

// Replaces the conditional jump at `a` (expected bytes `jcc`, `n` long) with NOPs.
static void nop_out(uintptr_t a, const uint8_t* jcc, size_t n) {
    uint8_t nops[8]; memset(nops, 0x90, n);
    if (!memcmp((void*)a, nops, n) || memcmp((void*)a, jcc, n)) return;   // already done / not the expected code
    DWORD old; VirtualProtect((void*)a, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)a, nops, n);
    VirtualProtect((void*)a, n, old, &old); FlushInstructionCache(GetCurrentProcess(), (void*)a, n);
}

static HMODULE g_self = nullptr;
static DWORD WINAPI Init(LPVOID) {
    kc::init(g_self, "kcd2_hardcore_markers");
    HMODULE h = kc::wait_game(); if (!h) { logf("{\"ev\":\"abort\",\"msg\":\"WHGame.dll not loaded\"}"); return 0; }
    kc::Resolver R; R.im.load(h);
    logf("{\"ev\":\"start\",\"version\":\"1.1.0\",\"game_ts\":\"0x%08X\"}", R.im.timestamp);
    uintptr_t a = R.find("hardcore check: marker input", SIG_HC_INPUT, 0x12);
    uintptr_t b = R.find("hardcore check: marker on map", SIG_HC_DRAW, 0x15);
    static const uint8_t je8[] = {0x74, 0x02};
    uint8_t je32[6] = {0x0F, 0x84}; if (b) memcpy(je32 + 2, (void*)(b + 2), 4);
    // both or neither: the input without the map refresh would place markers nobody can see
    bool okA = a && (!memcmp((void*)a, je8, 2) || *(uint16_t*)a == 0x9090);
    bool okB = b && ((*(uint16_t*)b == 0x840F && *(int32_t*)(b + 2) > 0) || *(uint16_t*)b == 0x9090);
    if (!R.ok() || !okA || !okB) { logf("{\"ev\":\"disabled\",\"msg\":\"this game version is not supported yet - nothing was changed\"}"); return 0; }
    nop_out(b, je32, 6); nop_out(a, je8, 2);
    logf("{\"ev\":\"ready\",\"msg\":\"custom map markers enabled in hardcore mode\"}");
    return 0;
}

KC_PLUGIN("Hardcore Map Markers", KC_AUTHOR, 110)
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); g_self = h; KC_START(Init); }
    return TRUE;
}
