// mk_target.h -- Theatrical AutoMarksman: which target to follow, and when to let go ("soft lock").
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Plain C++17 (tested by tools/marksim.cpp).  The player stays in charge:
//  - a target is only taken when it is close to where Henry is looking (inside the cone);
//  - people come before animals; horses and dogs are skipped unless the .ini allows them;
//  - while the mouse moves, the mod does not steer, and a target the player has looked away from (outside
//    the cone) is dropped at once;
//  - once the mouse is still again, the choice starts afresh from where the player now looks: the old
//    target gets no preference, so the camera never swings back to it.
#pragma once
#include "mk_ballistics.h"
#include <cstdint>

namespace mk {

enum Kind { K_PERSON = 0, K_ANIMAL = 1, K_HORSE = 2, K_DOG = 3 };
inline const char* kind_name(int k) { return k == K_PERSON ? "person" : k == K_HORSE ? "horse" : k == K_DOG ? "dog" : "animal"; }

struct Cand { uint32_t id; float ang; float dist; int kind; };   // ang: radians between the view and the aim point

struct TargetPolicy {
    float cone = rad(10.f);       // a target is taken only this close to the centre of the view
    float keep = 1.3f;            // ...and kept while the mod steers if it stays within keep * cone
    bool people = true, animals = true, horses = false, dogs = false;
    float animalPenalty = 1.6f;   // an animal must be this much closer to the centre than a person to win
    float distWeight = 0.0005f;   // rad per metre: a nearer target wins a tie
    bool allowed(int kind) const {
        return kind == K_PERSON ? people : kind == K_HORSE ? horses : kind == K_DOG ? dogs : animals;
    }
    float score(const Cand& c) const { return c.ang * (c.kind == K_PERSON ? 1.f : animalPenalty) + distWeight * c.dist; }
};

// After the player has committed a shot, target selection cannot legitimately jump
// to a newly centred bystander while a hand cannon's fuse is burning.
// This is deliberately independent of Henry's weapon skill, which changes sway.
struct FiringTargetPin {
    bool active = false;
    bool cancelled = false;
    uint32_t id = 0;
    void state(bool firing, uint32_t current) {
        if (firing && !active) { active = true; cancelled = false; id = current; }
        else if (!firing) { active = false; cancelled = false; id = 0; }
    }
    void user_moved() { if (active) cancelled = true; }
    bool may_select() const { return !active; }  // even a cancelled shot must not lock onto a new person
    bool may_steer() const { return !active || (!cancelled && id != 0); }
};

struct Lock {
    enum Why { KEPT, NEW, SWITCHED, LOOKED_AWAY, LOST, NONE };
    uint32_t id = 0;
    bool userMoved = false;       // the mouse moved since the last choice: choose afresh next time
    // One decision.  handsOn: the player is moving the mouse right now.
    Why update(const Cand* c, int n, const TargetPolicy& p, bool handsOn) {
        const Cand* cur = nullptr;
        for (int i = 0; i < n; i++) if (c[i].id == id && id) cur = &c[i];
        if (handsOn) {
            userMoved = true;
            if (id && (!cur || cur->ang > p.cone)) { id = 0; return LOOKED_AWAY; }
            return id ? KEPT : NONE;
        }
        const Cand* best = nullptr;
        for (int i = 0; i < n; i++) {
            if (!p.allowed(c[i].kind) || c[i].ang > p.cone) continue;
            if (!best || p.score(c[i]) < p.score(*best)) best = &c[i];
        }
        bool fresh = userMoved; userMoved = false;
        if (!fresh && cur && p.allowed(cur->kind) && cur->ang <= p.keep * p.cone) {
            // Keep the current target unless another one is clearly nearer the centre of the view.
            if (!best || best->id == id || p.score(*best) > 0.5f * p.score(*cur)) return KEPT;
            id = best->id; return SWITCHED;
        }
        uint32_t old = id;
        id = best ? best->id : 0;
        if (!id) return old ? LOST : NONE;
        return id == old ? KEPT : (old ? SWITCHED : NEW);
    }
};

}  // namespace mk
