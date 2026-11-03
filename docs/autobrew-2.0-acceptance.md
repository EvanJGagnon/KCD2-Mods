# AutoBrew 2.0.0-test acceptance candidate

## PR reconciliation

Existing local branch `local/brew-2-install-20261102` includes:
1. `d91383f` (PR #10): weak/strong turns timed by native boil counters, not sand.
2. `315d57f` (PR #13): mixed-recipe queue, ingredient model and overlay.
3. `c3dcf8d`: existing compatibility correction accepts a pre-existing result-
   presenter detour only after verifying its unchanged body; leaves hook intact.

PR #13 already contains PR #10's exact commit. There is no need to cherry-pick
or rewrite the boiling engine, and no need to merge/close either PR before user
approval. PR #10 comes from an external fork; do not push to that contributor's
branch. Merge ordering is an eventual review choice, not a reason to lose fixes.

## Verified vs pending

Live startup reports `v2.0.0-test`, queue on, result presenter compatibility and
queue ready. Installed binary SHA256:
`3420c3172346eb1186a22c9fd97ee1935d2fa43b88f3871f14045167b8dc4b37`.

Current local MSVC release/development builds pass. All 48 existing queue model
checks pass; production development selftest resolves 34 entries. Independent
`check_alchemy_sigs.py` passes every engine/queue lookup against installed
WHGame.dll. These checks do NOT verify mixed-potion gameplay, overlay rendering,
quality, potion awards or the between-brew transition.

Review confirms the queue waits for an idle/clean table before reset/restock,
keeps native grading separate from popup suppression, and uses the existing
boil-timer engine. No speculative native source changes were made. The internal
`done` counter advances on completing the action plan: verify actual inventory
awards/quality instead of treating that counter as proof of native grading.

## Minimal acceptance and packaging

The two-different-potions x1 acceptance is in `theatrical_autobrew.txt`.
`python tools/package_brew_rc.py --tested-dll PATH --sha256 HASH` packages only
this mod, refuses overwrites and audits exact DLL/CRC/version/recipe/license
contents. The private archive sets Queue=1, QueueNotices=0: native literal
SendInfoText messages can tokenize incorrectly, so dynamic details remain in
menu/progress overlay and log. The source and installed configuration remain
unchanged. A real localized-notice solution is still a follow-up, not claimed
fixed by this configuration mitigation.

Original Autobrew is also installed locally. The prior no-co-run warning remains:
accepting its detour at startup is NOT gameplay compatibility proof. Do not alter
that unrelated mod automatically. No private install was performed this session.
Public release/merge/upload await approval AND successful mixed-queue acceptance.
