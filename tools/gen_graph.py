# gen_graph.py: ubernav.tmm (auto-followable roads, flag u8=1) -> compact road graph <level>.amg
# usage: python gen_graph.py [output dir]
# format: 'AMG1', u32 nnodes, u32 nedges, nnodes*(f32 x,f32 y), nedges*(u32 a,u32 b,f32 len,f32 cost)
import struct,math,collections,sys,os
from ubernav import load,parse
def mult(f1): return 1.0 if f1>=1.0 else (1.4 if f1>=0.5 else 2.5)
def build(level,out,snap=2.0):
    recs,_,_=parse(load(level))
    pts=[];edges=[]
    for r in recs:
        if r['u8']!=1: continue
        b=len(pts)
        for p in r['pts']: pts.append((p[1],p[2]))
        for i in range(len(r['pts'])-1): edges.append((b+i,b+i+1,mult(r['f1'])))
    parent=list(range(len(pts)))
    def f(x):
        while parent[x]!=x: parent[x]=parent[parent[x]]; x=parent[x]
        return x
    g=collections.defaultdict(list)
    for i,(x,y) in enumerate(pts): g[(int(x//4),int(y//4))].append(i)
    for (gx,gy),ids in g.items():
        for dx in (-1,0,1):
            for dy in (-1,0,1):
                for j in g.get((gx+dx,gy+dy),()):
                    for i in ids:
                        if i<j and math.dist(pts[i],pts[j])<=snap: parent[f(i)]=f(j)
    rep={};acc=collections.defaultdict(lambda:[0,0,0])
    for i,p in enumerate(pts):
        a=acc[f(i)]; a[0]+=p[0];a[1]+=p[1];a[2]+=1
    ids={r:n for n,r in enumerate(acc)}
    nodes=[(a[0]/a[2],a[1]/a[2]) for a in acc.values()]
    E={}
    for a,b,m in edges:
        A,B=ids[f(a)],ids[f(b)]
        if A==B: continue
        L=math.dist(pts[a],pts[b]); k=(min(A,B),max(A,B))
        if k not in E or L*m<E[k][1]: E[k]=(L,L*m)
    with open(out,'wb') as fh:
        fh.write(b'AMG1'+struct.pack('<II',len(nodes),len(E)))
        for x,y in nodes: fh.write(struct.pack('<ff',x,y))
        for (a,b),(L,c) in E.items(): fh.write(struct.pack('<IIff',a,b,L,c))
    print(level,'nodes',len(nodes),'edges',len(E),'->',out)
if __name__=='__main__':
    out=sys.argv[1] if len(sys.argv)>1 else '.'
    for lv in ('trosecko','kutnohorsko','klaster'): build(lv,os.path.join(out,lv+'.amg'))
