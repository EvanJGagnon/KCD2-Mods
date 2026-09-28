# sim_route.py -- replay the direction-aware planner offline on a road graph (mirrors autotravel.cpp).
# usage: sim_route.py <level> x y hx hy mx my [uturn]   (reads <level>.amg from AMG_DIR, default .)
import struct, sys, heapq, math, os
lvl = sys.argv[1]; x0, y0, hx, hy, mx, my = map(float, sys.argv[2:8]); U = float(sys.argv[8]) if len(sys.argv) > 8 else 400
d = open(os.path.join(os.environ.get('AMG_DIR', '.'), lvl + '.amg'), 'rb').read()
nn, ne = struct.unpack_from('<II', d, 4)
X = [struct.unpack_from('<ff', d, 12 + 8 * i) for i in range(nn)]
adj = [[] for _ in range(nn)]; o = 12 + 8 * nn
for i in range(ne):
    a, b, l, c = struct.unpack_from('<IIff', d, o + 16 * i); adj[a].append((b, c)); adj[b].append((a, c))
near = lambda px, py: min(range(nn), key=lambda i: (X[i][0] - px) ** 2 + (X[i][1] - py) ** 2)
hl = math.hypot(hx, hy); hx /= hl; hy /= hl

def unit(a, b):
    dx, dy = X[b][0] - X[a][0], X[b][1] - X[a][1]; l = math.hypot(dx, dy)
    return (dx / l, dy / l) if l > .01 else None

def rev(d1, d2): return d1 is not None and d2 is not None and d1[0] * d2[0] + d1[1] * d2[1] < -0.2

def route(s, t, pen):
    # Dijkstra over (node, came_from) states; the incoming direction at s is the heading.
    # Any reversal (turn sharper than ~100 deg between consecutive segments) costs `pen`.
    dist = {}; prev = {}; q = []
    for v, c in adj[s]:
        nd = c + (pen if rev((hx, hy), unit(s, v)) else 0)
        if nd < dist.get((v, s), 1e30): dist[(v, s)] = nd; prev[(v, s)] = None; heapq.heappush(q, (nd, v, s))
    best = None
    while q:
        dd, u, p = heapq.heappop(q)
        if dd > dist[(u, p)]: continue
        if u == t: best = (u, p); break
        din = unit(p, u)
        for v, c in adj[u]:
            nd = dd + c + (pen if rev(din, unit(u, v)) else 0)
            if nd < dist.get((v, u), 1e30): dist[(v, u)] = nd; prev[(v, u)] = (u, p); heapq.heappush(q, (nd, v, u))
    path = []; st = best
    while st: path.append(st[0]); st = prev[st]
    path.append(s); path.reverse()
    L = sum(math.hypot(X[path[i]][0] - X[path[i - 1]][0], X[path[i]][1] - X[path[i - 1]][1]) for i in range(1, len(path)))
    nrev = sum(1 for i in range(1, len(path) - 1) if rev(unit(path[i - 1], path[i]), unit(path[i], path[i + 1])))
    return path, L, nrev

s, t = near(x0, y0), near(mx, my)
for pen in (0, U):
    p, L, nrev = route(s, t, pen)
    print(f"penalty {pen:>7.0f}: len {L:6.0f} m, first step {'BACKWARDS' if rev((hx, hy), unit(p[0], p[1])) else 'forward'}, later reversals {nrev}")
