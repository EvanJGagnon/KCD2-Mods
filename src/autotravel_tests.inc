// autotravel_tests.inc -- regression tests for Horse Route Follow's custom-marker handling: removal,
// a brief disappearance, a new marker, and the route line around fast-travel hovering.
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Development builds only: #included into autotravel.cpp, so the tests drive the mod's own code (map
// refresh, the list read while riding, the game-thread step, arrival, the hide hook) with a stand-in
// for the game: a test road, a map screen (marker list, getter, route line) and a rider.
// Run with tools/markertest.

namespace mt {

static int fails=0;
#define MT_CHECK(c) do { if(!(c)){ fails++; printf("  FAIL line %d: %s\n",__LINE__,#c); } } while(0)

// Test road (written as <dir>\markertest.amg): a straight road along y=2000 from x=1000 to x=2000, a node
// every 10 m (nodes 0..100), and a side road north from x=1500 up to y=2300 (nodes 101..130).
static bool write_graph(const char* dir){
    std::vector<float> xy; std::vector<uint32_t> ab;
    for(int i=0;i<=100;i++){ xy.push_back(1000.f+10*i); xy.push_back(2000.f); if(i) { ab.push_back(i-1); ab.push_back(i); } }
    for(int j=1;j<=30;j++){ xy.push_back(1500.f); xy.push_back(2000.f+10*j); ab.push_back(j==1? 50 : 100+j-1); ab.push_back(100+j); }
    char p[MAX_PATH]; sprintf_s(p,"%s\\markertest.amg",dir);
    FILE* f=nullptr; fopen_s(&f,p,"wb"); if(!f) return false;
    uint32_t nn=(uint32_t)xy.size()/2, ne=(uint32_t)ab.size()/2; float len=10.f;
    fwrite("AMG1",1,4,f); fwrite(&nn,4,1,f); fwrite(&ne,4,1,f); fwrite(xy.data(),4,xy.size(),f);
    for(uint32_t i=0;i<ne;i++){ fwrite(&ab[2*i],4,2,f); fwrite(&len,4,1,f); fwrite(&len,4,1,f); }
    fclose(f); return true;
}

// The game, as the mod sees it.
struct Rec { uint8_t head[0x50]; float x,y; };          // a custom-marker record: position at +0x50
static char tagMap, tagEnt;                             // stand-in vtables (kc::is_a compares the first pointer)
alignas(16) static uint8_t mapObj[0x600];               // C_UIMap: marker list at +0x588/+0x590, UI element at +0x208
alignas(16) static uint8_t ent[0x100];                  // the rider: position at +0x64/+0x74
static uintptr_t list[8], mapSlot;
static Rec recA, recB, recS;                            // the player's marker, a second one, a leftover the map never shows
static uintptr_t shownRec;                              // what the map's getter returns: the marker it shows (0: none)
static int shows; static bool line;                     // the route line on the map

static void* FakeGetter(void*,void* out){ *(uintptr_t*)out=shownRec; return out; }
static void FakeShow(void*,const VecView* v){ shows++; line = v->e-v->b >= 2; }
static void FakeStrCtor(void* s,const char* n){ *(const char**)s=n; }
static void FakeStrDtor(void*){}
static void FakeSend(void* el,void* s,const bool*){ if(el==mapObj+0x208 && !strcmp(*(const char**)s,"HideFastTravelPath")) line=false; }

static void set_list(std::initializer_list<Rec*> l){
    size_t n=0; for(Rec* r : l) list[n++]=(uintptr_t)r;
    *(uintptr_t*)(mapObj+0x588)=(uintptr_t)list; *(uintptr_t*)(mapObj+0x590)=(uintptr_t)(list+n);
}
static void place(Rec& r,float x,float y){ r.x=x; r.y=y; }
static void rider(float x,float y){ *(float*)(ent+0x64)=x; *(float*)(ent+0x74)=y; }
static void refresh(uint64_t t){                        // the map's marker refresh, at time t
    Mk s{shownRec,0,0}; if(shownRec){ s.x=((Rec*)shownRec)->x; s.y=((Rec*)shownRec)->y; }
    map_refresh(mapObj,&s,t);
}
static void hover_end(){                                // the game hides the path of a hovered fast-travel point
    const char* name="HideFastTravelPath"; bool animate=true; line=false;
    HideHook(mapObj+0x208,&name,&animate);
}
static bool approx(float a,float b){ return fabsf(a-b)<0.5f; }
static bool routed_to(float x,float y){                 // a route on the map whose destination is the road point (x,y)
    return g_haveMarker && g_endOk && approx(g_endX,x) && approx(g_endY,y) && !g_route.empty()
        && hypotf(g_cur->x[g_route.back()]-x,g_cur->y[g_route.back()]-y)<=10.f;
}
static bool nothing_left(){                             // no destination, route, arrival or line
    return !g_haveMarker && !g_goneT && g_route.empty() && !g_endOk && !g_arrived && !line;
}

static void setup(){
    g_map=(volatile uintptr_t*)&mapSlot; mapSlot=(uintptr_t)mapObj;
    *(uintptr_t*)mapObj=(uintptr_t)&tagMap; g_vtUIMap=(uintptr_t)&tagMap;
    *(uintptr_t*)ent=(uintptr_t)&tagEnt; g_vtEntity=(uintptr_t)&tagEnt; g_ent=(uintptr_t)ent;
    g_fwGlobal=nullptr; g_gmGlobal=nullptr;              // keep the stand-in rider; not hardcore
    g_cpget=FakeGetter; g_showPath=FakeShow; g_strCtor=FakeStrCtor; g_strDtor=FakeStrDtor; g_sendEvent=FakeSend;
    g_hideName="HideFastTravelPath"; g_mapLine=true; g_brakeOn=false;
}
static void fresh(const char* name){                    // a new scenario: nothing placed, nothing seen
    printf("%s\n",name);
    g_haveMarker=false; g_goneT=0; clear_route(); g_cum.clear(); g_rev.clear(); g_prog=0;
    g_endOk=false; g_endA=g_endB=-1; g_arrived=false; g_dmMin=1e9f; g_wrongWay=g_showWrong=false;
    g_cur=nullptr; g_lastPlan=0; g_mkl={}; g_headOk=g_faceOk=false;
    place(recA,1800,2004); place(recB,1504,2250); place(recS,1100,1996);
    set_list({}); shownRec=0; shows=0; line=false; rider(1300,2000);
}
static void with_marker_a(uint64_t t){                  // the player has placed marker A; the map shows it
    set_list({&recA}); shownRec=(uintptr_t)&recA; refresh(t);
}

static void run(){
    const uint64_t GONE=MARKER_GONE_MS;

    fresh("marker removed on the map (the game keeps the record listed, as in the bug report)");
    with_marker_a(10000);
    MT_CHECK(routed_to(1800,2000) && line);
    shownRec=0; refresh(11000);                         // removed: the map stops showing it, the list still holds it
    MT_CHECK(!line);                                    // the line goes with the flag
    MT_CHECK(g_haveMarker && !g_route.empty());         // the destination is kept for MARKER_GONE_MS ...
    marker_step(11000+GONE-1);
    MT_CHECK(g_haveMarker && !g_route.empty());
    marker_step(11000+GONE);                            // ... and cleared once that has passed
    MT_CHECK(nothing_left());
    MT_CHECK(!marker_destination(14000) && nothing_left());   // riding: the record still listed is not taken up again
    int s0=shows; refresh(15000);
    MT_CHECK(nothing_left() && shows==s0);              // opening the map again draws nothing

    fresh("marker removed after arriving");
    with_marker_a(10000); g_arrived=true;
    shownRec=0; refresh(11000); marker_step(11000+GONE);
    MT_CHECK(nothing_left());

    fresh("marker removed; a leftover marker the map never showed stays listed");
    set_list({&recS,&recA}); shownRec=(uintptr_t)&recA; refresh(10000);
    MT_CHECK(routed_to(1800,2000));
    set_list({&recS}); shownRec=0; refresh(11000);
    marker_step(11000+GONE);
    MT_CHECK(nothing_left());
    MT_CHECK(!marker_destination(14000) && nothing_left());
    refresh(15000);
    MT_CHECK(nothing_left());

    fresh("marker removed while riding, before the map was opened (seen in the list only)");
    set_list({&recS,&recA});
    MT_CHECK(marker_destination(10000) && approx(g_mx,1800)); // the newest entry, as before
    set_list({&recS});                                  // A erased from the list
    MT_CHECK(marker_destination(10100) && g_haveMarker && approx(g_mx,1800));
    MT_CHECK(!marker_destination(10100+GONE) && nothing_left());
    MT_CHECK(!marker_destination(20000) && nothing_left()); // the leftover is never picked up

    fresh("arrival needs a marker: none for one missing for good, even before it is cleared");
    with_marker_a(10000);
    g_hookT=11000; rider(1800,2000); g_speed=8.f;       // riding on auto-follow at the destination
    shownRec=0; refresh(11000);
    g_mount=1; g_mountT=GetTickCount64();                // mounted (the stand-in has no game to ask)
    track_position(11000+GONE);
    MT_CHECK(!g_arrived);

    fresh("brief disappearance from the list changes nothing");
    with_marker_a(10000);
    { std::vector<int> r0=g_route; uint64_t p0=g_lastPlan; g_arrived=true;
      set_list({});                                     // the list is empty for a moment
      MT_CHECK(marker_destination(10500) && g_haveMarker);
      *(uintptr_t*)(mapObj+0x588)=0;                    // ... then unreadable
      marker_step(11000); marker_step(11900);
      MT_CHECK(g_haveMarker && g_route==r0 && g_arrived);
      set_list({&recA});                                // back after 1.4 s
      MT_CHECK(marker_destination(11950) && !g_goneT);
      marker_step(20000);
      MT_CHECK(g_haveMarker && approx(g_mx,1800) && g_route==r0 && g_lastPlan==p0 && g_arrived); }

    fresh("brief disappearance on the map changes nothing");
    with_marker_a(10000);
    { std::vector<int> r0=g_route; uint64_t p0=g_lastPlan;
      shownRec=0; refresh(10500);
      MT_CHECK(!line && g_haveMarker);
      shownRec=(uintptr_t)&recA; refresh(10500+GONE-1);
      MT_CHECK(line && !g_goneT && g_route==r0 && g_lastPlan==p0);
      marker_step(20000);
      MT_CHECK(routed_to(1800,2000) && line);
      g_arrived=true; shownRec=0; refresh(30000); shownRec=(uintptr_t)&recA; refresh(30500);
      MT_CHECK(g_arrived && g_haveMarker); }

    fresh("arrival during a brief disappearance");
    with_marker_a(10000);
    g_hookT=11000; rider(1800,2000); g_speed=8.f;
    shownRec=0; refresh(11000);
    g_mount=1; g_mountT=GetTickCount64();                // mounted
    track_position(11100);
    MT_CHECK(g_arrived);

    fresh("new marker placed after a removal");
    with_marker_a(10000);
    shownRec=0; refresh(11000); marker_step(11000+GONE);
    MT_CHECK(nothing_left());
    set_list({&recA,&recB}); shownRec=(uintptr_t)&recB; refresh(13000);
    MT_CHECK(routed_to(1500,2250) && line && !g_arrived);
    MT_CHECK(marker_destination(14000) && routed_to(1500,2250));   // riding: B, not A still listed
    refresh(15000);
    MT_CHECK(routed_to(1500,2250) && line);

    fresh("new marker placed while the old one is missing");
    with_marker_a(10000);
    shownRec=0; refresh(11000);
    set_list({&recB}); shownRec=(uintptr_t)&recB; refresh(11300);
    MT_CHECK(routed_to(1500,2250) && line && !g_goneT);
    marker_step(20000);
    MT_CHECK(routed_to(1500,2250) && line);

    fresh("marker moved after arriving");
    with_marker_a(10000); g_arrived=true;
    place(recA,1504,2250); refresh(11000);
    MT_CHECK(routed_to(1500,2250) && line && !g_arrived);

    fresh("between map refreshes the marker placed or moved last is used, as before");
    set_list({&recS,&recA});
    MT_CHECK(marker_destination(10000) && approx(g_mx,1800));
    place(recS,1150,1996);
    MT_CHECK(marker_destination(10100) && approx(g_mx,1150));

    fresh("the map's getter result decides (CpHook)");
    set_list({&recS,&recA}); shownRec=(uintptr_t)&recA;
    { alignas(16) uintptr_t out[2]={0,0};
      CpHook(mapObj,out);
      MT_CHECK(routed_to(1800,2000) && line);
      shownRec=0; CpHook(mapObj,out);
      MT_CHECK(!line && g_goneT);
      marker_step(GetTickCount64()+GONE);
      MT_CHECK(nothing_left()); }

    fresh("fast-travel hover: the route comes back when the game hides the hovered path");
    with_marker_a(10000);
    { int s0=shows; hover_end();
      MT_CHECK(line && shows==s0+1); }
    shownRec=0; refresh(11000);                         // missing: not put back
    { int s0=shows; hover_end(); MT_CHECK(!line && shows==s0); }
    marker_step(11000+GONE);                            // removed: not put back
    { int s0=shows; hover_end(); MT_CHECK(!line && shows==s0); }
    with_marker_a(20000); g_arrived=true;               // arrived: not put back
    { int s0=shows; hover_end(); MT_CHECK(!line && shows==s0); }
    g_arrived=false; g_mapLine=false;                   // MapRoute=0: not put back
    { int s0=shows; hover_end(); MT_CHECK(!line && shows==s0); }
    g_mapLine=true;
}

}  // namespace mt

// tools/markertest: <dir> is a writable folder for the test road; returns the number of failed checks.
extern "C" __declspec(dllexport) int KC_MarkerTest(const char* dir){
    strcpy_s(kc::g_dir,dir);
    if(!mt::write_graph(dir) || !g_graphs[0].load("markertest")){ printf("cannot write or load the test road in %s\n",dir); fflush(stdout); return -1; }
    g_ng=1; mt::fails=0;
    mt::setup(); mt::run();
    fflush(stdout);
    return mt::fails;
}
