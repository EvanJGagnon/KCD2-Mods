# xref.py -- rip-relative references to a target RVA inside .text
import sys, numpy as np
from whimg import Img
def xrefs(im, tgt):
    m=np.frombuffer(bytes(im.mem[:im.text[1]]),dtype=np.uint8); t0,t1=im.text; out=[]
    for a in range(4):
        n=(t1-t0-a)//4*4; v=m[t0+a:t0+a+n].view(np.int32).astype(np.int64); pos=t0+a+np.arange(0,n,4)
        out+=pos[(pos+4+v)==tgt].tolist()
    return sorted(out)
if __name__=='__main__':
    im=Img()
    for t in sys.argv[1:]:
        t=int(t,16); rs=xrefs(im,t); print(hex(t),[ (hex(r), [hex(x) for x in (im.func_of(r) or [])]) for r in rs][:12])
