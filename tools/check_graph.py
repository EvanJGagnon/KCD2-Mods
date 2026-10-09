# check_graph.py -- independent check of data/<level>.amg against the game's road data (ubernav.tmm):
#   * every graph edge is two CONSECUTIVE points of ONE road that auto-follow rides (u8=1), and its cost
#     is that road's length x class multiplier;
#   * every graph node is a set of road points at the same place and height (a real junction or a road point);
#   * no road segment of the game data is missing, nothing else is in the graph;
#   * the main network holds (almost) everything; small separate pieces are listed.
# usage: python tools/check_graph.py [data dir]
import struct,math,sys,os,collections
sys.path.insert(0,os.path.dirname(__file__))
from ubernav import load,parse
def mult(f1): return 1.0 if f1>=1.0 else (1.4 if f1>=0.5 else 2.5)
D=sys.argv[1] if len(sys.argv)>1 else os.path.join(os.path.dirname(__file__),'..','data')
bad=0
for lv in ('trosecko','kutnohorsko','klaster'):
    d=open(os.path.join(D,lv+'.amg'),'rb').read(); assert d[:4]==b'AMG1'
    n,e=struct.unpack_from('<II',d,4); o=12
    N=[struct.unpack_from('<ff',d,o+8*i) for i in range(n)]; o+=8*n
    E=[struct.unpack_from('<IIff',d,o+16*i) for i in range(e)]
    recs,_,_=parse(load(lv))
    grid=collections.defaultdict(list)
    for i,(x,y) in enumerate(N): grid[(int(x//2),int(y//2))].append(i)
    def node_of(x,y):
        hits=[i for dx in (-1,0,1) for dy in (-1,0,1) for i in grid.get((int(x//2)+dx,int(y//2)+dy),()) if math.hypot(N[i][0]-x,N[i][1]-y)<=0.1]
        return hits
    want={}; members=collections.defaultdict(list); unmatched=0; ambiguous=0
    for ri,r in enumerate(recs):
        if r['u8']!=1: continue
        ids=[]
        for p in r['pts']:
            h=node_of(p[1],p[2])
            if len(h)!=1: unmatched+= len(h)==0; ambiguous+= len(h)>1
            ids.append(h[0] if h else -1); 
            if h: members[h[0]].append(p[3])
        for (a,pa),(b,pb) in zip(zip(ids,r['pts']),zip(ids[1:],r['pts'][1:])):
            if a<0 or b<0 or a==b: continue
            L=math.dist(pa[1:3],pb[1:3]); k=(min(a,b),max(a,b))
            if k not in want or L*mult(r['f1'])<want[k][1]: want[k]=(L,L*mult(r['f1']))
    have={(min(a,b),max(a,b)):(L,c) for a,b,L,c in E}
    extra=[k for k in have if k not in want]; missing=[k for k in want if k not in have]
    costbad=[k for k in have if k in want and (abs(have[k][0]-want[k][0])>0.01 or abs(have[k][1]-want[k][1])>0.01)]
    zspread=[max(z)-min(z) for z in members.values()]
    lonely=[i for i in range(n) if i not in members]
    # components
    adj=collections.defaultdict(list)
    for a,b in have: adj[a].append(b); adj[b].append(a)
    comp=[-1]*n; sizes=[]
    for s in range(n):
        if comp[s]>=0: continue
        st=[s]; comp[s]=len(sizes); k=0
        while st:
            u=st.pop(); k+=1
            for v in adj[u]:
                if comp[v]<0: comp[v]=comp[s]; st.append(v)
        sizes.append(k)
    sizes.sort(reverse=True)
    ok = not extra and not missing and not costbad and unmatched==0 and ambiguous==0 and not lonely and max(zspread)<=1.0
    bad+= not ok
    print(f"{lv}: {n} nodes, {e} edges | edges not from one auto-follow road: {len(extra)} | game road segments missing: {len(missing)} | wrong length/cost: {len(costbad)}")
    print(f"   road points without exactly one node: {unmatched+ambiguous} | nodes without a road point: {len(lonely)} | largest height spread inside a node: {max(zspread):.2f} m")
    print(f"   networks: main {sizes[0]} nodes ({100*sizes[0]/n:.1f}%), {len(sizes)-1} small separate pieces (largest {sizes[1] if len(sizes)>1 else 0} nodes) -> {'OK' if ok else 'PROBLEM'}")
sys.exit(1 if bad else 0)
