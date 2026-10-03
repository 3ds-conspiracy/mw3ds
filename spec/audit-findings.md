# Audit findings: our code vs OpenMW

From the read-only comparison agents (Sonnet, one per area, 2026-10-01; see `audit.md`). Each row is a claimed
behavioural difference. Status: **unverified** (agent's word), **confirmed** (checked in both sources),
**wrong** (agent mistaken), **fixed** (with the test), **differs** (kept on purpose, why).
**The 3DS UI is kept** (menus mostly on the bottom screen): only the rules menus decide are matched to OpenMW, never
layout, arrangement, numbering or styling; such rows are "differs (3DS UI, kept)".
Line numbers are as reported; OpenMW paths under `build/openmw-full/apps/openmw/` unless named.

## Script: items, sounds, animation (`mwscript/containerextensions.cpp`, `soundextensions.cpp`, `animationextensions.cpp`)

| # | Function | Difference | Conf | Status |
|---|---|---|---|---|
| I1 | AddItem | only `Player->AddItem` works; on NPCs, creatures, containers or a bare AddItem in their own script: nothing | high | fixed (`hw-scriptitems`) |
| I2 | RemoveItem | same: only the player | high | fixed (`hw-scriptitems`) |
| I3 | GetItemCount | any other object: 0; no object: the player's count (OpenMW 0) | high | fixed (`hw-scriptitems`) |
| I4 | Add/Remove/GetItemCount | gold_005/010/025/100 not folded into gold_001 | high | fixed (`hw-scriptitems`) |
| I5 | AddItem | unknown ids added as ghost entries; count 0 / negative accepted (OpenMW: error; 0 no-op; negative as uint16) | medium | fixed (`hw-scriptitems`) |
| I6 | Add/RemoveItem messages | OpenMW shows them only while in dialogue (ShowInDialogueMode_Only); ours always | medium | fixed (`hw-scriptitems`) |
| I7 | AddItem message | hard-coded text; OpenMW sNotifyMessage60 / 61 (count, name); no gold special case | low | fixed (`hw-scriptitems`) |
| I8 | RemoveItem message | ours always, even when nothing removed; OpenMW only if removed, sNotifyMessage62 / 63 with count | high | fixed (`hw-scriptitems`) |
| I9 | RemoveItem | removing a worn item doesn't refresh stats (constant effects stay) | low | unverified |
| I10 | AddItem | stacks onto an equipped stack (OpenMW: never onto an equipped item) | medium | unverified |
| I11 | AddItem | scripted items stack with one script instance (OpenMW adds them one by one) | low | unverified |
| I12 | AddItem | a leveled item list id isn't rolled | medium | fixed (rolled per item; not tested) |
| I13 | Equip | player only; no implicit add when not carried; marks potions / books "equipped" instead of using; no filled-gem preference | high | unverified |
| I14 | HasItemEquipped | always reads the player's equipment whatever the reference | high | unverified |
| I15 | HasSoulGem | always the player's inventory; any item type | medium | unverified |
| I16-18 | PlaySoundVP / PlaySound3DVP / PlayLoopSound3DVP | volume and pitch ignored | high | unverified |
| I19 | PlayLoopSound3D on the player | no-op | low | unverified |
| I20 | GetSoundPlaying | only loop sounds; one-shot sounds and Say never "playing" | medium | unverified |
| I21 | Say | subtitles always shown (OpenMW: only with the Subtitles setting, off by default) | medium | unverified |
| I22 | Say / SayDone | missing voice file = speaking 1 s + 0.06 s a character (OpenMW: done at once) | low | unverified |
| I23 | StreamMusic | does nothing | high | unverified |
| I24 | PlayGroup | actors only (not the player, doors, objects); mode ignored | medium | unverified |
| I25 | LoopGroup | loop count ignored: loops forever | high | unverified |

## Script: placement and controls (`transformationextensions.cpp`, `controlextensions.cpp`)

| # | Function | Difference | Conf | Status |
|---|---|---|---|---|
| T1 | Move | world axes; OpenMW the object's own axes | high | unverified |
| T2 | Move / MoveWorld | doesn't carry actors standing on the object (lifts); moves disabled objects | high | unverified |
| T3 | Move/SetPos/Rotate... | level-baked statics and activators not redrawn or re-collided when moved | medium | unverified |
| T4 | axis argument | any axis other than x / y means z (OpenMW: nothing, getters 0) | high | unverified |
| T5 | SetAngle | u / v / w and rotation order not handled | medium | unverified |
| T6-7 | SetAngle / GetAngle on the player | x / y ignored, read as 0 | medium | unverified |
| T8 | GetStartingAngle | returns the z start angle for every axis | high | unverified |
| T9 | GetStartingPos / Angle on the player | 0 | low | unverified |
| T10 | SetPos z on an actor | not clamped to the terrain outdoors | high | unverified |
| T11 | SetPos across a cell border | actor stays in its old cell | low | unverified |
| T12 | Rotate on the player | no-op (OpenMW turns the player about z) | high | unverified |
| T13 | RotateWorld | same as Rotate (local); actors tilt on x / y; player ignored | medium | unverified |
| T15 | SetAtStart | only z rotation restored; nothing for the player | medium | unverified |
| T16 | PositionCell on a non-player | OpenMW converts the angle to radians twice (Morrowind.exe's bug), ours once: NPCs face another way | high | unverified |
| T17 | PositionCell with an unknown interior, non-player | ours falls back to the exterior at x, y; OpenMW doesn't move it | high | unverified |
| T18 | PositionCell / Position actors | no snap to the ground | medium | unverified |
| T19 | Position on the player | stays in the current cell (OpenMW: the exterior cell at x, y, even from an interior) | high | unverified |
| T20 | Position on a non-player | angle converted once (as T16) | high | unverified |
| T21 | PlaceAtMe / PlaceAtPC | only converted actors: items, gold, containers, effects never appear | high | unverified |
| T22 | PlaceAtMe count 0 | places one | high | unverified |
| T23 | PlaceAtMe several | spread 60 units sideways (OpenMW: all at one spot) | medium | unverified |
| T24 | PlaceAtMe facing | turned 180 degrees from the reference (OpenMW: the reference's yaw) | medium | unverified |
| T25 | PlaceAtMe | no blocked-spot check (inside walls) | medium | unverified |
| T26 | PlaceAtMe | the player's cell, not the reference's; no scale | low | unverified |
| T30 | ForceSneak / ClearForceSneak | one global flag on the player whatever the reference | high | unverified |
| T31-32 | GetForceSneak, GetPlayer*Disabled getters | missing | medium | unverified |
| T33 | Enable / DisablePlayerLooking | missing | medium | unverified |
| T35 | GetDistance | 100000 for an unloaded object (OpenMW measures) | low | unverified |

## Script: AI packages (`aiextensions.cpp`, `mwmechanics/ai*.cpp`)

| # | Function | Difference | Conf | Status |
|---|---|---|---|---|
| A1 | AiWander duration | never counts down; GetAIPackageDone never 1 | high | unverified |
| A2 | all packages | a finished package stays current (OpenMW removes it; GetCurrentAIPackage -1) | medium | unverified |
| A3 | reset / repeat argument | ignored: patrols don't repeat | medium | unverified |
| A4 | AiWander idles | idle chances ignored | low | unverified |
| A5 | AiFollow | done by duration / arrival but keeps following | high | unverified |
| A6 | AiFollow | no activation gate (OpenMW waits until the target is within reach and seen); timer starts at once | medium | unverified |
| A7 | AiFollow distances | 180 / run past 600 (OpenMW about 256, run past 450, walk under 325) | low | unverified |
| A8 | AiFollow cell rules | completion by cell differs | low | unverified |
| A9 | AiEscort duration | not truncated to whole hours | low | unverified |
| A10 | AiEscortCell | unknown / empty cell still adds the package | medium | unverified |
| A11 | AiEscort | escortee follows the player through doors (OpenMW pauses in the wrong cell) | medium | unverified |
| A12 | AiEscort | walks without a target present; wait ranges differ | low | unverified |
| A13 | AiActivate | ours only sets a script flag once at 150 units; OpenMW activates any object (doors, containers, items) repeatedly within iMaxActivateDist | high | unverified |
| A14 | AiActivate | target lookup / disabled handling | low | unverified |
| A15 | AiTravel | no 7168-unit limit | medium | unverified |
| A16 | AiTravel done | instant within 64 units (OpenMW 2 s settle, paused while greeting) | low | unverified |
| A17 | giving a package | doesn't stop combat (OpenMW stack() stops it) | medium | unverified |
| A19 | GetCurrentAIPackage | an ESM wander reports -1 (OpenMW 0); dead not special-cased | medium | unverified |
| A20 | GetAIPackageDone | sticky flag; timing differs | medium | unverified |
| A21 | GetTarget | only "player" in combat; no NPC targets; no greeting case | high | unverified |
| A22 | GetDetected | only the player; 3-second heuristic | medium | unverified |
| A24 | StartCombat | target ignored: always attacks the player | high | unverified |
| A26 | StopCombat | doesn't stop the other side or allies | medium | unverified |
| A27 | Set/ModFight, Flee | clamped 0..100 (OpenMW not clamped) | medium | unverified |
| A28 | GetFight / GetFlee | ignore Frenzy / Calm / Demoralize / Rally | low | unverified |
| A29-31 | Mod/SetFight, Flee, Hello, Alarm | not written to the base record (survives respawn, all copies) | low | unverified |

## Script: stats (`statsextensions.cpp`)

| # | Function | Difference | Conf | Status |
|---|---|---|---|---|
| S1 | GetLevel / SetLevel | missing: always 0 ("unsupported function") | high | fixed |
| S2 | GetHealthGetRatio / Magicka / Fatigue | missing: 0 | high | fixed |
| S3 | Get/Set/Mod attributes and skills on NPCs / creatures | reads the record only; Set / Mod dropped; drain / fortify not seen | high | fixed |
| S4 | Get<Skill> on a creature | record array, not combat / magic / stealth | low | skipped (creature skills not modelled) |
| S5 | Set<Attr/Skill> on the player | truncated to int, capped at 100 (OpenMW float, no cap) | medium | differs (player stats whole numbers, cap 100) |
| S6 | Mod<Attr/Skill> on the player | unbounded hidden bonus (OpenMW clamps the base 0..100) | medium | fixed |
| S7 | ModHealth / Magicka / Fatigue | max can go negative; current capped at the new max | medium | fixed |
| S8 | SetHealth / Magicka / Fatigue | fortify / drain not kept on top | low | wrong (no separate fortify in our model) |
| S9 | ModCurrentHealth / Magicka | no floor at 0 | medium | fixed |
| S10 | ModCurrentFatigue | no knockdown at <= 0 | medium | fixed |
| S11 | GetMagicka | negative not returned as 0 | low | fixed |
| S12 | GetHealth on a non-actor | 100 (OpenMW: item health or 0) | medium | fixed |
| S13 | AddSpell / RemoveSpell | player only | high | fixed |
| S14 | GetSpell | always the player's spells | high | fixed |
| S15 | RemoveSpell | ability / disease effects and the selected spell not cleared | low | skipped |
| S16 | GetCommonDisease / GetBlightDisease | always the player | high | fixed |
| S17 | GetRace | always the player's race | high | fixed |
| S18 | GetDisposition | no weapon-drawn, Charm or crime terms | medium | wrong (Charm is a disposition bump already); weapon drawn not tracked |
| S19 | disposition on creatures | stored / returned (OpenMW: NPCs only) | low | fixed |
| S20 | SetPCCrimeLevel 0 | guards / witnesses not reset; PCCrimeLevel global stale | low | partly fixed (global kept up; no crime id) |
| S21 | ModPCCrimeLevel | missing | medium | fixed |
| S22 | PCRaiseRank | our own "You are now ..." message (OpenMW none) | medium | fixed |
| S23 | PCLowerRank at rank 0 | doesn't remove from the faction | high | fixed |
| S24 | Raise / LowerRank gaps | rank names with gaps | low | skipped |
| S25 | PCExpell message | hard-coded, every call (OpenMW sExpelledMessage, first time only) | low | fixed |
| S26 | RaiseRank / LowerRank on NPCs | do nothing | high | fixed |
| S27 | OnKnockout | consumed by the first script; only knock-outs, not knockdowns | medium | skipped (needs combat.cpp) |
| S28 | Resurrect | player ignored; NPC keeps looted inventory / state; works on the living | medium | partly fixed (inventory not reset) |
| S29 | stat calls with no reference | land on the player | low | skipped |

## Systemic: functions that ignore their reference

Many script functions were written for the player only and ignore the object they're called on, or read the
player whatever it is: I1-I3, I13-I15, S3, S13, S14, S16, S17, S26, A24 (and A21, A22 for targets). One fix:
each takes the reference (explicit `x->`, else the script's owner / the speaker) and acts on that actor's or
container's own state; the player is just one case.

## Dialogue: response filter (`mwdialogue/filter.cpp`, `selectwrapper.cpp`)

| # | Rule | Difference | Conf | Status |
|---|---|---|---|---|
| D1 | Info Refusal | when only disposition blocks a topic, OpenMW says the "Info Refusal" line; ours says nothing | high | unverified |
| D2 | topic list | a topic with only a refusal line is listed in OpenMW, hidden in ours | medium | unverified |
| D3 | Talked to PC | OpenMW freezes it at the conversation's start; ours turns 1 after the greeting | medium | unverified |
| D4 | Talked to PC without a greeting | ours sets it anyway | low | unverified |
| D5 | NotLocal | ours fails whenever the variable exists (OpenMW: not (local op value)) | medium | unverified |
| D6 | missing global | OpenMW ignores the condition; ours compares 0 | low | unverified |
| D7 | Weather condition indoors | OpenMW fails it in interiors; ours uses the outdoor weather | high | unverified |
| D8 | Faction Rank Difference (47) | sign reversed (OpenMW player minus NPC) | high | unverified |
| D9 | functions 0 / 1 | OpenMW: faction reaction lowest / highest; ours: the speaker's rank | medium | unverified |
| D10 | Rank Requirement (2) | modified vs base stats | low | unverified |
| D11 | Detected (48) | always 1 (OpenMW awareness check) | medium | unverified |
| D12 | Alarmed (49) | ours = in combat (OpenMW the alarmed flag) | low | unverified |
| D13 | Creature Target (65) | heuristic; never 2 | medium | unverified |
| D14 | Friend Hit (66) | not capped at 4 | medium | unverified |
| D15 | Should Attack (71) | fight >= 80 (OpenMW isAggressive) | medium | unverified |
| D16-17 | PC Werewolf Kills (73), Werewolf (72) | always 0 | low | unverified |
| D18 | Clothing Modifier (42) | counts weapons, shield, ammo, light (OpenMW slots 0-15) | medium | unverified |
| D19 | Health Percent (4 / 7) | float, not truncated int | low | unverified |
| D20 | PC Vampire (60) | global, not the effect | low | unverified |
| D21 | speaker rank with no faction field | OpenMW uses the speaker's own faction rank; ours (and build_game.py) reject | medium | unverified |
| D22 | rank on a factionless (FFFF) info | ignored by OpenMW, tested by ours | low | unverified |
| D23 | cell filter | OpenMW the player's cell; ours the speaker's | low | unverified |
| D24-25 | creature speakers | OpenMW skips disposition and race / class / faction / gender for creatures | low | unverified |
| D26 | Not* conditions | OpenMW ignores operator and value | low | unverified |
| D27 | Service Refusal / voice choice | OpenMW passes the service id / 0 as the choice | medium | unverified |
| D28 | PC faction FFFF | not special-cased in OpenMW | low | unverified |

## Script: misc (`miscextensions.cpp`)

| # | Function | Difference | Conf | Status |
|---|---|---|---|---|
| M1 | Activate | ours always runs the default activation; OpenMW only after a held-back player activation (or a container) | medium | unverified |
| M2 | OnActivate | consumed on read in OpenMW; ours cleared after the run | low | unverified |
| M3 | Lock with no level | ours 50; OpenMW keeps the old level, else 100 | high | unverified |
| M4 | Lock 0 / Unlock | no separate locked flag: Lock 0 unlocked, Unlock forgets the level; open door not closed by Lock | medium | unverified |
| M5 | GetLocked | level > 0, not the flag | medium | unverified |
| M6 | GetEffect | ignores numeric / 100+ effects; 0 for NPCs; misses magnitude-0 effects (Paralyze) | medium | unverified |
| M7 | GetSpellEffects | spells table only, by name; no abilities, potions, enchantments; 0 for NPCs | medium | unverified |
| M8 | GetAttacked | set on every player hit, never cleared, ignores NPC / spell attackers | medium | unverified |
| M9 | HitOnMe | player hits only; a fist hit clears it | low | unverified |
| M10 | Drop | drops at most what's held; 0 drops 1 (OpenMW: the full amount, created if missing; 0 no-op; actors only) | high | unverified |
| M11 | RemoveSoulGem | always the player's | medium | unverified |
| M12 | Cast | applies effects to the target directly (no caster, AI, visuals, reflect); Player->Cast casts instead of readying | high | unverified |
| M13 | PayFine | no confiscation to stolen_goods, no crime id reset, weapon not sheathed | high | unverified |
| M14 | PayFineThief | no crime id reset | low | unverified |
| M15 | GoToJail | immediate; 100 gold a day hard-coded; stolen items deleted not moved to the chest | low | unverified |
| M16 | HurtStandingActor | player only; runs in menus; negative still flashes | medium | unverified |
| M17 | GetStandingPC | box top within 30 units | low | unverified |
| M18 | StartScript with a reference | reference ignored | medium | unverified |
| M19 | StopScript | also stops a local script of that name | low | unverified |
| M20 | XBox | 1 (OpenMW 0): console wording, on purpose | high | differs |
| M21 | PlayBink | skip flag ignored; scripts don't wait; our own "end" text | medium | unverified |
| M22 | GetPCSleep | only while hours pass (OpenMW from opening the rest menu) | low | unverified |
| M23 | MenuMode | queued notifications count | low | unverified |
| M24 | Random | negative: 0, not an error; rand() | low | unverified |
| M25 | GetSecondsPassed | 0 in dialogue result scripts | low | unverified |
| M26 | missing explicit reference | silently 0 (OpenMW aborts the script) | low | unverified |

## Script: dialogue, menus, weather, cells (`dialogueextensions.cpp`, `guiextensions.cpp`, `skyextensions.cpp`, `cellextensions.cpp`)

| # | Function | Difference | Conf | Status |
|---|---|---|---|---|
| J1 | Journal | dedup on index not entry: an older stage repeats its text; same index after SetJournalIndex adds nothing | high | unverified |
| J2 | Journal | "journal updated" shown even with no entry / empty text for that index | high | unverified |
| J3 | Journal | no finished flag; Restart stages don't reopen | medium | unverified |
| J4 | Journal display | no date; newest first | low | unverified |
| J5 | Choice | drops a trailing unnumbered question; replaces instead of appending | medium | unverified |
| J6 | Goodbye | pending choices not cleared | low | unverified |
| J7 | ClearInfoActor | ours bars the response forever; OpenMW only removes the Topics journal line | high | unverified |
| J8 | Get/Set/ModReputation on an NPC | a delta from 0, not the record value | medium | unverified |
| J9 | ForceGreeting | refuses more cases; turns the player | low | unverified |
| J10 | AddTopic | no existence check | low | unverified |
| J11 | ShowMap | exact name only (OpenMW prefix over cells) | medium | unverified |
| J12 | ShowRestMenu in an owned bed | always refused (OpenMW: only if witnessed; bounty) | low | unverified |
| J13 | Enable*Menu (chargen) | queued; timing differs | low | unverified |
| J14 | MessageBox defines | only ^ActionActivate; ^PCName etc shown literally | high | unverified |
| J15 | MessageBox formats | %d prints fractions; %s with a number: garbage or a crash | medium | unverified |
| J16 | GetButtonPressed | not reset by a new box; boxes queued | low | unverified |
| J17 | ChangeWeather | permanent override: never changes again | high | unverified |
| J18 | GetCurrentWeather | the transition's target at once | low | unverified |
| J19 | ModRegion | no clamp; short list = defaults; no re-roll | medium | unverified |
| J20 | GetPCCell | wilderness region names (data check) | low | unverified |

## Dialogue: flow, topics, services (`dialoguemanagerimp.cpp`, `mwgui/dialogue.cpp`, `interpreter/defines.cpp`)

Overlaps with the filter report: D1/D2 = F6, D3/D4 = F2/F3, D6 = F10, D7 = F11, D8 = F12, D9 = F13, D18 = F14,
D21 = F18, D23 = F19, J5 = F25, J7 = F20.

| # | Rule | Difference | Conf | Status |
|---|---|---|---|---|
| F1 | no greeting matches | OpenMW: no conversation at all (closes); ours opens the topic screen and sets talked-to | high | unverified |
| F4 | order | OpenMW learns topics after the result script; ours before | medium | confirmed (mq-ch7, 9, 10: named by the speaker before the script gave them an answer); fixed, rerun pending |
| F5 | text for learning | OpenMW the raw text; ours after % substitution | low | fixed |
| F7 | topic list order | OpenMW alphabetical; ours file order (unless the converter sorts) | low | unverified |
| F8 | exhausted / specific topic colours | missing (OpenMW setting, off by default) | low | differs (3DS UI, kept) |
| F9 | dialogue globals | PCHasCrimeGold, PCHasGoldDiscount, CrimeGoldDiscount, CrimeGoldTurnIn, PCHasTurnIn never set: guard / bounty lines wrong | high | unverified |
| F21 | Topics index | choice answers not recorded | medium | unverified |
| F22 | Topics index | capped at 12, dedup by text | low | unverified |
| F23 | Service Refusal | choice = service type not passed (= D27) | medium | unverified |
| F24 | refusal / persuasion replies | no heading; persuasion not the last topic for a Choice | low | unverified |
| F26 | choices | "1. " prefix (OpenMW text only) | low | differs (3DS UI, kept) |
| F27 | persuasion | OpenMW temporary + permanent parts, temporary dropped at Goodbye; ours keeps the whole change | high | unverified |
| F28 | Bribe | gold taken on failure and not given to the NPC (OpenMW only on success, to the NPC) | high | unverified |
| F29 | % with no faction / bad rank | OpenMW prints "%"; ours blank | low | unverified |
| F30 | %NextPCRank at the top | OpenMW repeats the top rank; ours blank | medium | unverified |
| F31 | %PCNextRank alias | missing | low | unverified |
| F32 | %PCCrimeLevel | not substituted | high | unverified |
| F33 | %GlobalName, %Action* | not substituted | medium | unverified |
| F34 | ^ escapes | not substituted | medium | unverified |
| F35 | Companion Share | missing | medium | unverified |
| F36 | Spells service | keyed on the spell list, not the flag | low | unverified |
| F37 | after barter | no sBarterDialog5 line | low | unverified |
| F38 | ForceGreeting on a werewolf | not suppressed | low | unverified |

## Script language and runtime (`components/compiler`, `components/interpreter`, `mwscript/*`, `localscripts.cpp`)

| # | Rule | Difference | Conf | Status |
|---|---|---|---|---|
| L1 | int vs float | everything float: 7 / 2 = 3.5 (OpenMW 3 for int operands) | high | unverified |
| L2 | short / long locals | keep fractions (OpenMW truncates; short is 16-bit) | high | unverified |
| L3 | set ref.var | no truncation by the target's type | medium | unverified |
| L4 | runtime errors (division by 0 ...) | ours 0 and carries on; OpenMW stops that script for good | medium | unverified |
| L5 | =< => <> | ours the intuitive meaning; OpenMW == / == / < | medium | unverified |
| L6 | unary plus | our compiler drops the whole script | low | unverified |
| L7 | [ ] | not parentheses in ours | low | unverified |
| L8 | function arguments | ours one token only: no arithmetic, (..), -var, nested calls (OpenMW full expressions) | high | unverified |
| L9 | optional argument left out before == | ours swallows the operator: `if GetPCRank == 2` wrong | medium | unverified |
| L10 | set on an unknown name | ours makes a phantom global | low | unverified |
| L11 | set id->var | ours remote write | low | unverified |
| L12 | ScriptName.var | other global scripts' locals not found | medium | unverified |
| L13 | missing reference / no script | ours 0 and carries on (OpenMW error, script stops) | medium | unverified |
| L14 | id-> an item the player carries | not a target in ours | low | unverified |
| L15 | ids starting with digits, with - or ` | tokenise differently | low | unverified |
| L16 | scripts on items carried by NPCs / in containers | never run | high | unverified |
| L17 | a carried item's script | keeps running after the item is dropped / sold, beside the dropped copy's | medium | unverified |
| L18 | run order | globals before locals (OpenMW locals then globals) | low | unverified |
| L19 | start scripts | hard-coded Main, Startup, VampireCheck (no SSCR records) | low | unverified |
| L20 | StartScript's target | none: bare calls inside do nothing (= M18) | high | unverified |
| L21 | StopScript from a local script | stops itself (= M19) | low | unverified |
| L22 | while | stops at 1000 iterations | low | unverified |
| L23 | stray elseif | our compiler drops the script | low | unverified |
| L24 | MessageBox %d | decimals (= J15) | medium | unverified |
| L25 | bad arguments | ours runs with defaults (OpenMW refuses the script) | low | unverified |
| L26 | large ints | float precision past 16.7 million | low | unverified |
| L27 | GetSecondsPassed in dialogue scripts | 0 (= M25) | low | unverified |

## Vanilla impact noted while verifying

- I1-I3 (items on non-players): 409 call sites in Morrowind.esm scripts and dialogue results use AddItem / RemoveItem /
  GetItemCount on something other than the player (e.g. `aryonScript` puts the Hortator robe in a crate on his
  death; `boneScript` watches Jeanne's chest for the dwarf bone).

## Combat, current code (`mwmechanics/combat.cpp`, `mwclass/npc.cpp`, `creature.cpp`, `character.cpp`, Lua `omw/combat`)

| # | Rule | Difference | Conf | Status |
|---|---|---|---|---|
| C1 | blocking projectiles | the player's shield blocks NPC arrows / bolts / thrown (OpenMW never) | high | fixed |
| C2 | elemental shields vs ranged | shooter never burnt | high | fixed |
| C3 | enchanted ammo / thrown | When Strikes never fires | high | fixed |
| C4 | NPC / creature enchanted weapons | never apply to the player | high | fixed (NPC items keep no charge: always cast) |
| C5 | blocked blow | skips the enchantment (OpenMW applies it, zeroes damage) | high | fixed |
| C6 | killing blow | enchantment skipped: Soul Trap weapons don't trap on a one-hit kill | high | fixed |
| C7 | weapon wear | none on a miss / arrow into a wall | high | fixed |
| C8 | bow / thrown fatigue | ours charges a melee-style cost; OpenMW none | high | fixed |
| C9 | knockout | fatigue == 0 knocks out; NPC swing cost unclamped; Damage Fatigue alone never knocks out | medium | skipped (fatigue kept at 0 or above engine-wide) |
| C10 | knockout duration | fixed 3-3.5 s (OpenMW until fatigue recovers) | medium | skipped (timer-based knockout) |
| C11 | fists at 0 fatigue | health and fatigue together | medium | fixed |
| C12 | paralysed = knocked down | KO multiplier on paralysed targets | medium | fixed |
| C13 | hit recovery | doesn't stop movement / attacks / blocking | medium | skipped |
| C14 | melee LOS / vertical aim | none | medium | fixed (player melee) |
| C15 | melee reach geometry | different constants | low | skipped (low) |
| C16 | block gating | never mid-swing; unaware NPCs never block; no hit recovery gate | medium | fixed |
| C17 | block swing term / NPC blockers | fixed 0.5; NPC blockers no fatigue, no wear, always still | medium | partly fixed (NPC blockers tire; NPC swing term still 0.5) |
| C18 | NPC attack type | uniform (OpenMW weighted by damage) | medium | fixed |
| C19 | NPC swing strength | 0.3 + 0.7U (OpenMW U) | low | fixed |
| C20 | NPC Fortify / Drain attribute and skill | no effect in combat (magic.cpp lacks them for actors) | high | skipped (actor fortify / drain attribute not modelled) |
| C21 | NPC vs NPC fights | flat shortcut: no hit chance, armor, block, weapon, knockdown | high | skipped (needs NPC-vs-NPC through the player-hit code) |
| C22 | resist vs armor order (melee) | armor first | medium | fixed |
| C23 | crit and KO | don't stack | low | fixed |
| C24 | werewolves | no claws, no silver multiplier | high | skipped (no werewolves) |
| C25 | elemental shield vs NPC attacker | attacker's own resistance ignored | medium | fixed |
| C26 | disease from arrows | can infect (OpenMW melee only) | low | fixed |
| C27 | friendly hits | 4 forgiven, arrows too (OpenMW 3, arrows not counted) | medium | fixed |
| C28 | NPC archer aim | extra aim error before the roll | low | skipped (low) |
| C29 | weaponless bipedal creatures | attack list, not hand-to-hand | low | skipped (low) |
| C30 | GBAC mode | ours only (optional) | low | differs |
