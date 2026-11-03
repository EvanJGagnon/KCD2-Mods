// hookaudit.cpp -- offline check of AutoForge's capture of the game's item dialog (the "Smithing successful" screen).
// Maps WHGame.dll WITHOUT running it, lets the dev DLL apply its two call-site patches to the mapped image, then checks
// the machine code the game would execute:
//   call site  : E8 rel32 -> trampoline (was -> C_UIModalDialogItem::Open)
//   trampoline : 48 89 0D disp32   mov [rip+disp], rcx      (rcx = the dialog object, Open's 'this')
//                FF 25 00 00 00 00 + 8-byte address         jmp [rip+0] -> the unmodified Open
//   slot       : the 8 bytes the mov writes, inside the trampoline page, 8-byte aligned
// Usage: hookaudit <WHGame.dll> <dev kcd2_autoforge.dll>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
typedef int (*Audit_t)(HMODULE, uintptr_t*);
static int bad = 0;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    SetEnvironmentVariableA("KC_SELFTEST", "1");
    HMODULE g = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!g) { printf("cannot map game dll (%lu)\n", GetLastError()); return 2; }
    HMODULE m = LoadLibraryA(argv[2]);
    if (!m) { printf("cannot load mod dll (%lu)\n", GetLastError()); return 2; }
    auto audit = (Audit_t)GetProcAddress(m, "KC_HookAudit");
    uintptr_t o[8] = {0};
    int n = audit(g, o);
    CHECK(n == 8, "patches were applied (%d)", n);
    if (n != 8) return 1;
    const uintptr_t base = (uintptr_t)g, open = o[6];
    printf("Open at RVA 0x%llx, close handler RVA 0x%llx\n", (unsigned long long)(open - base), (unsigned long long)(o[7] - base));
    for (int i = 0; i < 2; i++) {
        uintptr_t site = o[i * 3], tramp = o[i * 3 + 1], slot = o[i * 3 + 2];
        const uint8_t* s = (const uint8_t*)site; const uint8_t* t = (const uint8_t*)tramp;
        printf("call site RVA 0x%llx -> trampoline %p, slot %p\n", (unsigned long long)(site - base), (void*)tramp, (void*)slot);
        CHECK(s[0] == 0xE8, "call opcode kept");
        CHECK(tramp != open && (tramp < base || tramp >= base + 0x10000000), "call now goes to the trampoline, not Open");
        CHECK(t[0] == 0x48 && t[1] == 0x89 && t[2] == 0x0D, "trampoline starts with mov [rip+disp], rcx");
        uintptr_t slotFromCode = (uintptr_t)(t + 7) + *(const int32_t*)(t + 3);
        CHECK(slotFromCode == slot, "the mov stores into the slot (%p vs %p)", (void*)slotFromCode, (void*)slot);
        CHECK(slot % 8 == 0 && slot >= tramp + 0x20 && slot < tramp + 0x1000, "slot is aligned, inside the page, after the code");
        CHECK(t[7] == 0xFF && t[8] == 0x25 && *(const int32_t*)(t + 9) == 0, "then jmp [rip+0]");
        CHECK(*(const uintptr_t*)(t + 13) == open, "the jump lands on the unmodified Open (%p)", (void*)*(const uintptr_t*)(t + 13));
        // the first bytes of Open itself are untouched
        static const uint8_t openHead[] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57 };
        CHECK(!memcmp((const void*)open, openHead, sizeof openHead), "Open's own code is not modified");
        MEMORY_BASIC_INFORMATION mbi; VirtualQuery((void*)tramp, &mbi, sizeof mbi);
        CHECK((mbi.Protect & (PAGE_EXECUTE_READWRITE)) != 0, "trampoline page is executable and the slot writable (0x%lx)", mbi.Protect);
        CHECK(*(const volatile uintptr_t*)slot == 0, "slot starts empty");
    }
    printf(bad ? "%d FAILED\n" : "ALL PASSED\n", bad);
    return bad ? 1 : 0;
}
