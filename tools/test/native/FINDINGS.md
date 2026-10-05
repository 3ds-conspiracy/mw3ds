# Native test build: findings and how to fix the broken tests

Written 2026-10-04 on branch `native-test-harness`. See `README.md` for how to run things.

## Where it stands

The full suite (317 cases) runs on the PC build in about 6 minutes at 10 at a time. The last full run, before the two
test fixes below, passed 268 of 317 (`-IgnoreWarnings`). For comparison, the same cases take hours on the emulator.

What was checked against the emulator (Azahar, `-Fast`), case by case:

| Kind | Cases compared | Result |
|---|---|---|
| Plain cases that fail natively | `hw-dialogueflow`, `spell-cast-cap`, `hw-firebite`, `hw-scriptmisc`, `hh-formulas`, `il-courtesy`, `mq-ch6`, `mq-ch8`, `bug-container-state` | Same expectation results on both sides |
| Cases that differed | `openmw-spec-ai` | Native bug (float fusing), fixed |
| Cases that differed | `mech-enchant` | Not native: unseeded dice, passes and fails on the emulator too (pass, fail, pass) |
| Quest sweeps | `qrun-15`, `qrun-37`, `xrun-15`, `srun-19`, `qrun-1`, `qrun-3`, `xrun-1` | Same quests started and same quests failing on both sides |

Not compared yet: the rendering and water cases (`bug-d8-fog-distance`, `bug-interior-water`, `snd-rain-effects`), `ui-*`,
the `hw-script*` family, `mech-magic`, `mech-skills`, `hw-escort`, `hw-writ`, `mt-writ`, `tg-bragor`, `ht-muck`,
`hr-mudcrabs`, `journalmax`, `mg-mushrooms`. Anything in that list that fails natively is unverified.

## What we learned

### The native build

- **Loading, not the speed cap, was the slow part on the emulator.** The emulated ARM CPU parses the JSON game data at
  about 1 MB/s. `-Fast` (no frame cap, no vsync) only saved about 24 s on a 83 s case. (A bug in the first `-Fast` patch
  also left the cap on until it was fixed.)
- **Most of the game is portable.** About 30 of the source files have no 3DS dependency. Native only needed stubs for
  rendering, audio, fonts, the heap and threads.
- **Fused multiply-add changed results.** zig builds for the host CPU, which fuses `a*b+c`. The 3DS rounds every
  operation, so `(int)` truncations came out one apart (`fightterm` values). `-ffp-contract=off` fixed it.
- **The 3DS `rand()` generator made it worse.** Using devkitARM's newlib sequence dropped sweep batches from 10 quests
  started to 4. The PC's own `rand()` matched the emulator much better. Left off by default
  (`NATIVE_NEWLIB_RAND=1` turns it on). We do not know why.
- **Leftover SD state broke parallel runs.** A worker reusing its SD folder carried saves and settings into the next
  case, and some sweeps started 0 quests. Each run now starts from an empty folder.
- **Stubs can invent warnings.** The linear heap stub never gave memory back, which produced false "linear memory falls"
  monitors. Texture and memory warnings are emulator-only checks.

### The tests

- **The sweep batches (`qrun`, `srun`, `xrun`) are not judged by `RESULT:`.** They count quests started
  (`questrun.py --check`). A `topic X not offered` in them is a finding about that quest. `run-native-all.ps1` now
  judges them that way and compares with the emulator results in `build\<prefix>-results.txt`.
- **Those saved emulator results are stale.** They are from 2026-09-28; a fresh emulator run now gives fewer quests
  started (for example `qrun-15` 10 then, 7 now). Native matches the fresh emulator numbers exactly. So a gap against
  the file is not a native bug.
- **Some failures are stale tests, not game bugs.** Both fixes so far were test timing or placement, found with a
  throwaway probe case (see below).
- **Some cases roll unseeded dice** (`ROLL:-1`, no `SEED:`): `mech-enchant`. Dice in the game: leveled lists (spawns),
  script `Random`, combat, spell cast chance, resist, persuasion, enchant, repair, alchemy, awareness and idle rolls.
- **Outcomes depend on the seed.** Dialogue "no answer from the speaker" and quest starts changed with the random
  sequence. That looks fragile in the game and is worth a look, but we have not traced a single case to a roll.
- **`build\xrun.json` has no entry for `xrun-17` and `xrun-18`,** so their checks report a `KeyError`.

## Fixed so far

| Case | Cause | Fix |
|---|---|---|
| `openmw-spec-ai` | Native build only: fused multiply-add | `-ffp-contract=off` in `build.py` |
| `hw-firebite` | Test timing: a touch spell lands when the cast animation releases (about 0.9 s). The check ran at 0.9 s, before the hit visual existed. | Wait after `CAST` changed from `0.4` to `0.6` |
| `spell-firebite-damage` | Test placement: a guar 110 units away wanders out of the touch spell's reach before it lands. At 40 and 80 units it lands. | `PLACE:guar:110` changed to `:80` |

Both test fixes pass natively three times in a row and on the emulator twice.

## How to fix a broken test

The rule: find out whether it fails on both sides before changing anything.

1. **Run it natively** (about 1 s): `tools\test\run-test.ps1 <case> -Native -Data out\world -Start 'Seyda Neen'`.
2. **Run it on the emulator** (`-Fast`) once or twice. Results:
   - Fails on both, the same way: a real failure in the game or a stale test. Go to step 3.
   - Fails only natively: a native bug (float, stub, heap, timing). Fix the harness, not the test.
   - Passes and fails across runs on the same side: it rolls dice. Add `SEED:<n>` or `ROLL:<n>` to the case.
3. **Read the failing line** in `build\native\sd\3ds\mw3ds\log.txt` (for example `expect: FAIL vfxcount:ge:2 (got 1)`).
4. **Probe with a throwaway case.** Copy the case up to the interesting step, then add many short steps that print
   the value, for example `0.1 0 0 0 0 EXPECT:vfxcount:ge:99`, and read the `got N` values over time. Vary one
   thing at a time (distance, wait). Native does this in about a minute. Delete the probe case afterwards.
5. **Decide what the failure is:**
   - Timing (the check runs before the effect lands): move the wait.
   - Placement (a creature wanders, aim cone or range): move the actor closer or use a creature that stays.
   - Setup (the case relies on state it does not set): add the setup steps.
   - Real game behavior that is wrong: leave the test, file the bug.
6. **Change the test, then confirm** natively three times and on the emulator twice.
7. **Record it** in the table above, with the cause.

### Rules for what not to do

- Do not loosen an expectation just to turn a case green. Fix the timing or setup instead.
- Do not treat a native-only `monitor:` warning as a failure. It may be float drift. The emulator decides.
- Do not judge `qrun`, `srun` or `xrun` by `RESULT:`; use the quests-started count against a fresh emulator run.

## Proposed order for the remaining broken cases

1. **Quick group:** the 28 plain cases that fail natively. Same loop as above. `spell-cast-cap`, `hw-dialogueflow`,
   `hw-scriptmisc`, `hh-formulas` and `il-courtesy` are already known to fail on both sides, so start there.
2. **Seed the dice cases:** add `SEED:` to `mech-enchant` and any case that flips between runs, so results are repeatable.
3. **Rendering-dependent cases:** compare `bug-d8-fog-distance`, `bug-interior-water` and `snd-rain-effects` with the
   emulator. If native can never answer them (`refwater` returns `unknown check` natively), mark them emulator-only
   in the runner.
4. **Sweeps:** replace the stale `build\*-results.txt` baseline with a fresh emulator run, then list the quests that
   fail on both sides (for example `va_vampchild`, `hr_rescuesarethi`) as game or test items to look at.
5. **Drift guard:** generate `main_native.cpp` from `source/main.cpp` plus the exit patch, so the two cannot diverge.
6. **Regenerate `build\xrun.json`** so `xrun-17` and `xrun-18` can be checked.

## Open questions

- Why the newlib `rand()` made sweeps worse. Possibly the emulator's draws are not what a plain newlib LCG gives, or
  the sequence is consumed differently.
- Whether the sweep quests that fail on both sides are game bugs. Several started more quests in September, and ten
  commits have landed since. The commit log would narrow it down.
- How much of the remaining failure list is dice. Running one failing batch on the emulator three times would show it.
