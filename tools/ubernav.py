# ubernav.py -- reader for the game's ubernav.tmm road data (Data/Levels/<level>/level.pak).
import os
# Game folder: set KCD2_DIR to override.
GAME = os.environ.get("KCD2_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\KingdomComeDeliverance2")
import zipfile,struct,sys,collections
L=os.path.join(GAME,"Data","Levels","%s","level.pak")
SIG=b'\xff\xff\xff\xff\xff\xff\xff\x7f'
def load(level): return zipfile.ZipFile(L%level).read('ubernav.tmm')
def parse(b):
    recs=[]; o=b.find(SIG); gaps=collections.Counter()
    prev_end=None
    while o!=-1:
        a,c,k,n=struct.unpack_from('<HHII',b,o+8)
        p=o+20; pts=[]
        ok = 0<n<100000
        for i in range(n if ok else 0):
            idx,typ,x,y,z=struct.unpack_from('<IIfff',b,p)
            if idx!=i: ok=False; break
            pts.append((typ,x,y,z)); p+=20
        if ok:
            pre=b[o-21:o]
            f1,=struct.unpack_from('<f',pre,0); f2,=struct.unpack_from('<f',pre,5)
            if prev_end is not None: gaps[o-21-prev_end]+=1
            recs.append(dict(f1=f1,u8=pre[4],f2=f2,a=a,c=c,k=k,pts=pts,start=o-21)); prev_end=p
            o=b.find(SIG,p)
        else: o=b.find(SIG,o+1)
    return recs,gaps,prev_end
if __name__=='__main__':
    b=load(sys.argv[1]); recs,gaps,end=parse(b)
    print(sys.argv[1],'records',len(recs),'pts',sum(len(r['pts']) for r in recs),'end',end,'of',len(b),'first start',recs[0]['start'])
    print(' gaps',gaps.most_common(5))
    for k in ['f1','f2','u8','a','c','k']: print(' ',k,collections.Counter(r[k] for r in recs).most_common(6))
    print(' types',collections.Counter(p[0] for r in recs for p in r['pts']).most_common(6))
    xs=[p[1] for r in recs for p in r['pts']]; ys=[p[2] for r in recs for p in r['pts']]
    print(' bbox',min(xs),max(xs),min(ys),max(ys))
    print(' tail',b[end:end+80].hex())
