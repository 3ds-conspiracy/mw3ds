# AGENTS.md

Notes for AI coding agents working in this repository. Read `README.md` first for what the project is and how a
person builds it; this file covers what an agent needs on top of that.

## The project

MW3DS is a from-scratch engine for *The Elder Scrolls III: Morrowind* on the **New Nintendo 3DS**, in C++ (gnu++20,
devkitARM, libctru, citro3d / citro2d). Python converters turn the user's own Morrowind install into 3DS-ready data.
Scope is `Morrowind.esm` only: no Tribunal, no Bloodmoon.

The engine follows **OpenMW's rules** where they are known. OpenMW is the reference, not the truth: where OpenMW looks
wrong (its changelog says so, or UESP disagrees), the spec page says which side we follow and why.

## Hard rules

- **Never commit Morrowind content or anything derived from it.** That means `out/`, `data/`, `out-vanilla/`, `mods/`,
  `.3dsx` / `.cia` / `.elf` builds, screenshots of game content, and converted assets. The one exception: cropped
  before / after shots for an issue go on the separate `issue-media` branch (never merged), as the `fix-issue`
  skill describes. Most of it is in `.gitignore`;
  check `git status` before every commit anyway, and never `git add -A` or `git add .`.
- **Never commit `.env`** or paste its paths into commits, issues or docs.
- **Do not copy OpenMW code** (GPLv3). Read it, then write the rule in our own words and code. The specs in `spec/`
  describe mechanics, they do not quote source.
- **`internal/` is private working notes** (git-ignored): handoffs, TODO, design notes. Read them for context when
  they exist, update them when asked, never commit them.
- **The UI stays as it is on the 3DS** (menus mostly on the bottom screen). OpenMW parity covers rules (which
  services show, refusals, message text, values), never layout, screen arrangement, numbering or button styling.
  Mark presentation differences "differs (3DS UI, kept)" in `spec/audit-findings.md`.
- **Commit or push only when the user asks.** Disk space is tight: ask before deleting anything that is not yours,
  and do not make large copies of `out/` or emulator folders.
- **No hidden debug button combos on the device.** Testing is automated, in the emulator or the native build.

## Layout

| Path | What is in it |
|---|---|
| `source/`, `include/` | The engine: world, cells and rendering, collision, combat, magic, dialogue, the MWScript interpreter, UI screens, saves, and the test driver (`testdrive.cpp`) |
| `source/formulas.cpp` | Game formulas pulled out of the UI code so tests can `EXPECT` them |
| `tools/convert/` | Converters (Python): Morrowind's ESM / NIF / BSA / sounds to the engine's formats. `level.py` runs them all |
| `tools/build/` | PowerShell: `setup.ps1`, `make.ps1`, `make-cia.ps1`, `build-cia.ps1`, `env.ps1` (loads `.env`), `addr2line.ps1`, CIA banner and RSF |
| `tools/test/` | Test runners, sweep generators, spec test generators, data checks |
| `tools/test/cases/` | The scripted test cases, one `.txt` per test |
| `tools/test/native/` | PC build of the engine for fast test runs (see its `README.md` and `FINDINGS.md`) |
| `spec/` | Formula and rule specs taken from OpenMW, `audit.md` (what has been compared with OpenMW, file by file), `findings/` |
| `E2E-HANDOFF.md` | Latest emulator results, what is fixed and what is open |

## Environment

- Windows. Scripts are PowerShell (`powershell -File ...`); converters and test tools are Python 3.10+
  (`requirements.txt`). Paths come from `.env` (see `.env.example`), loaded by `tools/build/env.ps1`; real
  environment variables win over it.
- The 3DS toolchain lives in MSYS2 with devkitPro's pacman repos. `tools/build/make.ps1` runs `make` inside it; do not
  call `make` from a plain shell.
- The emulator is Azahar (portable), at `MW3DS_EMU_DIR`. Extra copies for parallel runs are in `MW3DS_EMU_DIRS`. It
  must use Vulkan; window capture (PrintWindow) hangs it.
- On Python 3.13+ `audioop` comes from `audioop-lts`; run `tools\build\setup.ps1 -Check` if an import fails.

## Build

| What | Command |
|---|---|
| Engine only (writes `mw3ds.3dsx`) | `powershell -File tools\build\make.ps1` |
| Native PC build for tests | `python tools\test\native\build.py` (writes `build\native\mw3ds-native.exe`) |
| Convert the whole island | `$env:MW3DS_OUT='out/world'; python tools\convert\level.py --world` |
| Convert the Balmora area | `python tools\convert\level.py` (to `out\data`) |
| Full CIA from scratch | `powershell -File tools\build\build-cia.ps1` (`-SkipConvert` after engine-only changes) |
| CIA from existing data | `powershell -File tools\build\make-cia.ps1 -Data out\world` |
| Emulator fault address to source line | `powershell -File tools\build\addr2line.ps1 <pc>` |

- The island conversion takes about 12 minutes with the cell cache and about 4 GB of RAM; much longer when a
  converter whose code is part of the cache key changed (`npc.py`, `meshes.py`, ...).
- Under memory pressure `tex3ds` can die writing `map.t3x` (exit 0xC0000142) after the cells are written but before
  `game.json`. Check `game.json`'s timestamp and rerun.
- **Never rebuild `out\world` while an emulator or native test is running**: they all read it. Rebuilding the app is
  fine if the runs use an `-App` copy of the 3dsx.
- Compiler flags: `-fno-rtti -fno-exceptions`. No exceptions, no `dynamic_cast`, no `typeid`.

## Tests

A test is a text file in `tools/test/cases/`: one step per line, `secs moveX moveY lookX [jump] [TOKENS]`, tokens
joined by `+`, `%` for a space in a token. Tokens are parsed in `source/main.cpp` (`parseKeys`) and
`source/testdrive.cpp`. The driver acts as a player would (`WALKTO`, `ACTIVATE`, `DOORTO`, `KILL`, ...); setup tokens
set the world (`GOTO`, `GIVE`, `JOURNAL`, `SETSKILL`, `SEED`, ...); checks are `EXPECT:<kind>:...` with
`ge / gt / eq / ne / lt / le`, and `SNAP` records a value to compare against with `@`. `SHOT` writes a screenshot.

Run one case:

```powershell
tools\test\run-test.ps1 <name> -Native -Data out\world -Start 'Seyda Neen'      # seconds, on the PC
tools\test\run-test.ps1 <name> -Data out\world -Start 'Seyda Neen' [-Fast]      # minutes, in Azahar
```

Run many:

```powershell
powershell -NoProfile -File tools\test\run-native-all.ps1 -IgnoreWarnings [-Filter 'mq-*','bug-*']
tools\test\run-suite.ps1 -Chained -Data out\world -Start 'Seyda Neen' -Wait 2400   # main quest, chained
tools\test\sweep-par.ps1 -ItemsFile <file>                                          # several Azahar copies
```

- The verdict is the `RESULT: PASS|FAIL (...)` line. "FAIL" with 0 failed expectations means `monitor:` warnings only.
- The game log is `<sd>\3ds\mw3ds\log.txt` (Azahar: `<azahar>\user\sdmc\3ds\mw3ds\log.txt`; native:
  `build\native\sd...`). It carries `expect:`, `drive:`, `monitor:` and script error lines. The emulator's own log
  (`<azahar>\user\log\azahar_log.txt`) shows memory faults.
- Keep `-Start 'Seyda Neen'` and let the case `GOTO` where it needs to be: interior start cells spawn the player under
  the floor.
- **Native is the fast regression check; the emulator is the final word.** Native cannot see missing textures, linear
  memory, GPU hangs, or crashes from unmapped reads, and floor monitors can differ by a few units. A `monitor:`
  warning on only one side needs a look, not an automatic fail.
- **Screenshots:** `SHOT` saves both screens (400 x 480, top above bottom) on the emulator and natively. Native draws
  in software only the frames a `SHOT` saves (`tools/test/native/native_gpu.cpp`), so it takes seconds. Add
  `-Shots <folder>` to `run-test.ps1` for PNGs. Native text has the device's gold / tan colours; Azahar draws it
  white. See `tools/test/native/README.md` for what native drawing does not cover.
- Compare two native suite runs with `python tools\test\native-compare.py <before> <after>` (lists regressions).
- `tools/test/native/main_native.cpp` is a **copy** of `source/main.cpp` with an exit after the script ends. When
  `main.cpp` changes, re-copy it and re-apply that change (search for `native test build`).
- Generated cases: edit the generator, not the `.txt`. `openmw-spec-*` / `spec-*` come from
  `tools/test/openmw_specgen.py` and the `specgen_*.py` scripts, `mech-magic` from `mechmagic.py`, `qrun` / `srun` from
  `questrun.py`, `xrun` from `scriptrun.py`, `chain-*` from `questchain.py` and `run-suite.ps1 -Chained` (ignored by
  git).
- Sweep batches (`qrun`, `srun`, `xrun`) are judged by quests started (`questrun.py --check`), not by `RESULT:`.
- Unseeded dice (`ROLL:-1` with no `SEED:`) make a case pass and fail from run to run on both sides. Seed new cases.
- A crime is recorded only if a witness within `fAlarmRadius` has AI Alarm 100 or more: use `SETALARM` for bounty
  tests.

## How to work

- **GitHub issues:** use the `fix-issue` skill when it is available. Every fixed issue commits a test,
  `tools/test/cases/issue-<n>.txt`, that fails before the fix and passes after. Reproduce, debug and run the suite
  natively (fast); Azahar runs once at the end as the required final check. Before / after pictures go on the
  `issue-media` branch (`tools/test/issue_media.py`), never on `main`.
- **Shared working tree:** other sessions may have uncommitted work here. Never `git stash`, reset or check out files
  you did not change; take baselines from a git worktree; check for other sessions' runs before rebuilding
  `build\native` or `out\world`.
- **Read the failing step's log before calling a regression.** Most failures so far were test setup (landing spot,
  disabled speaker, timing); a few were real engine bugs.
- **A finding gets a test before it gets a fix**, and the test stays in the suite. If you wrote the test after the fix,
  say so: it shows the fixed engine passes, not that the test would have caught the bug.
- For a rule question, read OpenMW's game code (a sparse checkout lives in `build/openmw-full/`, git-ignored), update or
  add the `spec/` page and its row in `spec/audit.md`, then fix the engine.
- Be exact about what is verified: "passed in the emulator", "passed natively", "compiled only", or "needs a look on
  hardware" (3D rendering, sound and feel cannot be judged by the tests). The user plays each build on a real New 3DS.
- When several agents work at once: one agent per bug, never two editing the same file at the same time (take a lock
  per file before editing), one `BUILD` lock around `make.ps1` since `build/` and `mw3ds.3dsx` are shared, each
  emulator-using agent on its own Azahar copy and its own `-App` 3dsx copy, and no agent commits, touches git or
  rebuilds `out\world`. The main session reviews and commits each agent's work, then rebuilds the data and runs the
  end-to-end pass once at the end.

## Code style

- Match the file you are in. C++ uses tabs, Allman braces, `camelCase` functions, `kName` constants, and plain
  `//` comments that say what the rule is and where it comes from (GMST names, OpenMW's function) rather than what
  the next line does. Big files are normal here (`world.cpp`, `combat.cpp`, `session.cpp`); add to the system's file
  rather than splitting one rule across new ones.
- Every log line goes through `logf`. New engine-side checks that should show in test verdicts use `monitorOnce`
  (`source/log.cpp`).
- Python tools: standard library plus `numpy`, `pillow`, `miniaudio`. A module docstring with usage, like the existing
  ones.
- PowerShell tools: a comment header with usage lines, `param(...)`, dot-source `env.ps1`, settings through
  `Get-MwEnv`. Windows PowerShell 5.1: no `&&`, `??` or ternaries.
- Docs and comments in plain English, short sentences, colons rather than dashes.

## Commits

Plain prose, sentence case, no `feat:` / `fix:` prefixes. The subject says what changed in game terms, the body (when
needed) says why. Examples from the log:

```
Fix main quest chapters failing only when chained
Keep wear, soul and charge on container and merchant stacks
Pick small / medium / large follow-through by blow strength
```

End the message with the co-author line the harness gives you.
