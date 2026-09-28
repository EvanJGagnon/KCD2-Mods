// alchemy.cpp -- Theatrical Autobrew for Kingdom Come: Deliverance II (kcd2_alchemy.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// An adaptation of Autobrew by JerryYOJ.  The planner and state machine are ported from Autobrew's
// source (github.com/JerryYOJ/libKCD2, GPL-3.0):
// Projects/Autobrew/src/core/Executor.cpp BuildPlan + tasks/Tick/Tick.cpp.  Differences:
//   * natives are found by byte signature / RTTI at startup (kcd2_common.h), no address library;
//   * recipe conditions come from alch_recipes.txt (generated offline from AlchemyRecipe.xml);
//   * boiling is theatrical: every recipe turn = one native TurnSandglass verb, then the pot
//     stays on the fire until the game's own sand level (C_Alchemy+0x338) has run from full to 0;
//   * no deliberate-mistake engine, no looping.
// Use: open a recipe page in the alchemy book, press F8 (kcd2_alchemy.ini).  F8 again stops.
// Log: kcd2_alchemy.log next to the DLL.

#include "kcd2_common.h"
#include <string>
#include <algorithm>

// ---------------- natives (signatures; build 15693 addresses in comments) ----------------
// Struct offsets inside the patterns stay literal (e.g. mode +0x300, state +0x2A0), so a layout
// change in a game update makes the lookup fail and the mod stays off.
static const char* SIG_PERFORMVERB = "48 89 5C 24 20 89 54 24 10 57 48 83 EC 20 80 B9 00 03 00 00 02";                       // 0x15FC494
static const char* SIG_CANPERFORM  = "48 89 5C 24 08 57 48 83 EC 20 83 FA 11";                                             // 0x8D1F90
static const char* SIG_GETSTATE    = "8B 81 A0 02 00 00 83 F8 15 75 ?? 8B 81 A4 02 00 00";                                  // 0x8D237C
static const char* SIG_SLOTBUSY    = "85 D2 78 ?? 4C 8B 41 38 48 8B 41 40";                                                 // 0x8D2450
static const char* SIG_APPLYING    = "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 48 8B D3 48 8B CF 84 C0"; // 0xA955DC
static const char* SIG_CLEARSLOTS  = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 30 33 FF 4C 8D B1 08 01 00 00"; // 0xA95760
static const char* SIG_FINDITEM    = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 59 08 48 8B EA 48 8B 79 10 EB ??"; // 0x8D315C
static const char* SIG_FINDCLASS   = "48 89 5C 24 08 55 56 57 41 56 41 57 48 83 EC 40 48 8B EA";                           // 0x468340
static const char* SIG_PARSEGUID   = "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 70 4C 8B F2"; // 0x719B1C
static const char* SIG_ITEMDB_REF  = "48 8D 0D ?? ?? ?? ?? F3 0F 7F 44 24 50 E8 ?? ?? ?? ?? 48 8B F0";                      // lea rcx,[0x5325820]; call FindClassByGuid
static const char* SIG_FRAMEWORK_REF = "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 84 C0";                                      // mov rcx,[0x549D328]
static const char* SIG_UPDATE      = "48 89 5C 24 08 57 48 83 EC 30 40 8A B9 00 03 00 00 48 8B D9 0F 29 74 24 20";          // C_Alchemy vtbl[19] 0x737920
static const char* SIG_RESET       = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 8B 49 18 48 8B 01";          // C_Alchemy vtbl[21] 0x6C4F1C
static const char* SIG_GETITEMSYS  = "48 8B 81 20 05 00 00 48 8D 48 08 48 F7 D8 48 1B C0 48 23 C1 C3";                    // CCryAction vtbl[0xD0/8]
static const char* SIG_GETITEM     = "89 54 24 10 53 48 83 EC 20 48 8D 59 70 48 8B CB 48 8D 54 24 38 E8 ?? ?? ?? ?? 48 3B 03 75 ?? 33 C0"; // CItemSystem(IItemSystem) vtbl[0xA8/8]
struct GUID16 { uint64_t hi, lo; bool operator==(const GUID16& o) const { return hi==o.hi && lo==o.lo; } };
static const GUID16 NULLGUID{0,0};
typedef void  (*PerformVerb_t)(void*, int);
typedef bool  (*CanPerformVerb_t)(void*, int);
typedef int   (*GetState_t)(void*);
typedef bool  (*IsSlotOccupied_t)(void*, int);
typedef void  (*ApplyIngredient_t)(void*, uint64_t);
typedef void  (*ClearSlots_t)(void*);
typedef void* (*FindItemByClass_t)(void*, const GUID16*);
typedef void* (*FindClassByGuid_t)(void*, const GUID16*);
typedef bool  (*ParseGuid_t)(const char*, GUID16*);
typedef void  (*Update_t)(void*, float);

static uintptr_t B = 0;
static PerformVerb_t     pPerformVerb;
static CanPerformVerb_t  pCanPerformVerb;
static GetState_t        pGetState;
static IsSlotOccupied_t  pIsSlotOccupied;
static ApplyIngredient_t pApplyIngredient;
static ClearSlots_t      pClearSlots;
static FindItemByClass_t pFindItemByClass;
static FindClassByGuid_t pFindClassByGuid;
static ParseGuid_t       pParseGuid;
static Update_t          pOrigUpdate;
static void*             pItemDb;          // item class database (FindClassByGuid's `this`)
static void**            pFramework;       // global CCryAction*
static uintptr_t         vtCryAction, vtItemSystem;   // RTTI vtables for runtime type checks
typedef void (*Reset_t)(void*);
static Reset_t           pReset;

enum Verb { TakePot=5, UsePot=6, UseMortar=2, Distill=4, TurnSandglass=1, UseBellows=18, MovePotBack=20, TakeHerb1=7, TakeSpecial1=10, OpenBook=19, VNone=21, PourSpiritus=13 };
enum { MODE_BREWING=2, MODE_READING=4, STATE_IDLE=21, STATE_HOLDPOT=5, SLOT_VERB=1 };

// C_Alchemy field offsets
static const uintptr_t O_ACTOR=0x18, O_CONTEXT=0x30, O_DIRECTOR=0x78, O_HERBKEY=0x120, O_KINDENT=0x150,
    O_SPECKEY=0x2D0, O_MODE=0x300, O_FIRE=0x320, O_SAND=0x338, O_POTONFIRE=0x428, O_POTMOVING=0x429, O_BOILSTATE=0x440,
    O_BUCKETS=0x470, O_PENDING=0x688;

static const char* volatile g_step="";
using kc::logf;
template<class T> static T& F(void* p, uintptr_t o){ return *(T*)((char*)p+o); }

// ---------------- recipes (alch_recipes.txt) ----------------
struct Cond { char kind; GUID16 g; int n; };
struct Recipe { uint32_t id=0; int base=0; std::vector<std::pair<GUID16,int>> ing; std::vector<Cond> conds; bool distill=false, resultMilled=false, bad=false; };
static std::vector<Recipe> g_recipes;
static void load_recipes(){
    char p[MAX_PATH]; kc::path(p,sizeof p,"alch_recipes.txt");
    FILE* f=nullptr; fopen_s(&f,p,"r"); if(!f){ logf("no recipe file"); return; }
    char line[256]; Recipe cur;
    while(fgets(line,sizeof line,f)){
        char k=line[0]; char gs[64]={0}; int n=0; unsigned id=0;
        if(k=='R'){ cur=Recipe(); sscanf_s(line+2,"%u %d",&id,&cur.base); cur.id=id; }
        else if(k=='I'||k=='M'||k=='W'||k=='S'){ sscanf_s(line+2,"%63s %d",gs,(unsigned)sizeof gs,&n); GUID16 g{}; if(!pParseGuid(gs,&g)) cur.bad=true;
            if(k=='I') cur.ing.push_back({g,n?n:1}); else cur.conds.push_back({k,g,n}); }
        else if(k=='D'){ cur.distill = atoi(line+2)==1; }
        else if(k=='P'){ cur.resultMilled = atoi(line+2)==1; }
        else if(k=='X'){ cur.bad=true; }
        else if(k=='E'){ g_recipes.push_back(cur); }
    }
    fclose(f); logf("recipes loaded %zu",g_recipes.size());
}
static Recipe* find_recipe(uint32_t id){ for(auto& r:g_recipes) if(r.id==id) return &r; return nullptr; }

// ---------------- game queries ----------------
static void* inventory_of(void* alc){
    void* actor=F<void*>(alc,O_ACTOR); if(!actor) return nullptr;
    void* soul=F<void*>(actor,0x668); if(!soul) return nullptr;
    void* invSoul=(char*)soul+0x198;
    typedef void* (*GetInv_t)(void*); return ((GetInv_t)(*(void***)invSoul)[0])(invSoul);
}
static GUID16 dried_variant(const GUID16& g){
    void* db=pItemDb;
    g_step="FindClassByGuid"; void* cls=pFindClassByGuid(db,&g); if(!cls) return NULLGUID;
    g_step="IsType/AsHerb";
    typedef bool (*IsType_t)(void*,int); typedef void* (*AsHerb_t)(void*);
    void** vt=*(void***)cls;
    if(!((IsType_t)vt[3])(cls,10)) return NULLGUID;               // E_ItemType::Herb
    void* herb=((AsHerb_t)vt[39])(cls); if(!herb) return NULLGUID;
    return F<GUID16>(herb,0x100);                                  // DriedItemId
}
static int item_amount(void* item){ return item? F<int32_t>(item,0x50):0; }
static int available(void* inv,const GUID16& g){
    int n=item_amount(pFindItemByClass(inv,&g));
    GUID16 d=dried_variant(g); if(!(d==NULLGUID)) n+=item_amount(pFindItemByClass(inv,&d));
    return n;
}
static uint32_t open_recipe_id(void* alc){
    g_step="book entity"; int32_t ent=F<int32_t>(alc,O_KINDENT+4*OpenBook); if(!ent) return 0;
    g_step="cryaction"; void* fw=*pFramework; if(!fw) return 0;
    if(!kc::is_a((uintptr_t)fw,vtCryAction)){ logf("framework object has an unexpected type"); return 0; }
    typedef void* (*GetIS_t)(void*);
    g_step="GetIItemSystem"; void* is=((GetIS_t)(*(void***)fw)[0xD0/8])(fw); if(!is) return 0;   // CCryAction::GetIItemSystem (as Autobrew calls it)
    if(!kc::is_a((uintptr_t)is,vtItemSystem)){ logf("item system has an unexpected type"); return 0; }
    typedef void* (*GetItem_t)(void*,uint32_t);
    g_step="GetItem"; void* book=((GetItem_t)(*(void***)is)[0xA8/8])(is,(uint32_t)ent); if(!book) return 0;   // IItemSystem::GetItem(EntityId)
    g_step="book fields";   
    if(F<uint8_t>(book,0x170)!=2 || F<uint8_t>(book,0x12D)) return 0;   // AlchemyBook, numeric pages
    return F<uint32_t>(book,0x130);
}
static const char* context(void* alc){ const char* s=F<const char*>(alc,O_CONTEXT); return s?s:""; }
static void* pot_base_record(void* alc){
    auto* v=(std::vector<void*>*)((char*)alc+O_BUCKETS+6*sizeof(std::vector<void*>));
    for(void* r:*v) if(r && F<uint8_t>(r,0x20)) return r;
    return nullptr;
}
static void log_pot(void* alc,const char* tag){
    auto* v=(std::vector<void*>*)((char*)alc+O_BUCKETS+6*sizeof(std::vector<void*>));
    char buf[768]; int n=sprintf_s(buf,"%s: pot records %zu:",tag,v->size());
    for(void* r:*v){ if(!r) continue;
        n+=sprintf_s(buf+n,sizeof buf-n," [%s weak %.1fs strong %.1fs q %.2f flags %u]",
            F<uint8_t>(r,0x20)?"BASE":"herb",F<float>(r,0x14),F<float>(r,0x18),F<float>(r,0x24),F<uint32_t>(r,0x10));
        if(n>700) break; }
    KC_DLOG("%s",buf);
}
static bool workspace_dirty(void* alc){
    auto* v=(std::vector<void*>*)((char*)alc+O_BUCKETS);
    for(int i=0;i<21;i++) if(!v[i].empty()) return true;
    return false;
}
static int ingredient_verb(void* alc,const GUID16& g){
    GUID16 d=dried_variant(g); bool hd=!(d==NULLGUID);
    for(int i=0;i<3;i++){ GUID16 k=F<GUID16>(alc,O_HERBKEY+16*i); if(k==g||(hd&&k==d)) return TakeHerb1+i; }
    for(int i=0;i<3;i++){ GUID16 k=F<GUID16>(alc,O_SPECKEY+16*i); if(k==g) return TakeSpecial1+i; }
    return VNone;
}

// ---------------- plan ----------------
struct Op { enum K { DoVerb, TakeIngredient, EnsureHoldPot, SetPotOnFire, TakePotOffFire, SandTurn } k; int verb; GUID16 g; bool strong; int strongAfter; };
struct IngPlan { GUID16 g; int qty; bool milled, mustNotMill; int w, s; };

static bool build_plan(Recipe* r,std::vector<Op>& plan){
    std::vector<IngPlan> ing;
    for(auto& i:r->ing) ing.push_back({i.first,i.second,false,false,-1,-1});
    auto find=[&](const GUID16& g)->IngPlan*{ for(auto& e:ing) if(e.g==g) return &e; return nullptr; };
    for(auto& c:r->conds){
        IngPlan* e=find(c.g); if(!e){ logf("condition names unknown ingredient"); return false; }
        if(c.kind=='M'){ if(c.n==1) e->milled=true; else e->mustNotMill=true; }
        else if(c.kind=='W') e->w=std::max(e->w,c.n);
        else if(c.kind=='S') e->s=std::max(e->s,c.n);
    }
    std::stable_sort(ing.begin(),ing.end(),[](const IngPlan&a,const IngPlan&b){ return a.w!=b.w? a.w>b.w : a.s>b.s; });
    { int nw=0,ns=0; for(size_t i=ing.size();i-->0;){
        if(ing[i].w>=0){ if(ing[i].w<nw) return false; nw=ing[i].w; }
        if(ing[i].s>=0){ if(ing[i].s<ns) return false; ns=ing[i].s; } } }
    std::vector<int> effW(ing.size()), effS(ing.size());
    { int w=0,s=0; for(size_t i=ing.size();i-->0;){ if(ing[i].w>=0) w=ing[i].w; if(ing[i].s>=0) s=ing[i].s; effW[i]=w; effS[i]=s; } }

    plan.clear();
    plan.push_back({Op::DoVerb,PourSpiritus+r->base});
    for(size_t i=0;i<ing.size();i++){
        auto& e=ing[i];
        for(int u=0;u<e.qty;u++){
            plan.push_back({Op::TakeIngredient,VNone,e.g});
            plan.push_back({Op::DoVerb, e.milled? UseMortar:UsePot});
        }
        if(e.milled){ plan.push_back({Op::DoVerb,UseMortar}); plan.push_back({Op::DoVerb,UsePot}); }
        int wT=effW[i]-(i+1<ing.size()?effW[i+1]:0), sT=effS[i]-(i+1<ing.size()?effS[i+1]:0);
        if(wT>0||sT>0){
            plan.push_back({Op::SetPotOnFire});
            KC_DLOG("stage %zu: weak %d turn(s), strong %d turn(s)",i,wT,sT);
            for(int t=0;t<wT;t++) plan.push_back({Op::SandTurn,VNone,{},false,0});
            for(int t=0;t<sT;t++) plan.push_back({Op::SandTurn,VNone,{},true,sT-1-t});
            plan.push_back({Op::TakePotOffFire});
        }
    }
    if(r->distill && r->resultMilled) return false;
    if(r->resultMilled) plan.push_back({Op::DoVerb,UseMortar});
    else { plan.push_back({Op::EnsureHoldPot}); plan.push_back({Op::DoVerb, r->distill? Distill:UsePot}); }
    return true;
}

// ---------------- input ----------------
static void tap(WORD scan){
    INPUT in[2]={}; in[0].type=in[1].type=INPUT_KEYBOARD;
    in[0].ki.wScan=scan; in[0].ki.dwFlags=KEYEVENTF_SCANCODE;
    in[1].ki.wScan=scan; in[1].ki.dwFlags=KEYEVENTF_SCANCODE|KEYEVENTF_KEYUP;
    SendInput(2,in,sizeof(INPUT));
}
static const WORD SC_R=0x13;

// ---------------- state machine (runs inside C_Alchemy::Update, main thread) ----------------
enum Phase { Idle, Closing, Arming, Cooking };
static Phase    g_phase=Idle;
static uint32_t g_recipeId=0;
static std::vector<Op> g_plan; static size_t g_cur=0; static int g_sub=0;
static uint64_t g_t=0, g_stall=0;
static kc::Hotkey g_key;
static int      g_insane=0; static bool g_dead=false;
static const uint64_t STALL_ABORT_MS=12000;

static void to_idle(const char* why){ if(why) logf("stop: %s (recipe %u, op %zu/%zu)",why,g_recipeId,g_cur,g_plan.size()); g_phase=Idle; g_plan.clear(); g_cur=0; g_sub=0; g_recipeId=0; }

static void tick(void* alc){
    uint64_t now=GetTickCount64();
    bool pressed=g_key.pressed();
    int mode=F<uint8_t>(alc,O_MODE);
    {   // sanity: values the mod relies on must look like the build it was made for
        float sand=F<float>(alc,O_SAND); int stt=pGetState(alc);
        if(mode>24 || !(sand>=-0.01f && sand<=1.5f) || stt<0 || stt>64 || F<uint8_t>(alc,O_BOILSTATE)>4){
            if(g_phase!=Idle) to_idle("unexpected table state");
            if(++g_insane>=30){ g_dead=true; logf("disabled: alchemy table data does not look right (game updated?)"); }
            return;
        }
        g_insane=0;
    }
    bool verbBusy=pIsSlotOccupied((char*)alc+O_DIRECTOR,SLOT_VERB);

    if(pressed && g_phase!=Idle){
        if(F<int>(alc,O_PENDING)==UseBellows) F<int>(alc,O_PENDING)=VNone;
        to_idle("user stop"); return;
    }
    switch(g_phase){
    case Idle: {
        if(!pressed) break;
        bool reading = mode==MODE_READING, atBench = mode==MODE_BREWING;
        if(!reading && !atBench){ logf("F8 ignored: table busy (ctx %s mode %d)",context(alc),mode); break; }
        uint32_t id=open_recipe_id(alc); g_step="find recipe"; Recipe* r=id?find_recipe(id):nullptr;
        if(!r||r->bad){ logf("recipe %u not brewable (no recipe page open in the book?)",id); break; }
        g_step="inventory"; void* inv=inventory_of(alc);
        g_step="availability"; for(auto& i:r->ing) if(!inv || available(inv,i.first)<i.second){ logf("recipe %u: missing ingredients",id); r=nullptr; break; }
        if(!r) break;
        g_recipeId=id; g_phase=Closing; g_t=now;
        if(reading) tap(SC_R);                                       // close the book (read_back); at the bench R would reset the table
        logf("start recipe %u",id);
        break;
    }
    case Closing:
        if(!strcmp(context(alc),"alchemy") && mode==MODE_BREWING && pGetState(alc)==STATE_IDLE && !verbBusy){
            if(workspace_dirty(alc)){ pReset(alc); logf("workspace reset"); }
            g_phase=Arming;
        } else if(now-g_t>15000) to_idle("book did not close");
        break;
    case Arming: {
        Recipe* r=find_recipe(g_recipeId); void* inv=inventory_of(alc);
        if(!r||!inv){ to_idle("lost recipe/inventory"); break; }
        std::vector<uint64_t> stock;
        for(auto& i:r->ing){
            void* it=pFindItemByClass(inv,&i.first);
            if(!it){ GUID16 d=dried_variant(i.first); if(!(d==NULLGUID)) it=pFindItemByClass(inv,&d); }
            if(!it){ to_idle("ingredient vanished"); return; }
            stock.push_back(F<uint64_t>(it,0x30));
        }
        pClearSlots(alc);
        for(uint64_t w:stock) pApplyIngredient(alc,w);
        if(!build_plan(r,g_plan)){ to_idle("recipe not plannable"); break; }
        logf("plan %zu ops",g_plan.size());
        g_phase=Cooking; g_cur=0; g_sub=0; g_stall=now;
        break;
    }
    case Cooking: {
        if(g_cur>=g_plan.size()){ logf("brew finished (recipe %u)",g_recipeId); to_idle(nullptr); break; }
        Op& op=g_plan[g_cur];
        bool sand=op.k==Op::SandTurn && g_sub>=1;
        if(verbBusy && !sand){ g_stall=now; break; }
        if(mode!=MODE_BREWING && !sand){ if(now-g_stall>STALL_ABORT_MS) to_idle("brewing interrupted"); break; }
        bool adv=false, prog=false;
        switch(op.k){
        case Op::DoVerb:
            if(pCanPerformVerb(alc,op.verb)){ pPerformVerb(alc,op.verb); adv=true; }
            break;
        case Op::TakeIngredient: {
            int v=ingredient_verb(alc,op.g);
            if(v!=VNone && pCanPerformVerb(alc,v)){ pPerformVerb(alc,v); adv=true; }
            break; }
        case Op::EnsureHoldPot: {
            if(g_sub==0){ log_pot(alc,"before finish"); g_sub=1; }
            int st=pGetState(alc);
            if(st==STATE_HOLDPOT) adv=true;
            else if(st==STATE_IDLE && pCanPerformVerb(alc,TakePot)){ pPerformVerb(alc,TakePot); adv=true; }
            break; }
        case Op::SetPotOnFire: case Op::TakePotOffFire: {
            bool want=op.k==Op::SetPotOnFire;
            if(F<uint8_t>(alc,O_POTMOVING)) { prog=true; break; }
            if((bool)F<uint8_t>(alc,O_POTONFIRE)==want){ if(!want) log_pot(alc,"stage done"); adv=true; break; }
            if(pCanPerformVerb(alc,MovePotBack)){ pPerformVerb(alc,MovePotBack); prog=true; }
            break; }
        case Op::SandTurn: {
            float s=F<float>(alc,O_SAND);
            if(g_sub==0){                                   // wait for the boil ("when you hear it bubbling"), then flip
                int boil=F<uint8_t>(alc,O_BOILSTATE);       // 0 heating, 1 weak boil, 2 strong boil
                int want=op.strong?2:1;
                if(!F<uint8_t>(alc,O_POTONFIRE) && !F<uint8_t>(alc,O_POTMOVING)){ to_idle("pot left the fire"); return; }
                if(op.strong && boil<2){ F<int>(alc,O_PENDING)=UseBellows; prog=true; break; }   // work the bellows up to a strong boil
                if(boil!=want){ prog=true; break; }        // still heating
                if(s<=0.0001f && pCanPerformVerb(alc,TurnSandglass)){ pPerformVerb(alc,TurnSandglass); g_sub=1; g_t=now; KC_DLOG("sandglass turn (%s) at boil %d",op.strong?"strong":"weak",boil); }
                else prog=true;
                break;
            }
            if(g_sub==1){                                   // wait for the sand to start running
                if(s>0.05f){ g_sub=2; prog=true; }
                else if(now-g_t>8000){ to_idle("sandglass did not turn"); return; }
                else prog=true;
                break;
            }
            // g_sub==2: sand running; the pot must stay on the fire
            if(!F<uint8_t>(alc,O_POTONFIRE) && !F<uint8_t>(alc,O_POTMOVING)){ to_idle("pot left the fire"); return; }
            if(op.strong){                                  // keep the heat up with the bellows (Autobrew's rule)
                float remaining=s*10.0f + 10.0f*op.strongAfter;
                float charge=(F<float>(alc,O_FIRE)-1.0f)*6.0f;
                if(remaining>charge) F<int>(alc,O_PENDING)=UseBellows;
                else if(F<int>(alc,O_PENDING)==UseBellows) F<int>(alc,O_PENDING)=VNone;
            }
            if(s<=0.0001f){                                 // sand ran out: turn complete
                if(F<int>(alc,O_PENDING)==UseBellows && op.strongAfter==0) F<int>(alc,O_PENDING)=VNone;
                adv=true;
            } else prog=true;
            break; }
        }
        if(adv){ g_cur++; g_sub=0; g_stall=now; }
        else if(prog) g_stall=now;
        else if(now-g_stall>STALL_ABORT_MS){
            logf("stalled: op %zu kind %d verb %d state %d mode %d busy %d potFire %d",g_cur,(int)op.k,op.verb,pGetState(alc),mode,(int)verbBusy,(int)F<uint8_t>(alc,O_POTONFIRE));
            to_idle("stalled");
        }
        break;
    }
    }
}

static void UpdateHook(void* alc,float dt){
    pOrigUpdate(alc,dt);
    if(g_dead) return;
    __try { tick(alc); } __except(EXCEPTION_EXECUTE_HANDLER){ logf("exception in tick at step: %s",g_step); to_idle("exception"); }
}

// ---------------- install ----------------
struct Found { uintptr_t perform, can, state, busy, apply, clear, finditem, findclass, parse, itemdb, fwglob, vt, update, reset, vtca, vtis; };
static bool resolve(kc::Resolver& R, Found& f){
    f.perform=R.find("PerformVerb",SIG_PERFORMVERB); f.can=R.find("CanPerformVerb",SIG_CANPERFORM);
    f.state=R.find("GetState",SIG_GETSTATE); f.busy=R.find("Director::IsSlotOccupied",SIG_SLOTBUSY);
    f.apply=R.find("ApplyIngredient",SIG_APPLYING); f.clear=R.find("ClearIngredientSlots",SIG_CLEARSLOTS);
    f.finditem=R.find("Inventory::FindItemByClass",SIG_FINDITEM); f.findclass=R.find("FindClassByGuid",SIG_FINDCLASS);
    f.parse=R.find("ParseGuid",SIG_PARSEGUID);
    uintptr_t dbref=R.find("item database",SIG_ITEMDB_REF);
    if(dbref){ f.itemdb=R.riprel("item database",dbref+3); if(R.branch("item database call",dbref+0xD)!=f.findclass) R.fail("item database","reference does not call FindClassByGuid"); }
    uintptr_t fwref=R.find("game framework",SIG_FRAMEWORK_REF); if(fwref) f.fwglob=R.riprel("game framework",fwref+3);
    f.vt=R.vtable("C_Alchemy",".?AVC_Alchemy@playermodule@wh@@",0);
    f.update=R.slot("C_Alchemy::Update",f.vt,19,SIG_UPDATE); f.reset=R.slot("C_Alchemy::Reset",f.vt,21,SIG_RESET);
    f.vtca=R.vtable("CCryAction",".?AVCCryAction@@",0); R.slot("CCryAction::GetIItemSystem",f.vtca,0xD0/8,SIG_GETITEMSYS);
    f.vtis=R.vtable("CItemSystem",".?AVCItemSystem@@",8); R.slot("CItemSystem::GetItem",f.vtis,0xA8/8,SIG_GETITEM);
    return R.ok();
}
#ifdef KC_DEVTOOLS
extern "C" __declspec(dllexport) int KC_SelfTest(HMODULE game, uintptr_t* out){   // offline check (tools/selftest)
    kc::Resolver R; R.im.load(game); Found f{}; bool ok=resolve(R,f);
    uintptr_t* p=(uintptr_t*)&f; for(size_t i=0;i<sizeof f/8;i++) out[i]=p[i];
    return ok? (int)(sizeof f/8) : -R.fails;
}
#endif

static HMODULE g_self=nullptr;
static DWORD WINAPI Init(LPVOID){
    kc::init(g_self,"kcd2_alchemy");
    g_key.vk=kc::ini_key("Brew",VK_F8);
    HMODULE h=kc::wait_game(); if(!h){ logf("abort: WHGame.dll not loaded"); return 0; }
    kc::Resolver R; R.im.load(h); B=R.im.base;
    logf("start v1.0.0 (game ts 0x%08X)",R.im.timestamp);
    Found f{};
    if(!resolve(R,f)){ logf("disabled: this game version is not supported yet - nothing was changed"); return 0; }
    pPerformVerb=(PerformVerb_t)f.perform; pCanPerformVerb=(CanPerformVerb_t)f.can;
    pGetState=(GetState_t)f.state; pIsSlotOccupied=(IsSlotOccupied_t)f.busy;
    pApplyIngredient=(ApplyIngredient_t)f.apply; pClearSlots=(ClearSlots_t)f.clear;
    pFindItemByClass=(FindItemByClass_t)f.finditem; pFindClassByGuid=(FindClassByGuid_t)f.findclass;
    pParseGuid=(ParseGuid_t)f.parse; pItemDb=(void*)f.itemdb; pFramework=(void**)f.fwglob;
    vtCryAction=f.vtca; vtItemSystem=f.vtis; pReset=(Reset_t)f.reset;
    load_recipes();
    if(g_recipes.empty()){ logf("abort: alch_recipes.txt missing next to the DLL"); return 0; }
    pOrigUpdate=(Update_t)f.update;
    kc::write_ptr(&((void**)f.vt)[19],(void*)&UpdateHook);
    char kn[16]; logf("ready (%s on a recipe page)",kc::key_name(g_key.vk,kn,sizeof kn));
    return 0;
}
KC_PLUGIN("Theatrical Autobrew", KC_AUTHOR, 100)
BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){ if(r==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); g_self=h; KC_START(Init); } return TRUE; }
