// alchemy.cpp -- Theatrical Autobrew for Kingdom Come: Deliverance II (kcd2_alchemy.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// An adaptation of Autobrew by JerryYOJ.  The planner and state machine are ported from Autobrew's
// source (github.com/JerryYOJ/libKCD2, GPL-3.0):
// Projects/Autobrew/src/core/Executor.cpp BuildPlan + tasks/Tick/Tick.cpp.  Differences:
//   * natives are found by byte signature / RTTI at startup (kcd2_common.h), no address library;
//   * recipe conditions come from alch_recipes.txt (generated offline from AlchemyRecipe.xml);
//   * boiling is theatrical: every recipe turn = one native TurnSandglass verb (on a strong turn
//     before the bellows), but the turn ends on the game's own boil timer (the pot's base record),
//     as in Autobrew.  Waiting for the sand (C_Alchemy+0x338) to run out instead overshot by
//     2-4 s (turn and lift animations) and failed exact-time steps such as Buck's Blood's dandelion;
//   * no deliberate-mistake engine; looping only through the optional 2.0 queue ([Options] Queue=1, see below).
// Use: open a recipe page in the alchemy book, press F8 (kcd2_alchemy.ini).  F8 again stops.
// Log: kcd2_alchemy.log next to the DLL.

#include "kcd2_common.h"
#include <string>
#include <algorithm>
#include "brew_queue.h"

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
    O_BUCKETS=0x470, O_PENDING=0x688, O_QCOEF=0x6D0;

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

// ---- herb stacks: the game's FindItemByClass returns the FIRST stack of a class, whatever its health ----
// C_InventoryBase::m_items (+0x08) is a std::vector<C_Item*>; C_Item: class data +0x48, amount +0x50,
// health +0x54, WUID +0x30.  Stacks of one class share the class-data pointer, so FindItemByClass
// gives the class to match without knowing where the class GUID lives.  Anything that does not look
// right (vector layout, the anchor stack missing from the list) -> enumeration reports failure and
// the caller keeps the old first-stack behaviour.
static const uintptr_t O_ITEMS=0x08, I_WUID=0x30, I_CLASS=0x48, I_AMOUNT=0x50, I_HEALTH=0x54, ITEM_SIZE=0xA8;
struct Stack { void* it; int amount; float health; uint64_t wuid; };
static const char* gstr(const GUID16& g){ static char b[4][40]; static int i=0; char* s=b[i++&3]; sprintf_s(s,40,"%08x-%04x",(unsigned)(g.hi>>32),(unsigned)((g.hi>>16)&0xFFFF)); return s; }
static bool stacks_of(void* inv,const GUID16& g,std::vector<Stack>& out){
    out.clear();
    void* first=pFindItemByClass(inv,&g); if(!first) return true;          // none in the inventory
    uintptr_t v=(uintptr_t)inv+O_ITEMS;
    if(!kc::readable(v,24)) return false;
    uintptr_t b=((uintptr_t*)v)[0], e=((uintptr_t*)v)[1], c=((uintptr_t*)v)[2];
    if(!b || b>e || e>c || (e-b)%8 || (e-b)/8>100000 || !kc::readable(b,e-b)) return false;
    if(!kc::readable((uintptr_t)first,ITEM_SIZE)) return false;
    void* cls=F<void*>(first,I_CLASS); if(!cls) return false;
    bool sawFirst=false;
    for(uintptr_t p=b;p<e;p+=8){
        void* it=*(void**)p; if(!it || !kc::readable((uintptr_t)it,ITEM_SIZE)) continue;
        if(it==first) sawFirst=true;
        if(F<void*>(it,I_CLASS)!=cls) continue;
        int amt=F<int32_t>(it,I_AMOUNT); float h=F<float>(it,I_HEALTH);
        if(amt<=0 || amt>100000 || !(h>=-0.01f && h<=1.5f)) return false;   // not an item stack as we know it
        out.push_back({it,amt,h,F<uint64_t>(it,I_WUID)});
    }
    return sawFirst && !out.empty();
}
static int available(void* inv,const GUID16& g){          // all matching stacks, fresh and dried
    GUID16 d=dried_variant(g); bool hd=!(d==NULLGUID);
    std::vector<Stack> a,b; bool ok=stacks_of(inv,g,a) && (!hd || stacks_of(inv,d,b));
    if(!ok){ int n=item_amount(pFindItemByClass(inv,&g)); if(hd) n+=item_amount(pFindItemByClass(inv,&d)); return n; }
    int n=0; for(auto& s:a) n+=s.amount; for(auto& s:b) n+=s.amount; return n;
}
// Which stack the station is stocked with: the healthiest FRESH stack that holds the whole quantity;
// otherwise the healthiest DRIED stack that does.  false = neither can supply it: refuse the brew (logged).
static bool choose_stock(void* inv,const GUID16& g,int qty,uint64_t& wuid){
    GUID16 d=dried_variant(g); bool hd=!(d==NULLGUID);
    std::vector<Stack> fresh,dried; bool ok=stacks_of(inv,g,fresh) && (!hd || stacks_of(inv,d,dried));
    if(!ok){                                              // enumeration not trustworthy: old behaviour
        static bool said=false; if(!said){ said=true; logf("herb stacks cannot be listed on this build - using the game's first stack"); }
        void* it=pFindItemByClass(inv,&g); if(!it && hd) it=pFindItemByClass(inv,&d);
        if(!it) return false; wuid=F<uint64_t>(it,I_WUID); return true;
    }
    int freshTotal=0, driedTotal=0; for(auto& s:fresh) freshTotal+=s.amount; for(auto& s:dried) driedTotal+=s.amount;
    if(kc::g_debug){
        for(size_t i=0;i<fresh.size();i++) logf("herb %s need %d: fresh stack %zu amount %d health %.3f",gstr(g),qty,i,fresh[i].amount,fresh[i].health);
        for(size_t i=0;i<dried.size();i++) logf("herb %s need %d: dried stack %zu amount %d health %.3f",gstr(g),qty,i,dried[i].amount,dried[i].health);
    }
    auto best=[&](std::vector<Stack>& v)->Stack*{ Stack* b=nullptr; for(auto& s:v) if(s.amount>=qty && (!b || s.health>b->health)) b=&s; return b; };
    if(Stack* s=best(fresh)){ wuid=s->wuid; KC_DLOG("herb %s: selected FRESH stack amount %d health %.3f",gstr(g),s->amount,s->health); return true; }
    if(Stack* s=best(dried)){
        wuid=s->wuid;
        KC_DLOG("herb %s: no single fresh stack holds %d (%zu fresh stack(s), %d in total) - selected DRIED stack amount %d health %.3f",gstr(g),qty,fresh.size(),freshTotal,s->amount,s->health);
        return true;
    }
    logf("herb %s: no single stack holds the %d needed (the table takes herbs from one stack) - brew refused",gstr(g),qty);
    KC_DLOG("herb %s: fresh %zu stack(s) %d in total, dried %zu stack(s) %d in total",gstr(g),fresh.size(),freshTotal,dried.size(),driedTotal);
    return false;
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
    char buf[768]; int n=sprintf_s(buf,"%s: m_qualityCoefBase %.3f, pot records %zu:",tag,F<float>(alc,O_QCOEF),v->size());
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
static bool g_quietPlan=false;   // menu building plans every learned recipe: no per-stage log lines

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
            if(!g_quietPlan) KC_DLOG("stage %zu: weak %d turn(s), strong %d turn(s)",i,wT,sT);
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
static float    g_boilBase=0;          // SandTurn: the pot's weak/strong boil seconds when the op started
// One recipe turn = the HourglassTimeout RPG param (10 s), the unit of the game's boil check.  The
// leads come off the last turn of a window (Autobrew's values): the pot keeps boiling until the
// lift animation starts, and before a strong window the first bellows pull still boils weak.
static const float TURN_S=10.0f, STOP_LEAD_S=0.5f, PRE_STRONG_LEAD_S=1.5f;
static const uint64_t SAND_GRACE_MS=2000;   // strong turn: bellows without the sandglass if it can't be turned
static kc::Hotkey g_key;
static int      g_insane=0; static bool g_dead=false;
static const uint64_t STALL_ABORT_MS=12000;

static void queue_stopped(const char* why);
static void to_idle(const char* why){ if(why) logf("stop: %s (recipe %u, op %zu/%zu)",why,g_recipeId,g_cur,g_plan.size()); g_phase=Idle; g_plan.clear(); g_cur=0; g_sub=0; g_recipeId=0; if(why) queue_stopped(why); }

// =====================================================================================================
// AutoBrew 2.0: recipe menu and mixed-potion queue  ([Options] Queue=1; off by default)
// =====================================================================================================
// The menu lists Henry's learned recipes (the game's own IsRecipeLearned) that this planner can brew, with the
// amount queued per recipe, the ingredients of the highlighted recipe, and the ingredients the whole queue needs
// against what Henry carries (src/brew_queue.h).  It is drawn with the script system's System.DrawText.
// Brews run one after another through the 1.0.3 engine above, unchanged: for each brew the table is reset and
// stocked for that recipe (ClearIngredientSlots + ApplyIngredient, the game's own stocking calls), so the recipe
// changes without the book -- the game grades a brew by what is in the pot, not by the open page (GradeBrew
// 0x2E16680 matches the pot against the recipe table; "no recipe matched" is its own verdict).
// AutoContinue=1 keeps the result popup closed between brews through the game's own flag C_Alchemy+0x6A2
// (m_presentResult: it gates only the popup; the potion, quality, XP and statistics are granted regardless, as
// in Autobrew's looping); the last brew of the queue shows the popup as usual.
// Every new game lookup is optional: if one is missing, the queue turns off and F8 works exactly as in 1.0.3.
static const char* SIG_RECIPEDB_REF = "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 8B D3 4C 8B 41 10 48 8D 0D ?? ?? ?? ?? 41 FF D0 48 8B F8 48 85 C0"; // 0x8C3B2D autocook gate: GetById on the recipe DB 0x5332B68
static const char* SIG_RPGMODULE_REF = "48 8B 05 ?? ?? ?? ?? 44 8B 43 30 48 8B 53 20 48 8B 88 98 00 00 00";  // 0x1326A8A C_RecipePerk: [C_RPGModule 0x53322A0]+0x98 = C_RPGMinigames
static const char* SIG_ISLEARNED   = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 41 8B D8 48 8B FA 48 8B F1 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 8B D3 FF 50 10"; // C_RPGMinigames vtbl[9] 0x2CF84D0 IsRecipeLearned(soul, id)
static const char* SIG_GETLOC_CALL = "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 68 05 00 00";                  // mov rcx,[gEnv->pSystem 0x492D8C0]; call GetLocalizationManager (16 sites)
static const char* SIG_SYS_GETLOC  = "48 8B 81 50 0D 00 00 C3";                                          // CSystem vtbl[173] 0x3D3300
static const char* SIG_LOCALIZE    = "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 70 33 FF 45 8A E1 49 8B F0 4C 8B F2 4C 8B E9 48 39 79 18 0F 84 ?? ?? ?? ?? 48 85 D2 0F 84 ?? ?? ?? ?? 80 3A 40"; // CLocalizedStringsManager vtbl[23] 0x51D468 LocalizeLabel(label, CryString&, english)
static const char* SIG_ITEMUINAME  = "E8 ?? ?? ?? ?? 0F 28 44 24 30 48 8D 54 24 20 48 8D 0D ?? ?? ?? ?? 66 0F 7F 44 24 20 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 10 48 8B C8 4C 8B 82 C8 00 00 00 33 D2 41 FF D0 48 8B CB 48 8B 90 A0 00 00 00"; // 0x2ABC37D ItemManager.GetItemUIName: class vtbl[25](0)+0xA0
static const char* SIG_PRESENTER_TAIL = "B9 A2 06 00 00 00 74 09 48 83 C1 68 E8 ?? ?? ?? ?? 48 83 C4 28 C3"; // stable body after the function first 5 bytes (the entry may have a JMP from another mod)  // 0x2E18ED0 "if (m_presentResult) emit brew result"
static const char* SIG_SS_UPDATE   = "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 3D ?? ?? ?? ?? 48 8B F1 33 D2 0F 29 74 24 20";   // CScriptSystem vtbl[1] 0xA26B94
static const char* SIG_SS_EXEC     = "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 48 8B F9 48 89 50 E8"; // CScriptSystem vtbl[6] ExecuteBuffer 0x4D4484
static const uintptr_t O_PRESENT=0x6A2, O_SOUL=0x668, RDB_IFACE=0x78, RDB_ROWS=0x30, ROW_SIZE=0x80, ROW_ID=0x08, ROW_NAME=0x10, ICLS_UINAME=0xA0;

typedef bool  (*IsLearned_t)(void*, void*, uint32_t);
typedef void* (*GetLocMgr_t)(void*);
typedef bool  (*Localize_t)(void*, const char*, void*, bool);
typedef void* (*ClassAs_t)(void*, int);
typedef void* (*SsUpdate_t)(void*, void*, void*, void*);
typedef bool  (*SsExec_t)(void*, const char*, size_t, const char*, void*);

static bool g_qOn=false, g_qAuto=true, g_qNotices=true, g_present=false;
static uintptr_t q_rdbIface=0, q_vtRecipe=0, q_rpgGlobal=0, q_vtMinigames=0, q_sysGlobal=0, q_vtSystem=0, q_vtLoc=0;
static IsLearned_t pIsLearned=nullptr; static Localize_t pLocalize=nullptr;
static SsUpdate_t g_ssUpdate=nullptr; static SsExec_t g_ssExec=nullptr;
static kc::Hotkey g_kPlus, g_kMinus, g_kPadPlus, g_kPadMinus, g_kUp, g_kDown, g_kClear;
static char g_keyHelp[160]="";
static float g_ovX=60, g_ovY=140, g_ovSize=1.4f, g_ovLine=20; static int g_ovRows=12;

// ---- game lookups (all checked; anything unexpected returns "unknown" and the caller degrades) ----
static void* soul_of(void* alc){ void* a=F<void*>(alc,O_ACTOR); return a? F<void*>(a,O_SOUL) : nullptr; }
static void* recipe_row(uint32_t id){                // the game's recipe table row (C_AlchemyRecipe, inline 0x80 rows)
    if(!q_rdbIface) return nullptr;
    uintptr_t base=q_rdbIface-RDB_IFACE, b=F<uintptr_t>((void*)base,RDB_ROWS), e=F<uintptr_t>((void*)base,RDB_ROWS+8);
    if(!b || e<b || (e-b)%ROW_SIZE || (e-b)/ROW_SIZE>5000 || !kc::readable(b,e-b)) return nullptr;
    for(uintptr_t r=b;r<e;r+=ROW_SIZE) if(*(uintptr_t*)r==q_vtRecipe && F<uint32_t>((void*)r,ROW_ID)==id) return (void*)r;
    return nullptr;
}
static void* minigames(){
    if(!q_rpgGlobal) return nullptr;
    void* mod=*(void**)q_rpgGlobal; if(!mod || !kc::readable((uintptr_t)mod,0xA0)) return nullptr;
    void* mg=F<void*>(mod,0x98); return (mg && kc::is_a((uintptr_t)mg,q_vtMinigames))? mg : nullptr;
}
static bool learned(void* soul,uint32_t id){ void* mg=minigames(); return mg && soul && pIsLearned && pIsLearned(mg,soul,id); }
// SEH wrappers kept free of C++ objects (no unwinding inside __try)
static int call_localize(void* mgr,const char* label,void** s){
    __try{ return pLocalize(mgr,label,s,false)? 1 : 0; } __except(EXCEPTION_EXECUTE_HANDLER){ return -1; }
}
static int exec_lua(void* self,const char* text,size_t n,const char* name){   // 1 ran, 0 script error, -1 fault
    __try{ return g_ssExec(self,text,n,name,nullptr)? 1 : 0; } __except(EXCEPTION_EXECUTE_HANDLER){ return -1; }
}
static std::string localize(const char* key){   // "@key" -> text in the game's language; the key itself if unknown
    if(!key || !*key || !kc::readable((uintptr_t)key,1)) return "";
    std::string label = key[0]=='@'? key : std::string("@")+key;
    void* sys=q_sysGlobal? *(void**)q_sysGlobal : nullptr;
    if(pLocalize && sys && kc::is_a((uintptr_t)sys,q_vtSystem)){
        void* mgr=((GetLocMgr_t)(*(void***)sys)[173])(sys);
        if(mgr && kc::is_a((uintptr_t)mgr,q_vtLoc)){
            // A CryString with a static header (refcount -1 = never freed): the game writes into it or points it
            // at its own string; nothing of ours is released by the game, nothing of the game's by us.
            struct { int32_t ref, len, cap; char buf[256]; } h={-1,0,255,{0}};
            void* s=h.buf;
            int ok=call_localize(mgr,label.c_str(),&s);
            if(ok<0){ pLocalize=nullptr; logf("queue: localization lookup failed - recipe names shown as keys"); }
            else if(ok && s && kc::readable((uintptr_t)s,1) && *(const char*)s){ std::string r((const char*)s); if(r!=label) return r; }
        }
    }
    return label.substr(1);
}
static std::string item_name(const GUID16& g){
    void* cls=pFindClassByGuid(pItemDb,&g); if(!cls) return gstr(g);
    void* pk=((ClassAs_t)(*(void***)cls)[25])(cls,0);
    const char* key=pk? F<const char*>(pk,ICLS_UINAME) : nullptr;
    std::string n=localize(key); return n.empty()? gstr(g) : n;
}
static int count_class(void* inv,const GUID16& g){
    std::vector<Stack> v; if(stacks_of(inv,g,v)){ int n=0; for(auto& s:v) n+=s.amount; return n; }
    return item_amount(pFindItemByClass(inv,&g));
}
static bq::Key qkey(const GUID16& g){ return {g.hi,g.lo}; }

// ---- on-screen text (script system update hook; drawn only while the table is live) ----
static SRWLOCK g_ovLock=SRWLOCK_INIT;
static std::vector<bq::Line> g_ov; static uint64_t g_ovStamp=0;
static std::vector<std::string> g_notes;
static void lua_quote(std::string& out,const std::string& s){
    out+='"';
    for(unsigned char c:s){ if(c=='"'||c=='\\'){ out+='\\'; out+=(char)c; } else if(c<0x20) out+=' '; else out+=(char)c; }
    out+='"';
}
static void notice(const std::string& msg){
    logf("%s",msg.c_str());
    if(!g_qNotices || !g_ssExec) return;
    std::string cmd="Game.SendInfoText("; lua_quote(cmd,msg); cmd+=", false)";
    AcquireSRWLockExclusive(&g_ovLock); if(g_notes.size()<8) g_notes.push_back(cmd); ReleaseSRWLockExclusive(&g_ovLock);
}
static void overlay(std::vector<bq::Line> lines,uint64_t now){
    AcquireSRWLockExclusive(&g_ovLock); g_ov=std::move(lines); g_ovStamp=now; ReleaseSRWLockExclusive(&g_ovLock);
}
static void* SsUpdateHook(void* self,void* a,void* b,void* c){
    void* r=g_ssUpdate(self,a,b,c);
    if(!g_ssExec) return r;
    std::string chunk; std::vector<std::string> notes;
    AcquireSRWLockExclusive(&g_ovLock);
    notes.swap(g_notes);
    if(!g_ov.empty() && GetTickCount64()-g_ovStamp<400){          // the alchemy table ticked just now
        static const float col[4][3]={{0.95f,0.92f,0.85f},{1.0f,0.84f,0.38f},{1.0f,0.45f,0.35f},{0.65f,0.65f,0.62f}};
        char b2[96];
        for(size_t i=0;i<g_ov.size();i++){
            const float* k=col[g_ov[i].tone&3];
            sprintf_s(b2,"System.DrawText(%.0f,%.0f,",g_ovX,g_ovY+g_ovLine*i); chunk+=b2; lua_quote(chunk,g_ov[i].text);
            sprintf_s(b2,",\"default\",%.2f,%.2f,%.2f,%.2f,1)\n",g_ovSize,k[0],k[1],k[2]); chunk+=b2;
        }
    }
    ReleaseSRWLockExclusive(&g_ovLock);
    static int drawErrors=0; static bool drawOff=false;
    int ok=1;
    for(auto& n:notes) if(ok>=0) ok=exec_lua(self,n.c_str(),n.size(),"TheatricalAutobrew");
    if(ok>=0 && !chunk.empty() && !drawOff){
        int d=exec_lua(self,chunk.c_str(),chunk.size(),"TheatricalAutobrewMenu");
        if(d==0 && ++drawErrors>=3){ drawOff=true; logf("queue: System.DrawText did not run (see kcd.log) - the on-screen menu is off; notices still show"); }
        else if(d==1) drawErrors=0;
        if(d<0) ok=-1;
    }
    if(ok<0){ g_ssExec=nullptr; logf("queue: on-screen text failed - menu and notices off"); }
    return r;
}
// vtbl[1] of CScriptSystem, chained with compare-exchange: our other mods hook the same slot (as AutoFollow does).
static bool hook_ss_update(uintptr_t vt,uintptr_t expect){
    void** slot=&((void**)vt)[1];
    DWORD old; if(!VirtualProtect(slot,8,PAGE_READWRITE,&old)) return false;
    void* cur=(void*)expect; bool ok=false;
    for(int i=0;i<8 && !ok;i++){
        g_ssUpdate=(SsUpdate_t)cur;
        void* was=InterlockedCompareExchangePointer(slot,(void*)&SsUpdateHook,cur);
        if(was==cur) ok=true; else cur=was;
    }
    VirtualProtect(slot,8,old,&old);
    return ok;
}

// ---- menu ----
static bq::Queue g_menu;
static bool g_menuOpen=false; static uint64_t g_menuStock=0; static std::string g_menuStatus;
static bq::Stock g_stock;
static void refresh_stock(void* alc){
    void* inv=inventory_of(alc); bq::Stock s;
    if(inv) for(auto& r:g_menu.recipes) for(auto& i:r.ing){
        for(const bq::Key& k:{i.cls,i.dried}) if(!k.null() && !s.get(k) ){ GUID16 g{k.hi,k.lo}; s.set(k,count_class(inv,g)); }
    }
    g_stock=s;
}
static void menu_build(void* alc){
    void* soul=soul_of(alc);
    static std::vector<std::pair<uint32_t,std::string>> names;     // localized once per session
    auto rname=[&](uint32_t id,void* row)->std::string{
        for(auto& n:names) if(n.first==id) return n.second;
        std::string s=localize(F<const char*>(row,ROW_NAME)); if(s.empty()) s="recipe "+std::to_string(id);
        names.push_back({id,s}); return s;
    };
    static std::vector<std::pair<GUID16,std::string>> inames;
    auto iname=[&](const GUID16& g)->std::string{ for(auto& n:inames) if(n.first==g) return n.second; std::string s=item_name(g); inames.push_back({g,s}); return s; };
    std::vector<bq::Recipe> rows; int nLearned=0, nUnplannable=0;
    g_quietPlan=true;
    for(auto& r:g_recipes){
        void* row=recipe_row(r.id); if(!row || !learned(soul,r.id)) continue;
        nLearned++;
        std::vector<Op> tmp; if(r.bad || !build_plan(&r,tmp)){ nUnplannable++; continue; }
        bq::Recipe m; m.id=r.id; m.name=rname(r.id,row);
        for(auto& i:r.ing){ GUID16 d=dried_variant(i.first); m.ing.push_back({qkey(i.first),qkey(d),iname(i.first),i.second}); }
        rows.push_back(m);
    }
    g_quietPlan=false;
    std::stable_sort(rows.begin(),rows.end(),[](const bq::Recipe& a,const bq::Recipe& b){ return a.name<b.name; });
    g_menu.set_recipes(rows);
    logf("queue: menu lists %zu recipes (%d learned, %d of them with steps AutoBrew cannot do)",rows.size(),nLearned,nUnplannable);
}
static void menu_show(uint64_t now){
    auto L=g_menu.menu(g_stock,g_ovRows,g_keyHelp,g_menuStatus);
    overlay(std::move(L),now);
}

// ---- queue run ----
struct QRun { bool active=false; std::vector<uint32_t> seq; size_t pos=0; int made=0, skipped=0; bool cleared=false; uint64_t lastTick=0, offCtx=0;
              bool after=false, sawBusy=false; uint64_t finishAt=0, dirtySince=0; };   // after: waiting out the previous brew's finish
static QRun g_run;
static std::string qname(uint32_t id){ int i=g_menu.row_of(id); return i>=0? g_menu.recipes[i].name : "recipe "+std::to_string(id); }
static void restore_present(void* alc){ if(g_run.cleared && alc){ F<uint8_t>(alc,O_PRESENT)=1; g_run.cleared=false; KC_DLOG("queue: result popup back on"); } }
static void* g_lastAlc=nullptr;
static void queue_end(const std::string& msg){
    restore_present(g_lastAlc);
    notice(msg);
    if(g_run.skipped) logf("queue: %d brew(s) skipped",g_run.skipped);
    g_run=QRun();
}
// Called by to_idle with a reason: the engine stopped (key, stall, interruption).  The queue stops with it.
static void queue_stopped(const char* why){ if(g_run.active) queue_end(std::string("AutoBrew queue: cancelled - ")+why); }
static void begin_item(void* alc,uint64_t now){
    while(g_run.pos<g_run.seq.size()){
        uint32_t id=g_run.seq[g_run.pos]; Recipe* r=find_recipe(id);
        if(r && !r->bad) break;
        notice("AutoBrew queue: skipped "+qname(id)+" - recipe not brewable"); g_run.skipped++; g_run.pos++;
    }
    if(g_run.pos>=g_run.seq.size()){ queue_end("AutoBrew queue: done, "+std::to_string(g_run.made)+" brewed"); return; }
    g_recipeId=g_run.seq[g_run.pos]; g_phase=Closing; g_t=now;
    notice("AutoBrew queue: "+std::to_string(g_run.pos+1)+" of "+std::to_string(g_run.seq.size())+" - "+qname(g_recipeId));
    (void)alc;
}
static void queue_start(void* alc,uint64_t now,int mode){
    refresh_stock(alc);
    if(int cut=g_menu.trim(g_stock)) logf("queue: lowered by %d to what Henry carries now",cut);
    if(g_menu.empty()){ g_menuStatus="Nothing queued: + adds the highlighted recipe."; return; }
    g_run=QRun(); g_run.active=true; g_run.seq=g_menu.sequence(); g_run.lastTick=now;
    logf("queue: %s",g_menu.summary().c_str());
    notice("AutoBrew queue: started, "+std::to_string(g_run.seq.size())+" brews");
    g_menuOpen=false; g_menu.clear();
    if(mode==MODE_READING) tap(SC_R);                 // close the book first, as for a single brew
    begin_item(alc,now);
}
// Arming could not stock or plan this brew: skip it, go on with the next.
static void queue_skip(void* alc,uint64_t now,const char* why){
    notice("AutoBrew queue: skipped "+qname(g_recipeId)+" - "+why);
    g_run.skipped++; g_run.pos++; g_phase=Idle; g_plan.clear(); g_cur=0; g_sub=0; g_run.after=false;
    begin_item(alc,now);
}
// The table looks idle.  After a brew, wait until the game itself has finished it: the last verb's animation ran
// (or 3 s passed) and the game cleared the table after grading.  The queue never resets a table holding an
// ungraded brew; if the table is not cleared within 20 s, the queue stops and leaves it to you.
static bool queue_ready(void* alc,uint64_t now){
    if(!g_run.after) return true;
    if(!g_run.sawBusy && now-g_run.finishAt<3000) return false;
    if(workspace_dirty(alc)){
        if(!g_run.dirtySince) g_run.dirtySince=now;
        else if(now-g_run.dirtySince>20000) to_idle("the table was not cleared after the last brew - finish it by hand");
        return false;
    }
    return true;
}
static void queue_table_ready(void* alc){ restore_present(alc); g_run.after=false; }
// Cooking starts: keep the popup closed unless this is the last brew (AutoContinue=1).
static void queue_cooking(void* alc){
    bool last=g_run.pos+1>=g_run.seq.size();
    if(g_qAuto && g_present && !last && F<uint8_t>(alc,O_PRESENT)==1){ F<uint8_t>(alc,O_PRESENT)=0; g_run.cleared=true; KC_DLOG("queue: result popup off for this brew"); }
}
static void queue_brewed(void* alc,uint64_t now){
    g_run.made++; g_run.pos++; g_run.after=true; g_run.sawBusy=false; g_run.finishAt=now; g_run.dirtySince=0;
    logf("queue: brew %d done (%s)",g_run.made,qname(g_recipeId).c_str());
    g_phase=Idle; g_plan.clear(); g_cur=0; g_sub=0;
    begin_item(alc,now);   // waits in Closing until the finish animation is over and the table is idle
}
static void queue_progress(uint64_t now){
    if(!g_run.active) return;
    std::vector<bq::Line> L;
    char b[200]; sprintf_s(b,"AutoBrew queue: %zu of %zu - %s",std::min(g_run.pos+1,g_run.seq.size()),g_run.seq.size(),qname(g_recipeId).c_str());
    L.push_back({b,1});
    char kn[16]; sprintf_s(b,"%s stops and cancels the queue",kc::key_name(g_key.vk,kn,sizeof kn)); L.push_back({b,3});
    overlay(std::move(L),now);
}
// Menu keys and F8 at the table.  Returns true if it used the F8 press.
static bool queue_frame(void* alc,uint64_t now,bool pressed,int mode){
    g_lastAlc=alc;
    bool plus=g_kPlus.pressed(), minus=g_kMinus.pressed(), pp=g_kPadPlus.pressed(), pm=g_kPadMinus.pressed();
    bool up=g_kUp.pressed(), down=g_kDown.pressed(), clr=g_kClear.pressed();
    plus=plus||pp; minus=minus||pm;
    if(g_run.active){
        bool atCtx=!strcmp(context(alc),"alchemy") || !strcmp(context(alc),"alchemy_reading");
        if(g_run.lastTick && now-g_run.lastTick>5000 && !atCtx){ to_idle("left the table"); return true; }   // the table stopped ticking
        g_run.lastTick=now;
        if(!atCtx){
            if(!g_run.offCtx) g_run.offCtx=now; else if(now-g_run.offCtx>3000){ to_idle("left the table"); return true; }
        } else g_run.offCtx=0;
        queue_progress(now);
        return false;                                  // F8 while brewing: the engine stops (and cancels the queue)
    }
    if(g_phase!=Idle) return false;                    // a single 1.0.3 brew is running
    if(g_menuOpen && strcmp(context(alc),"alchemy") && strcmp(context(alc),"alchemy_reading")){ g_menuOpen=false; logf("queue: menu closed (left the table)"); return false; }
    if(pressed){
        if(g_menuOpen){
            if(g_menu.empty()){ g_menuOpen=false; logf("queue: menu closed"); return true; }
            queue_start(alc,now,mode); return true;
        }
        if(mode==MODE_BREWING){                        // book closed at the table: open the menu
            menu_build(alc); refresh_stock(alc); g_menuOpen=true; g_menuStatus.clear(); g_menuStock=now;
            logf("queue: menu opened");
            menu_show(now); return true;
        }
        return false;                                  // a recipe page is open: brew it once (1.0.3)
    }
    if(!g_menuOpen) return false;
    bool changed=false;
    if(up){ g_menu.move(-1); changed=true; }
    if(down){ g_menu.move(1); changed=true; }
    if(plus^minus){
        refresh_stock(alc); g_menuStock=now; g_menuStatus.clear();
        if(plus){
            std::string why;
            if(g_menu.add(g_stock,g_menu.cursor,&why)!=bq::Add::Added){ g_menuStatus="Not enough ingredients for one more: "+why; logf("queue: + refused (%s)",why.c_str()); }
        } else g_menu.remove(g_menu.cursor);
        changed=true;
    }
    if(clr){ g_menu.clear(); g_menuStatus.clear(); changed=true; }
    if(now-g_menuStock>2000){ refresh_stock(alc); g_menuStock=now; }
    if(changed) KC_DLOG("queue: menu row %d (%s), queue %s",g_menu.cursor,g_menu.recipes.empty()?"-":g_menu.recipes[g_menu.cursor].name.c_str(),g_menu.summary().c_str());
    menu_show(now);
    return false;
}

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
    if(g_qOn && queue_frame(alc,now,pressed,mode)) return;   // the 2.0 menu used the key (or the queue was cancelled)

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
            if(g_run.active && !queue_ready(alc,now)) break;
            if(workspace_dirty(alc)){ pReset(alc); logf("workspace reset"); }
            if(g_run.active) queue_table_ready(alc);
            g_phase=Arming;
        } else if(g_run.active){                          // between queued brews: the finish animation (and, with
            g_run.sawBusy=true;                           //   AutoContinue=0, the result popup) comes first
            bool wait=!g_qAuto || !g_present;
            if(!wait && now-g_t>60000) to_idle("the table did not get ready for the next brew");
        } else if(now-g_t>15000) to_idle("book did not close");
        break;
    case Arming: {
        Recipe* r=find_recipe(g_recipeId); void* inv=inventory_of(alc);
        if(!r||!inv){ to_idle("lost recipe/inventory"); break; }
        std::vector<uint64_t> stock;
        for(auto& i:r->ing){
            uint64_t w=0;
            if(!choose_stock(inv,i.first,i.second,w)){
                if(g_run.active){ queue_skip(alc,now,"not enough ingredients"); return; }
                to_idle("ingredients cannot be stocked (see the lines above)"); return;
            }
            stock.push_back(w);
        }
        pClearSlots(alc);
        for(uint64_t w:stock) pApplyIngredient(alc,w);
        if(!build_plan(r,g_plan)){ if(g_run.active){ queue_skip(alc,now,"steps AutoBrew cannot do"); return; } to_idle("recipe not plannable"); break; }
        logf("plan %zu ops",g_plan.size());
        if(g_run.active) queue_cooking(alc);
        g_phase=Cooking; g_cur=0; g_sub=0; g_stall=now;
        break;
    }
    case Cooking: {
        if(g_cur>=g_plan.size()){ logf("brew finished (recipe %u)",g_recipeId); if(g_run.active){ queue_brewed(alc,now); break; } to_idle(nullptr); break; }
        Op& op=g_plan[g_cur];
        bool sand=op.k==Op::SandTurn;   // timed off the boil timer, through the sandglass/bellows animations
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
            // g_sub 0: op start, 1: waiting to turn the sandglass, 2: sandglass turned (or skipped).
            // The turn ends on the game's boil timer, so the sand may still be running at the lift.
            void* base=pot_base_record(alc);
            if(!base){ to_idle("no base liquid in the pot"); return; }
            if(!F<uint8_t>(alc,O_POTONFIRE) && !F<uint8_t>(alc,O_POTMOVING)){ to_idle("pot left the fire"); return; }
            float s=F<float>(alc,O_SAND);
            float boiled=F<float>(base,op.strong?0x18:0x14);
            if(g_sub==0){ g_boilBase=boiled; g_t=now; g_sub=1; }
            float done=boiled-g_boilBase;
            if(g_sub==1){                                   // weak: flip at the boil ("when you hear it bubbling");
                int boil=F<uint8_t>(alc,O_BOILSTATE);       //   strong: flip first, then the bellows.  0 heating, 1 weak, 2 strong
                bool ready=op.strong || boil>=1;
                if(ready && s<=0.0001f && pCanPerformVerb(alc,TurnSandglass)){
                    pPerformVerb(alc,TurnSandglass); g_sub=2;
                    KC_DLOG("sandglass turn (%s) at boil %d, %.1fs boiled",op.strong?"strong":"weak",boil,done);
                } else if(op.strong && (s>0.0001f || now-g_t>SAND_GRACE_MS)) g_sub=2;   // sand still running from the
            }                                                                          //   previous turn: bellows now
            if(op.strong && g_sub==2){                      // keep the heat up with the bellows (Autobrew's rule)
                float remaining=TURN_S-done + TURN_S*op.strongAfter;
                float charge=(F<float>(alc,O_FIRE)-1.0f)*6.0f;   // fire intensity drains 1/6 per s down to 1.0
                if(remaining>charge) F<int>(alc,O_PENDING)=UseBellows;
                else if(F<int>(alc,O_PENDING)==UseBellows) F<int>(alc,O_PENDING)=VNone;
            }
            const Op* next=g_cur+1<g_plan.size()? &g_plan[g_cur+1] : nullptr;
            float lead = next && next->k==Op::SandTurn ? (next->strong && !op.strong? PRE_STRONG_LEAD_S : 0.0f) : STOP_LEAD_S;
            if(done>=TURN_S-lead){                          // the turn's boil time is in: next turn or lift the pot
                if(op.strong && op.strongAfter==0 && F<int>(alc,O_PENDING)==UseBellows) F<int>(alc,O_PENDING)=VNone;
                KC_DLOG("turn done (%s): %.1fs boiled, sand %.2f",op.strong?"strong":"weak",done,s);
                adv=true;
            } else if(now-g_t>(uint64_t)(3*TURN_S*1000)+30000){ to_idle("pot did not boil"); return; }
            else prog=true;
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
// The 2.0 queue's lookups (optional: a failure here turns the queue off and nothing else).
struct QFound { uintptr_t rdbSite, rdbIface, vtRdb, vtRecipe, rpgSite, rpgGlobal, vtMg, isLearned, sysGlobal, vtSys, getLoc, vtLoc, localize, uiName, presenter, vtSs, ssUpdate, ssExec; };
// Every match of `pat` must point (rip-relative disp32 at +disp) at the same address; at least minHits matches.
static uintptr_t find_consensus(kc::Resolver& R,const char* what,const char* pat,int disp,int minHits){
    kc::Pattern p(pat); uintptr_t tgt=0; int n=0;
    for(uintptr_t a=R.im.text0; a<R.im.text1; ){
        uintptr_t m=0; if(!kc::scan_all(a,R.im.text1,p,&m,1)) break;
        uintptr_t t=kc::rel32(m+disp);
        if(n && t!=tgt){ R.fail(what,"references disagree"); return 0; }
        tgt=t; n++; a=m+1;
    }
    if(n<minHits){ R.fail(what,"signature not found"); return 0; }
    if(!R.im.in_image(tgt) || R.im.in_text(tgt)){ R.fail(what,"address outside game data"); return 0; }
    return tgt;
}
static bool resolve_queue(kc::Resolver& R, QFound& q, uintptr_t findclass){
    int before=R.fails;
    q.rdbSite=R.find("recipe database",SIG_RECIPEDB_REF);
    if(q.rdbSite){ q.rdbIface=R.riprel("recipe database",q.rdbSite+0x15); if(R.riprel("recipe database",q.rdbSite+8)!=q.rdbIface) R.fail("recipe database","references disagree"); }
    q.vtRdb=R.vtable("C_AlchemyRecipeDatabase",".?AVC_AlchemyRecipeDatabase@playermodule@wh@@",(uint32_t)RDB_IFACE);
    q.vtRecipe=R.vtable("C_AlchemyRecipe",".?AVC_AlchemyRecipe@playermodule@wh@@",0);
    q.rpgSite=R.find("RPG module",SIG_RPGMODULE_REF); if(q.rpgSite) q.rpgGlobal=R.riprel("RPG module",q.rpgSite+3);
    q.vtMg=R.vtable("C_RPGMinigames",".?AVC_RPGMinigames@rpgmodule@wh@@",0);
    q.isLearned=R.slot("C_RPGMinigames::IsRecipeLearned",q.vtMg,9,SIG_ISLEARNED);
    q.sysGlobal=find_consensus(R,"gEnv->pSystem",SIG_GETLOC_CALL,3,4);
    q.vtSys=R.vtable("CSystem",".?AVCSystem@@",0); q.getLoc=R.slot("CSystem::GetLocalizationManager",q.vtSys,173,SIG_SYS_GETLOC);
    q.vtLoc=R.vtable("CLocalizedStringsManager",".?AVCLocalizedStringsManager@@",0); q.localize=R.slot("LocalizeLabel",q.vtLoc,23,SIG_LOCALIZE);
    q.uiName=R.find("GetItemUIName",SIG_ITEMUINAME);
    if(q.uiName && R.branch("GetItemUIName",q.uiName+0x1C)!=findclass) R.fail("GetItemUIName","does not look the class up with FindClassByGuid");
    // Other installed KCD2 mods may detour the first five bytes of the game's
    // result presenter with E9 rel32. Our queue does not CALL this function or
    // modify the detour: we only need proof that m_presentResult gates its popup.
    // Resolve the unchanged tail and accept either exact original entry bytes
    // or a readable JMP trampoline. Anything else disables only the 2.0 queue.
    uintptr_t tail=R.find("brew result presenter (stable tail)",SIG_PRESENTER_TAIL);
    if(tail){
        const unsigned char* entry=(const unsigned char*)(tail-5);
        const unsigned char original[5]={0x48,0x83,0xEC,0x28,0x80};
        bool unhooked=memcmp(entry,original,5)==0;
        uintptr_t jumpTo=0;
        bool hooked=entry[0]==0xE9;
        if(hooked){
            int32_t displacement=0; memcpy(&displacement,entry+1,4);
            jumpTo=(uintptr_t)(entry+5)+(intptr_t)displacement;
            hooked=kc::readable(jumpTo,1);
        }
        if(!unhooked && !hooked) R.fail("brew result presenter","entry is neither original nor a readable jump");
        else {
            q.presenter=tail-5;
            if(hooked) logf("queue: result presenter already detoured by another mod; verified unchanged body, leaving its hook untouched");
        }
    }
    q.vtSs=R.vtable("CScriptSystem",".?AVCScriptSystem@@",0);
    uintptr_t cur=q.vtSs? ((uintptr_t*)q.vtSs)[1] : 0;
    if(cur && !R.im.in_image(cur)){ if(R.find("CScriptSystem::Update",SIG_SS_UPDATE)) q.ssUpdate=cur; }   // another mod hooked it first: chain
    else q.ssUpdate=R.slot("CScriptSystem::Update",q.vtSs,1,SIG_SS_UPDATE);
    q.ssExec=R.slot("CScriptSystem::ExecuteBuffer",q.vtSs,6,SIG_SS_EXEC);
    return R.fails==before;
}
#ifdef KC_DEVTOOLS
extern "C" __declspec(dllexport) int KC_SelfTest(HMODULE game, uintptr_t* out){   // offline check (tools/selftest)
    kc::Resolver R; R.im.load(game); Found f{}; QFound q{}; bool ok=resolve(R,f); ok=resolve_queue(R,q,f.findclass) && ok;
    uintptr_t* p=(uintptr_t*)&f; size_t n=0; for(size_t i=0;i<sizeof f/8;i++) out[n++]=p[i];
    p=(uintptr_t*)&q; for(size_t i=0;i<sizeof q/8;i++) out[n++]=p[i];
    return ok? (int)n : -R.fails;
}
#endif

// [Keys] names: everything kc::parse_key takes, plus Up/Down/Left/Right/Delete/Insert/Home/End/PageUp/PageDown/Enter/Backspace.
static int ini_key_named(const char* key,int def,const char* defText,std::string* shown=nullptr){
    char p[MAX_PATH]; char f[96]; sprintf_s(f,"%s.ini",kc::g_name); kc::path(p,sizeof p,f);
    char v[32]={0}; GetPrivateProfileStringA("Keys",key,"",v,sizeof v,p);
    if(shown){ *shown=v; size_t a=shown->find_first_not_of(' '); *shown= a==std::string::npos? defText : shown->substr(a); while(!shown->empty() && shown->back()==' ') shown->pop_back(); if(shown->empty()) *shown=defText; }
    static const struct { const char* n; int vk; } names[]={{"Up",VK_UP},{"Down",VK_DOWN},{"Left",VK_LEFT},{"Right",VK_RIGHT},{"Delete",VK_DELETE},{"Del",VK_DELETE},
        {"Insert",VK_INSERT},{"Home",VK_HOME},{"End",VK_END},{"PageUp",VK_PRIOR},{"PageDown",VK_NEXT},{"Enter",VK_RETURN},{"Backspace",VK_BACK}};
    char* s=v; while(*s==' ') s++;
    for(size_t n=strlen(s); n && s[n-1]==' '; ) s[--n]=0;
    for(auto& e:names) if(!_stricmp(s,e.n)) return e.vk;
    int vk=kc::parse_key(s,def);
    if(!vk){ logf("[Keys] %s=%s is not a key this mod understands - using the default",key,v); vk=def; if(shown) *shown=defText; }
    return vk;
}
static void install_queue(kc::Resolver& R,const Found& f){
    g_qAuto=kc::ini_int("Options","AutoContinue",1)!=0;
    g_qNotices=kc::ini_int("Options","QueueNotices",1)!=0;
    std::string kp,km,ku,kd,kcl;
    g_kPlus.vk=ini_key_named("IncreaseQueue",VK_OEM_PLUS,"+",&kp); g_kMinus.vk=ini_key_named("DecreaseQueue",VK_OEM_MINUS,"-",&km);
    g_kPadPlus.vk=VK_ADD; g_kPadMinus.vk=VK_SUBTRACT;
    g_kUp.vk=ini_key_named("MenuUp",VK_UP,"Up",&ku); g_kDown.vk=ini_key_named("MenuDown",VK_DOWN,"Down",&kd); g_kClear.vk=ini_key_named("ClearQueue",VK_DELETE,"Delete",&kcl);
    g_ovX=(float)kc::ini_int("Overlay","X",60); g_ovY=(float)kc::ini_int("Overlay","Y",140);
    g_ovSize=kc::ini_int("Overlay","SizePercent",140)/100.0f; g_ovLine=(float)kc::ini_int("Overlay","LineHeight",20);
    g_ovRows=std::max(3,std::min(30,kc::ini_int("Overlay","Rows",12)));
    char a[16];
    sprintf_s(g_keyHelp,"%s/%s choose   %s/%s amount   %s clear   %s start (empty queue: close)",
        ku.c_str(),kd.c_str(),kp.c_str(),km.c_str(),kcl.c_str(),kc::key_name(g_key.vk,a,sizeof a));
    QFound q{}; int fails=R.fails;
    if(!resolve_queue(R,q,f.findclass)){ R.fails=fails; g_qOn=false; logf("queue: off (this game version is not supported for the queue) - F8 works as in 1.0.3"); return; }
    q_rdbIface=q.rdbIface; q_vtRecipe=q.vtRecipe; q_rpgGlobal=q.rpgGlobal; q_vtMinigames=q.vtMg; pIsLearned=(IsLearned_t)q.isLearned;
    q_sysGlobal=q.sysGlobal; q_vtSystem=q.vtSys; q_vtLoc=q.vtLoc; pLocalize=(Localize_t)q.localize;
    g_present=q.presenter!=0;
    if(*(uintptr_t*)q_rdbIface!=q.vtRdb) logf("queue: recipe table not set up yet (checked again when the menu opens)");
    g_ssExec=(SsExec_t)q.ssExec;
    if(!hook_ss_update(q.vtSs,q.ssUpdate)){ g_ssExec=nullptr; logf("queue: could not hook the script system - no on-screen menu"); }
    logf("queue: ready (%s at the table with the book closed opens the menu; AutoContinue=%d, notices %s)",kc::key_name(g_key.vk,a,sizeof a),(int)g_qAuto,g_qNotices?"on":"off");
}

static HMODULE g_self=nullptr;
static DWORD WINAPI Init(LPVOID){
    kc::init(g_self,"kcd2_alchemy");
    g_key.vk=kc::ini_key("Brew",VK_F8);
    g_qOn=kc::ini_int("Options","Queue",0)!=0;
    HMODULE h=kc::wait_game(); if(!h){ logf("abort: WHGame.dll not loaded"); return 0; }
    kc::Resolver R; R.im.load(h); B=R.im.base;
    logf("start v2.0.0-test (game ts 0x%08X)%s",R.im.timestamp,g_qOn?", queue on":"");
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
    if(g_qOn) install_queue(R,f);
    pOrigUpdate=(Update_t)f.update;
    kc::write_ptr(&((void**)f.vt)[19],(void*)&UpdateHook);
    char kn[16]; logf("ready (%s on a recipe page)",kc::key_name(g_key.vk,kn,sizeof kn));
    return 0;
}
KC_PLUGIN("Theatrical Autobrew", KC_AUTHOR, 200)
BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){ if(r==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); g_self=h; KC_START(Init); } return TRUE; }
