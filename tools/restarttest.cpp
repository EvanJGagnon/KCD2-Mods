// Exercises the REAL ForgeGame::restart(), not a duplicate simulation adapter.
#define KC_AUTHOR "Flubbermunchkin"
#include "../src/autoforge.cpp"
#include <cassert>
#include <cstdio>
static uintptr_t context[4], systemObj[2], systemVt[65], actorMem[16], freshMem[32], wrongMem[32];
static int lookups, starts, managers; static bool refuse, retireOnStart;
static uintptr_t returned, currentActor;
static uintptr_t contextFn(){return (uintptr_t)context;}
static uintptr_t actorFn(uintptr_t s){assert(s==(uintptr_t)systemObj); return currentActor;}
static uintptr_t managerFn(){++managers; return 0x1234;}
static uintptr_t lookupFn(uintptr_t m,uint32_t id,uint8_t type,bool create,bool inactive){
    assert(m==0x1234 && id==42 && type==12 && create && !inactive); ++lookups; return returned;
}
static bool startFn(uintptr_t bs,uint32_t forge){
    assert(bs==(uintptr_t)freshMem && forge==7217); ++starts;
    if(refuse) return false;
    if(retireOnStart){freshMem[0]=0; return true;}
    F<uint8_t>(bs,BS_ACTIVE)=1; F<uint32_t>(bs,BS_ENTITY)=forge; return true;
}
static void reset(){
    memset(context,0,sizeof context); memset(systemVt,0,sizeof systemVt);
    memset(actorMem,0,sizeof actorMem); memset(freshMem,0,sizeof freshMem);
    context[1]=(uintptr_t)systemObj; systemObj[0]=(uintptr_t)systemVt;
    systemVt[64]=(uintptr_t)actorFn; currentActor=(uintptr_t)actorMem;
    F<uint32_t>(currentActor,0x30)=42;
    g_bsVt=0xABCDEF; freshMem[0]=g_bsVt;
    F<uintptr_t>((uintptr_t)freshMem,BS_ACTOR)=currentActor;
    g_canRestart=true; g_queueActorId=42; g_forgeId=7217;
    pGameContext=contextFn; pForgeManager=managerFn; pComponentLookup=lookupFn; pStartForge=startFn;
    returned=(uintptr_t)freshMem; lookups=starts=managers=0; refuse=retireOnStart=false;
    g_modalSlotA=g_modalSlotB=nullptr; g_bs=(uintptr_t)wrongMem; g_m=0x1111;
}
int main(){
    kc::g_log=stdout;
    reset();
    void* inaccessible=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_NOACCESS); assert(inaccessible);
    g_bs=(uintptr_t)inaccessible;
    assert(g_game.restart()); assert(lookups==1 && starts==1 && g_bs==(uintptr_t)freshMem && g_m==0);
    assert(!g_game.restart()); assert(lookups==2 && starts==1);
    VirtualFree(inaccessible,0,MEM_RELEASE);
    puts("PASS: inaccessible retired pointer never read; native lookup ABI; duplicate start refused");
    reset(); returned=0; assert(!g_game.restart() && lookups==1 && starts==0);
    returned=(uintptr_t)freshMem; assert(g_game.restart() && lookups==2 && starts==1);
    puts("PASS: transient lookup failure is retryable through real lookup");
    reset(); freshMem[0]=1; assert(!g_game.restart() && starts==0);
    reset(); F<uintptr_t>((uintptr_t)freshMem,BS_ACTOR)=(uintptr_t)wrongMem; assert(!g_game.restart() && starts==0);
    reset(); currentActor=0; assert(!g_game.restart() && lookups==0);
    reset(); F<uint32_t>((uintptr_t)actorMem,0x30)=43; assert(!g_game.restart() && lookups==0);
    reset(); context[1]=0; assert(!g_game.restart() && lookups==0);
    reset(); g_forgeId=0; assert(!g_game.restart() && lookups==0);
    reset(); g_queueActorId=0; assert(!g_game.restart() && lookups==0);
    puts("PASS: invalid type/owner/current player/context/entity safely rejected");
    reset(); refuse=true; uintptr_t old=g_bs; assert(!g_game.restart() && starts==1 && g_bs==old);
    refuse=false; assert(g_game.restart() && lookups==2 && starts==2);
    reset(); retireOnStart=true; old=g_bs; assert(!g_game.restart() && g_bs==old);
    puts("PASS: start refusal preserves old binding; post-start lifetime validated");
    reset(); uintptr_t modal[24]={}; F<uint8_t>((uintptr_t)modal,0xA8)=1;
    volatile uintptr_t slot=(uintptr_t)modal; g_modalSlotA=&slot;
    assert(!g_game.restart() && lookups==0 && starts==0);
    g_modalSlotA=nullptr;
    puts("PASS: visible result dialog blocks reacquisition");
    // Retire/recreate after each result, through the production method each time.
    for(int i=0;i<6;i++){reset(); g_bs=(uintptr_t)wrongMem; assert(g_game.restart());}
    puts("ALL PRODUCTION RESTART TESTS PASSED (offline mocks, not native gameplay)");
}
