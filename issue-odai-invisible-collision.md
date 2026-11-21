# Invisible collision reported on the Odai Plateau — no blocking geometry exists at the reported spot

**Component:** world collision / converted cell data (`ext_m5_m5`, "Odai Plateau")
**Severity:** player-visible (reported as unpassable "invisible walls", several places in one area)
**Found in:** play test on New 3DS hardware (`report.txt`, coordinates below; saves `save_040`–`save_043`, area of 2026-11)

## Symptoms

Player report (`report.txt`):

> I ran into a collsion at this locaiton with geometery that doesn't seem to be there
> x:-35298.683594, y: -35478.816406, z:1835.432861

plus "multiple other invisible walls" in the same area.

## Ground truth at the reported spot

All measurements use the engine's own collision code paths (`collisionRaycast` sweeps, `landTriangle`
classification, floor containment) against the data the card actually runs (`out/exp`, which is byte-identical
to the installed CIA's for this cell; `ext_m5_m5.cel` v10 + its `.json`).

| What | Result |
|---|---|
| Floor under the point | one land triangle, z 1824–1856, **slope 11.2°** — standable, not the 46° `kWalkSlope` limit |
| 16 escape rays, feet+70 (the standing check) | every direction either **clear to 4000** or first hit **1000–3600 units away** |
| Same rays at feet+56 and feet+12 (the engine's low body spheres, `player.cpp` `kBodySpheres`) | same or fewer hits; nothing closer than ~1000 |
| Ankle-height rays at the four save positions | no hits without matching drawn geometry |
| Actors within 3000 units (all four saves) | one living NPC (`durbul gro-rush`), 1396 units away — no corpse, no relocated body blocking there |

So at exactly the reported coordinates the shipped collision has **nothing near, in any direction** — the
reported numbers describe where the player *stood when reporting*, not where they were stopped.

## Where the stops actually are

The four saves in the cell (the player's last standing positions, pulled from the card) all sit **exactly
22 units** — the body sphere's radius — from collision, and every blocker is the **fort under construction**
(`ex_h_pcfort_*`, `ex_de_constr_06-2`, `ex_de_scaffold_01/02/03-*`):

| Save | Position (feet) |
|---|---|
| `save_040` | −36968.8, −38357.6, 2356.9 |
| `save_041` | −36697.0, −36659.6, 2071.7 |
| `save_042` | −36236.8, −36047.7, 1909.8 |
| `save_043` | −36131.4, −36026.9, 2109.4 |

None of these is phantom collision:

* every ray contact has a **drawn triangle at the contact point itself** (nearest-drawn distance 0.0,
  checked against all 43 hits within 400 units around the five positions);
* all 256 references that own object collision in the cell have drawn triangles inside their own boxes
  (scaffolds draw `atl_atlas_woodpoles.t3x`, fort pieces `atl_tx_hlaalu_atlas.t3x`) — there is **no
  collision-only reference** in `ext_m5_m5`;
* the cell's collision is not baked coarser than the drawn meshes (median triangle span 128 units, 90th
  percentile 290, only 12 triangles over 1000).

The one thing that genuinely behaves like an "invisible" stop: a fort wall piece whose **bottom sits ~54–63
units above the floor** where the player approaches. The visible gap under it reads as walkable, but the body
sphere centred at feet+84 (spanning 62–106) overlaps it and `bodyFits` refuses the standing spot, so the
engine refuses the gap at 22 units' distance. That matches Morrowind's own rules (a 54-unit gap is no passage
for a 128-unit body), but to a player it feels like a wall in thin air.

## Conclusion

Not a collision-data bug in `ext_m5_m5`: the stops are real, drawn fort-under-construction geometry, and the
worst-feeling one is the floating-wall-base gap. The reported coordinates themselves are unconstrained — which
is exactly why reports need the auto-filled context: the new in-game report feature (console `report`, pause
menu entry) captures coordinates, cell, warnings, the log tail and a screenshot at the moment of filing, so
"where" never relies on the player's transcription.

Suggested follow-ups:

* file the floating wall bases with the TR/asset conversion owner (the wall piece should extend to the ground
  or sit on a foundation: construction-site pieces placed on eroded plateau ground);
* ask the reporter (via a screenshot-based report) to file from the exact stopping spot rather than after
  walking back.

## Also found while investigating (separate bug)

`ext_m5_m5`'s stored spawn point is **under a cliff ledge with no floor for thousands of units**:
spawn `(-36965.3, -36598.0, 1682.1)`, nearest collision covering that XY is at z 2189 and 2560 — **above**
the spawn. A teleport there (`GOTO:Odai Plateau`) drops the player through everything to z −95 (monitor:
"the player is under every floor"). Verified offline (containment query, above) and by a native engine run
(`tools/test/cases/bug-odai-invisible-wall.txt`, PC build, data `out/exp`): the run also **proved the report
feature end to end** — `reports.json` entry + `report_01_save.json` + `report_01.bmp` all written.
