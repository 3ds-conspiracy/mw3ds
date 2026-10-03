# E2E handoff: the OpenMW-parity commits against the emulator

Updated 2026-10-03. The five parity commits (45c0b0e dialogue, 16cdcbe world rules, e2f342f magic effects, 0e3670a
weather, 50edcf1 activation) have now been run in the emulator. The fixes that came out of that, and four reported
bugs, are committed (b6ba258..HEAD); `out\world` was rebuilt on 2026-10-03 07:26 with them.

## How the tests are run

- One test: `tools\run-test.ps1 <name> -Data out\world -Start 'Seyda Neen' [-Wait secs] [-Emu <azahar folder>] [-App
  <3dsx>]`. Reads `tools\tests\<name>.txt`, prints a `RESULT:` line. `-Wait` defaults to 300 s; the spec tests need
  5400-7200. `-App` with a copy of the 3dsx lets the app be rebuilt while a test runs.
- Parallel (four Azahar copies: main, azahar-b, -c, -d): `tools\sweep-par.ps1 -ItemsFile <file>`, one `test:<name>` per
  line, results in `build\par\<name>.result` / `.log`. Its `-Wait 1500` is too short for openmw-spec-dialogue, -world,
  -effects: run those directly.
- Main-quest chain: `tools\run-suite.ps1 -Chained -Data out\world -Start 'Seyda Neen' -Wait 2400` (main emulator,
  live `mw3ds.3dsx`: do not rebuild the app while it runs).
- Do not rebuild `out\world` while any emulator runs. Rebuild: `MW3DS_OUT=out/world python tools/level.py --world`
  (about 12 minutes with the cell cache, ~4 GB of RAM). Under memory pressure `tex3ds` can die writing `map.t3x`
  (exit 0xC0000142) after the cells and skeletons are written but before `game.json`: check `game.json`'s time and rerun.
- App build: `powershell -File tools\make.ps1`.
- `SHOT` in a test writes `shot_step_NN.bmp` (400x480, top screen = top 240 rows) to the emulator's `sdmc/3ds/mw3ds`;
  `bug-dagger-hand` and `bug-andrano-tomb` use it.

## Results on the final build and data

| Test | Result |
|---|---|
| openmw-spec-weather | 1512 met, 0 failed |
| openmw-spec-world-hooks | 424 met, 0 failed |
| openmw-spec-world | 337 met, 0 failed |
| openmw-spec-effects | 346 met, 0 failed |
| openmw-spec-dialogue | 170 met, 0 failed, 3 SKIP (speaker disabled / elsewhere) |
| openmw-spec-magic / -actormagic / -enchcast | 126 / 5 / 98 met, 0 failed |
| openmw-spec-combat / -enchant / -movement / -recharge | 440 / 120 / 160 / 160, all pass |
| mg-fakesoulgem, bug-andrano-tomb | pass |
| main quest, chained | ch2, 4, 7, 14 pass; ch1, 3, 11, 13, 15 have 0 failed expectations (monitors only); ch5, 6, 8, 9, 10, 12 fail chained |
| main quest, alone | ch5, 8, 10, 12 run alone: 0 failed expectations (ch10 12/12, ch12 7/7) |

"FAIL" with 0 failed expectations means monitor warnings only: actors 8-40 units off the floor, "no memory for
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

## Open

1. Main quest chained: chapters fail chained but pass alone, so state carried between chapters breaks them (topics
   "never learned", kills timing out in ch10 after earlier chapters). Compare the chained save going into ch5 / ch10
   with a fresh one.
2. Female Argonian has no forearm body part in the data (`b_n_argonian_f_forearm` missing): about 4 units of gap in
   first person. Fill missing arm slots from the male entry in `tools/firstperson.py` (`build()`, `third_person()`).
3. First-person follow-through groups end at "large follow stop"; OpenMW picks small / medium / large by charge.
4. Container contents hold only id and count: a filled soul gem is refused, condition / charge reset on put.
5. Andrano / Rothan tombs and Seyda Neen: actors 8-40 units above the floor (placement), monitor noise.
6. The first-person camera-tilt change has no OpenMW reference in build/openmw-full (no rendering code there); judge it
   by eye on hardware.

## Rules of thumb that held up

- Read the failing step's log before calling a regression: most failures were test setup (bay landing, disabled
  speaker, test timing), but two were real (the same-group cast restart, Drain below 0).
- A crime is recorded only if a witness within `fAlarmRadius` has AI Alarm 100 or more: use SETALARM for bounty tests.
- Interior start cells spawn the player under the floor: keep `-Start 'Seyda Neen'` and let the test GOTO.
