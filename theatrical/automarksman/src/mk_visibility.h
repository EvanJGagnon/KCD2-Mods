// mk_visibility.h -- pure and testable geometric visibility policy.
// Ray callback returns true only if the segment is clear of solid obstacles.
// No game dependencies, no target-position guessing and no projectile manipulation.
#pragma once
#include "mk_target.h"
#include <algorithm>
#include <cmath>
namespace mk {
// Geometric low-arc path. Each segment is ray-tested against actual collision geometry,
// not just the endpoint. A long arc is sampled more densely (up to 12 casts).
template<class RayVisible>
inline bool projectile_path_clear(const V3& origin, const Solution& shot,
                                  float speed, const Flight& flight, RayVisible&& clear) {
    if (!shot.ok || !std::isfinite(shot.tof) || !(shot.tof > 0.f && shot.tof < 15.f) ||
        !(speed > 5.f && speed < 301.f)) return false;
    V3 velocity = shot.dir * speed, prev = origin;
    int n = std::max(2, std::min(12, (int)std::ceil(speed * shot.tof / 6.f)));
    for (int i = 1; i <= n; ++i) {
        float t = shot.tof * i / n;
        V3 next = origin + flight.at(velocity, t);
        if (!std::isfinite(next.x + next.y + next.z) || !clear(prev, next)) return false;
        prev = next;
    }
    return true;
}
// Perform expensive physics tests only for the currently locked candidate
// and the best-centred alternatives. Never silently accept untested actors:
// the candidate budget is deliberately fail-closed.
template<class Check>
inline int visible_candidates(const Cand* in, const V3* positions, int n, const TargetPolicy& p,
                              uint32_t current, Cand* out, int budget, Check&& clear) {
    if (n < 0 || n > 256 || budget <= 0) return 0;
    int order[256], eligible = 0;
    for (int i=0;i<n;++i) {
        if (!p.allowed(in[i].kind)) continue;
        if (in[i].ang <= p.cone ||
            (in[i].id == current && in[i].ang <= p.cone * p.keep))
            order[eligible++]=i;
    }
    std::sort(order,order+eligible,[&](int a,int b) {
        if ((in[a].id == current) != (in[b].id == current)) return in[a].id == current;
        return p.score(in[a]) < p.score(in[b]);
    });
    int kept=0;
    for (int i=0;i<eligible && i<budget;++i) {
        int k=order[i];
        if (clear(in[k],positions[k])) out[kept++]=in[k];
    }
    return kept;
}
}  // namespace mk
