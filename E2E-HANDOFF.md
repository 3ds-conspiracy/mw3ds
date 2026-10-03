# E2E handoff: the OpenMW-parity commits against the emulator

Updated 2026-10-03 (evening). The five parity commits (45c0b0e dialogue, 16cdcbe world rules, e2f342f magic effects,
0e3670a weather, 50edcf1 activation) have been run in the emulator. The fixes that came out of that, and four reported
bugs, are committed (b6ba258..HEAD). Open items 1-5 of the earlier list were then fixed (b23622f..1b48c43);
`out\world` was rebuilt on 2026-10-03 14:54 with them and everything below was re-run on that build.

## How the tests are run

- One test: `tools\test\run-test.ps1 <name> -Data out\world -Start 'Seyda Neen' [-Wait secs] [-Emu <azahar folder>] [-App
  <3dsx>]`. Reads `tools\test\cases\<name>.txt`, prints a `RESULT:` line. `-Wait` defaults to 300 s; the spec tests need
  5400-7200. `-App` with a copy of the 3dsx lets the app be rebuilt while a test runs.
- Parallel (four Azahar copies: main, azahar-b, -c, -d): `tools\test\sweep-par.ps1 -ItemsFile <file>`, one `test:<name>` per
  line, results in `build\par\<name>.result` / `.log`. Its `-Wait 1500` is too short for openmw-spec-dialogue, -world,
  -effects: run those directly.
- Main-quest chain: `tools\test\run-suite.ps1 -Chained -Data out\world -Start 'Seyda Neen' -Wait 2400 [-Emu <azahar
  folder>] [-App <3dsx copy>] [-Logs <folder>]`. Without `-App` it runs the live `mw3ds.3dsx` (do not rebuild meanwhile).
  Each chapter's closing save is kept as `<logs>\chain-mq-chN.sav`; `-ResumeFrom <that save> -Tests mq-chN,...` restarts
  mid-chain. Chained, GIVE only tops up, SPELL skips a known spell, JOIN never lowers a rank, CHAIN pays a carried
  bounty, and GOD goes before the LOAD.
- Do not rebuild `out\world` while any emulator runs. Rebuild: `MW3DS_OUT=out/world python tools/convert/level.py --world`
  (about 12 minutes with the cell cache, ~4 GB of RAM). Under memory pressure `tex3ds` can die writing `map.t3x`
  (exit 0xC0000142) after the cells and skeletons are written but before `game.json`: check `game.json`'s time and rerun.
- App build: `powershell -File tools\build\make.ps1`.
- `SHOT` in a test writes `shot_step_NN.bmp` (400x480, top screen = top 240 rows) to the emulator's `sdmc/3ds/mw3ds`;
  `bug-dagger-hand` and `bug-andrano-tomb` use it.

## Results on the final build and data

| Test | Result |
|---|---|
| openmw-spec-weather | 1512 met, 0 failed |
| openmw-spec-world-hooks | 424 met, 0 failed |
| openmw-spec-world | 337 met, 0 failed |
| openmw-spec-effects | 346 met, 0 failed |
| openmw-spec-dialogue | 170 met, 0 failed, 3 SKIP (speaker disabled / elsewhere); 55 monitor warnings, 29 of them missing textures in ext_m1_m9 |
| openmw-spec-magic / -actormagic / -enchcast | 126 / 5 / 98 met, 0 failed |
| openmw-spec-combat / -enchant / -movement / -recharge | 440 / 120 / 160 / 160, all pass |
| mg-fakesoulgem, bug-andrano-tomb, bug-guars-ground, bug-dagger-hand | pass |
| bug-container-state (new) | 17 met, 0 failed (monitor: actors stuck fighting in Beshara) |
| main quest, chained | all 15 chapters 0 failed expectations; ch1-7, 10, 12, 14 pass; ch8, 9, 11, 13, 15 monitor warnings only |

"FAIL" with 0 failed expectations means monitor warnings only: actors stuck fighting, "no memory for
sound", missing cliff racer textures when Bitter Coast cells stream in low linear memory. Noise for rules, but the
linear-memory squeeze outdoors is real.

## Fixed (committed)

- Test harness: GOTO into a town lands on its street (converter `town_spawns`), not the cell's middle (Seyda Neen's is
  in the bay: rest was refused, DOORTO swam ~2000 units). SETDISP refreshes an open topic list. topiclisted SKIPs with
  no open conversation. FOLLOW calms the creature. Follower spec cases start at the Census door with distances clear
  of 800. New hook PUT:<container>:<item>.
- Drain / Fortify never take a stat below 0 (the rest is owed back); Humanoid / Creature mind effects only hit their
  kind.
- Containers take the player's items (Inventory page, L / R) with OpenMW's refusals; capacity exported (CNDT).
- Placed corpses (0 health in the record) lie dead; biped creatures get base_anim layered (skeletons animate) and
  strike with weapon groups when they have no Attack1.
- Casting: a second cast of the same kind restarted no animation (the spell fired instantly: "cast too fast"); a cast
  now holds the arms for its whole group (no recast / blow / draw / sheathe), and one cut short does not release.
- First person: the Camera bone's tilt is taken out of the rig (mid-chop the fist drew over the forearm); NPC rigid
  parts use the pose their skins were built from.
- From another session: deferred linear frees (GPU hang outdoors on hardware), README quick start.
- Main quest chained (was Open 1): test setup, not the game. ch5's GIVE doubled the Dwemer artifact ch4 hands over,
  ch7's SPELL:corprus gave corprus back after the cure (corprus greetings broke ch6, 8, 9, 12), ch9 ends mid-fight so
  the player died loading ch10 before its GOD, and a bounty from driven thefts held the arrest screen over ch12. mq-ch9
  also needed TOPIC:accompany_you before "name you Nerevarine".
- Missing female body parts (was Open 2): male parts of the same race fill them (OpenMW getBodyParts), first person,
  third person and NPCs (`firstperson.py`, `npc.py`). Argonian female forearm was the only gap in the data.
- Follow-through (was Open 3): the converter exports `<W><Kind>FM` / `FS` and `Attack*M` / `S`; a blow under 0.33
  strength (or a missed roll) cuts over at the hit to the small one, under 0.66 to the medium one, player and NPCs.
- Container stacks (was Open 4): contents carry condition, soul and charge (`ContentItem`), stacked as OpenMW's
  ContainerStore::stacks; put, take, barter and save keep them. New verb SETITEMCHARGE.
- Actors above the floor (was Open 5): gravity and the monitor measured only under the middle while a step stands
  actors on the highest floor under the feet; all three now use `collisionFootFloor`.

## Open

1. First-person camera-tilt change has no OpenMW reference in build/openmw-full (no rendering code there); judge it by
   eye on hardware. Also by eye: female Argonian forearm (player and NPCs), small / medium / large follow-throughs.
2. Linear memory outdoors: missing textures when exterior cells stream in (29 in ext_m1_m9 in the dialogue spec), "no
   memory for sound", memory falling on each visit to Zainab Camp, Ashkhan's Yurt (ch8).
3. Monitor noise left in the chain: actors stuck fighting (Ald Daedroth, Arena Pit, Akulakhan's Chamber, Beshara),
   player falling in the Arena Pit / Akulakhan's Chamber, under the floor in Tel Naga Upper Tower (ch13), dralcea arethi
   20 below the floor in Balmora.
4. Not done, outside the fixed bugs: script Drop from a container drops a plain item (script.cpp); barter prices ignore
   condition and soul; the converter does not skip not-playable body parts (BYDT flags & 2) as OpenMW does; driven
   thefts in mq-ch2 / ch3 still run up a bounty (paid at CHAIN).

## Rules of thumb that held up

- Read the failing step's log before calling a regression: most failures were test setup (bay landing, disabled
  speaker, test timing), but two were real (the same-group cast restart, Drain below 0).
- A crime is recorded only if a witness within `fAlarmRadius` has AI Alarm 100 or more: use SETALARM for bounty tests.
- Interior start cells spawn the player under the floor: keep `-Start 'Seyda Neen'` and let the test GOTO.
