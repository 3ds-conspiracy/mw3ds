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
| A24 | StartCombat | target ignored: always attacks the player. Now another actor as the target is fought: it walks up to that actor and swings, the target fights back, a death ends it (no block, crit or spells between actors; not saved) | high | fixed (`bug-npc-startcombat`) |
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

## Movement and physics (`player.cpp`, `magic.cpp`; OpenMW `mwphysics/movementsolver.cpp`, `mwmechanics/character.cpp`)

| # | Rule | Difference | Conf | Status |
|---|---|---|---|---|
| P1 | Levitate | switched on the free fly mode: fixed 300 units/s, no collision (through walls) | high | fixed (`bug-levitate-noclip`) |
| P2 | Jump | moving jumps went straight up at full speed; no 45-degree take-off, no inertia in the air | high | fixed (`bug-jump-momentum`) |
| P3 | Air control | the pad steered the whole run speed in the air; OpenMW `fJumpMoveBase + fJumpMoveMult x Acrobatics / 100` | high | fixed (`bug-jump-momentum`) |
| P4 | Levitate in water | swimming won: a levitating player couldn't rise out of the water | high | fixed (`bug-levitate-water`) |
| P5 | Slow Fall | OpenMW scales fall and inertia by 1 - 0.005 x magnitude per step; ours quarters gravity, caps at 200 | medium | unverified |

| B1 | Intervention from an interior | used the last spot outdoors, which saves dropped: after LOAD inside, the wrong town. Now the exterior spot of the nearest way out is found door by door (OpenMW getClosestMarker), and the last spot outdoors is saved | high | fixed (`bug-intervention-load`) |
| B1b | Intervention from a chain of interiors | the door walk saw only cells whose objects were in memory, so two interiors deep it fell back to the last spot outdoors (wrong town). Each cell's doors are now read from the data when needed | high | fixed (`bug-almsivi-interior`) |
| B2 | Cure Common / Blight Disease on an actor | had no effect on NPCs / creatures (the Gnisis kwama queen stayed blighted); now every disease of that type leaves its spell list | high | fixed (`bug-cure-actor-disease`) |
| B3 | Items lying on an activator | the crosshair stopped at the activator's box (darts on a bed, Llethri Guard Quarters); OpenMW picks by collision shapes. Now an item within the activator's box on the ray wins, as for containers | medium | fixed (`bug-pick-on-activator`) |
| B4 | Locked door message; Open Lock vs lock level | OpenMW's locked door gives only the LockedDoor sound (empty FailedAction message); Open succeeds when magnitude >= lock level and Ondusi's Open Door is fixed 50, so a lock-60 door stays shut. Engine already matches: not a bug | low | matches OpenMW |

| B5 | Followers outdoors stall behind rocks and ledges | walked straight at the player and stopped for good at a rock or a ledge (Itermerel near -2458,-31767, Madura Seran near -86702,118217, Tarvyn Faren near 17176,-67410); OpenMW steers with a navigation mesh. Now a held-up follower walks the player's footsteps and may step down a ledge | high | fixed (`bug-follow-rock-walk`) |
| C1 | Ashlander yurt doors (Mila-Nipal, Manat's Yurt; Bensiberib) "inside the yurt's collision" | the door mesh sits about 330 units from the door reference's own position (the model is offset inside the tent); DOORTO walks at the reference position and climbs the tent. Standing at the door mesh the crosshair finds it and the door opens. Engine collision and picking are right; the driver must aim at the door's box, not its origin | medium | open (driver: testdrive.cpp DOORTO; case `bug-yurt-doorto` fails until then) |
| C2 | Arkngthand's outer doors can't be targeted | the two statics arkn_door00 / arkn_door01 are a vault door that the crank `in_dwrv_crank_arkn` (script Arkn_doors) swings open for 14 s; the activator doors lie 250 units behind it. Engine matches the game: turn the crank, then the doors can be reached | low | matches OpenMW (`bug-arkngthand-crank`) |
| C3 | Telvayn Ancestral Tomb exit spot "covered by a rock" | walking from the exit spot to the door works (the spot lies on the entrance mesh's base, terrain 1365). Only a levitation landing on the entrance's roof gets stuck, as a player's would | low | not an engine bug |
| C4 | Sword blows do 1 damage to Gordol and Bolvyn Venim (hr run) | the player's silver longsword was at condition 121 of its maximum: OpenMW scales a blow by condition / maximum, then armor (rating 79 / 102 here) takes at least a quarter off and a blow does at least 1. GOD:2 in the harness does not skip weapon wear (OpenMW's god mode does). Engine matches OpenMW; repair or give a fresh weapon in the test | low | matches OpenMW (no change) |
| H1 | Daedric ruins' exterior door exit spot inside solid stone (Ashalmimilkala, Maelkashishi) | ex_dae_ruin_01 has a root node offset of -1018 that the converter applied; the game takes a placed object's position from its reference and ignores the root's (spec/world-rules.md section 9). The tower base sank 1000 units, its cross-shaped block stood where the entry hall is and the exit spot lay inside it; at Maelkashishi the stairs ended at a pit. Now the root's rotation, translation and scale are dropped for placed objects (also changes the anvil, Mehrunes Dagon and Malacath statues, imperial dragon statue and a few more). Needs a reconvert of every cell using these models | high | fixed (`bug-ruin-exit-ashalmimilkala`, `bug-ruin-exit-maelkashishi`) |
| D1 | Answer to a Choice | the first fitting info answers, Choice condition or not, as in OpenMW and the original game: Milyn Faram's "Odirniran" gives "happier here" instead of the scroll reward after "Listen". UESP lists it as a bug of the original game (fixed only by the Morrowind Patch Project mod), so we keep it (spec/dialogue.md R19b) | low | matches OpenMW (`bug-choice-answer-first` guards it) |
| T1 | Test driver: a guard's crime greeting during a walk | the driver left the dialogue open or logged "stuck"; in a LEGIT run it now pays the fine (first choice, if the gold is there), closes the talk and goes on (test tooling, no game rule) | medium | fixed (`bug-guard-greet-walk`) |
| O1 | Max magicka drifted (150 read 121 later, uber hr) | Fortify Maximum Magicka added 0.1 x magnitude x Intelligence when it began and took off the same sum at the Intelligence of the moment it ended, so a Fortify Intelligence that began or ended in between left the maximum lower or higher for good. The effect now remembers the points it added and takes exactly those off. Still differs from OpenMW: our maximum is computed when stats are recomputed, not live, so a Fortify Intelligence alone does not move it until the next recompute | medium | fixed (`bug-max-magicka-drift`) |
| O2 | Test setup GIVE equipped a lockpick or probe (uber mq) | OpenMW never equips on add; the tool replaced the weapon in hand. GIVE now only adds lockpicks and probes (weapons, armor, clothes and lights are still worn, as the harness documents); EQUIP, USELOCKPICK and USEPROBE hold one (`security`, `issue-10` now EQUIP it) | low | fixed (`bug-give-pick-keeps-weapon`) |
| O3 | First TOPIC after Nibani Maesa's multi-topic answer unanswered (uber mq) | her answer ends in a Continue choice; with a choice open the topic list is disabled in OpenMW and ours. PERSUADE:0 only "fixed" it because a persuasion clears the choices. Driver must press CHOICE:1. Not an engine bug | low | matches OpenMW (`bug-topic-after-multilearn` documents it) |
| O4 | Madura Seran's local script ran while she was outside and the player inside (uber il) | not reproduced: scriptsRun skips references whose cell is not live, and entering an interior unloads every other cell; followers within 800 units come along, others stay in an unloaded cell | low | matches OpenMW |
| M1 | Gnisis caravaner fell off the strider platform when the cell holding its collision was freed or not yet loaded (uber bug 11) | actors start to fall only with a loaded neighbour within 2048 units, or a drop under 400 (`world-rules.md` section 10); the "no floor" monitor still shows while the cell is missing | medium | fixed (`bug-caravaner-platform`) |
| M2 | Follower stuck on the Gnisis strider platform while the player is below the cliff (uber bugs 41 / 87) | the grid route down the ramp was dropped for ending 2000 units from the player; kept now when the straight way failed and its last point sees the player. Seyda Neen's platform: follower walks down (not reproduced) | medium | fixed (`bug-follow-strider-platform`) |
| M3 | Cells freed and loaded again at every crossing of a cell border (uber bug 39) | the current cell changes only 512 units past its edge; new monitor for a cell freed and loaded again within 3 s | medium | fixed (`bug-cell-border-reload`) |
| M4 | Inner door shut again on arrival, East Empire Company hall (uber bug 45) | not reproduced: the walk to Canctunian Ponius opens the door and talks (the driver opened it twice) | low | not reproduced |
| N1 | Player wedged in the air between two rocks, Ascadian Isles (uber bug 68) | a fall step onto a face steeper than 60 degrees (not a floor) was reverted by the never-through-a-surface check for ever; now the body is pushed off the face sideways (`movement.md`, Steep faces) | medium | fixed (`bug-fall-steep-ridge`) |
| N2 | FLYTO's "can't climb" check never fired; levitation burned out under ceilings (uber bugs 47 / 53 / 12 / 25 symptoms) | driver bug: `TestDriver::progressAt` had 2 floats but z was read and compared as `progressAt[2]` (the next member), so a flight stuck under an overhang waited until the potion ran out and fell; fixed in `include/testdrive.h` (3 floats). The landings under the walkway are a floor missing in the collision (Cornerclub upper door: no floor between z 244 and 1368 at the door's z 568) | medium | fixed (driver); landing floor open |
| N3 | Flying into Red Mountain / Ghostfence "under every floor" (uber bugs 12 / 25) | not reproduced as passing into rock: the flight is stopped by an overhang with floor below it; the monitor text came from a fall after levitation ran out (see N2) | low | not reproduced |
| N4 | Asha-Ahhe Egg Mine slope (uber bug 54) | reproduced: the walk to 280,1476,137 is stuck at 180 1129 -324 (slope not climbable, path grid in two pieces); the stuck-recovery jumps under BOOST:100 end outside the cave ("under every floor at 22 952 -127"). Collision / path grid, not player code | medium | open (group H) |
| N5 | Second FLYTO right after a first fails "not levitating" (uber bug 93) | not reproduced (two legs and a potion between them pass without a SLEEP) | low | not reproduced |
| N6 | SLEEP steps under 1 s swallow the sleep (uber bug 94) | driver / step runner (`main.cpp`): a rest needs 4 UI frames per hour, a step shorter than that ends first and the next `SLEEP` token restarts the rest (6 h + 6 h gave 6 h). Engine rest code is right | low | open (group P) |
| S1 | Global script started on an object whose cell goes out of memory (uber bug 92: the uber-hh / uber-hr crash) | the object's slot is freed and reused, but the script kept the number: saving read `cells[-1]` (strlen on a stray pointer in `scriptState`), and the script could act on another object. Now eviction keeps the target as cell file + index and the script binds it again when it next runs (as a loaded save does); saving never reads a freed slot's cell. OpenMW keeps the object through its cell store | high | fixed (`bug-script-target-evicted`) |
| R2a | Wulf (Ghostgate, Tower of Dusk) can not be talked to (uber bug IC30) | not an engine bug: the Startup script disables him and Vivec's B8_MeetVivec reply enables him (as in the game); the questline has to meet Vivec first. With him enabled, ACTIVATE reaches him in bed (bug-talk-sleeping) | low | not a bug (bug-talk-sleeping) |
| R2b | Hatch in a ceiling (Sorkvild's, Dagon Fel; Arena Hidden Area trapdoor) never under the crosshair | the hatch's box reaches down round the player, so the eye is inside it and no pick was made. Looking steeply up at a door whose origin is above the eye, from inside its box, now picks it (OpenMW picks by the mesh, which is overhead). Driver: a ceiling door within reach of the eye is not "another floor" | low | fixed (bug-hatch-overhead) |
| R2c | DOORTO to Daedric ruin doors (oval ones) and Nchuleftingth's door stops short (uber bug 106) | driver only: the door's mesh stops the walk at its face, 130 units from the origin; a door whose box is within 40 units of the feet counts as reached | low | fixed (bug-doorto-ruin-ashal / -yasam / -nchul) |

| R1a | Icarian Flight HOPTO ends hanging in the air for good (uber bug 105) | player code: a refused step in the air (the never-through-a-surface check) kept the jump's inertia, so the body pushed into a cliff face every frame. A refused step in the air now clears the inertia (OpenMW clips the velocity at what it hits) | medium | fixed (zz-open-icarian-hop-hang) |
| R1b | Shallit upper tunnel / Rels Tenim unreachable (uber bug 46) | not a collision fault: levitating straight up meets the tube's belly; up the rocks at x 300-700 reaches the tunnel, whose grid walks on to Tenim's end | low | not an engine bug (bug-shallit-upper-tunnel passes) |
| R1c | Ashurnibibi, Shrine pocket "walled in" (IL_RescueKnight) | not a collision fault: the cave is flooded and the way on is a dive under an arch; the WALKTO driver does not dive (a flood over the player's moves reaches the hall at 2141,-1612) | medium | driver (open) |
| R1d | Omaren forge door unreachable (IC29) | not a collision fault: a flight low over the pit works (`FLYTO` cruise argument); the default cruise (1500 up) is what hits the cavern roof | low | driver / usage |
| T1 | Statue of Molag Bal (Bal Ur, Underground) never under the crosshair (uber TT_BalUr) | the statue's box (580 x 975, plinth and steps) holds the player's eye when standing beside it, and a pick from inside a box is refused. OpenMW picks by the mesh. An activator with a box under 1200 across, eye inside it, is now picked by the middle 40% of its box (`worldPick`, `world.cpp`) | med | fixed (bug-balur-statue; needs checkpoint uber-tt-aldsotha) |
| T2 | Chameleon 200 alone did not hide a theft from Berwen (uber bug 108) | matches OpenMW: awarenessCheck gives x = 200, her y = (47 + 13 + 4) x 1.25 x 1.5 = 120, so a roll of 80+ notices (1 in 5 per 5 s roll); weak observers never notice. Only the observer's Blind was missing from y; added (`formulas.cpp`) | low | matches OpenMW (bug-chameleon-theft guards the weak-observer side) |
| U1 | Escorts still stalled below the Seyda Neen strider platform (uber finding 26; Madura Seran, Tarvyn Faren) | a follower held up by a cliff base dropped off a ledge into a pocket the player never entered and kept re-trying a wall. OpenMW's navigation mesh finds a way; here the footstep trail now picks its start footstep by distance counting height twice, and a follower no nearer the player for 20 s, 700+ units off and out of the player's sight (or 2500+ away) is put 90 units behind the player. Differs (no OpenMW rule, kept as a catch-all) | high | PARTLY: footstep pick counts height; the Seyda hill case `bug-follow-seyda-hill` still fails (guar drops into a dead-end pocket under the ledge); no teleport |
| W1 | Ash statue lying on two crates (HR_MorvaynManor) could not be picked up | the crate's box covers the statue and the container rule kept the crate (the ray crossed its middle). OpenMW's nearest mesh hit is the statue. An item whose box is in the top half of the container's and over its middle now wins (`worldPick`). Fixed (`bug-pickup-ash-statue`) | low | fixed |
| W2 | DOORTO at Manat's Yurt failed "hidden from here" from the tent's roof | the walk ended on the skirt above the flap, and the spot search only looked at floor within 60 of our height. It now also looks at the door's own height, skipping spots right under our feet (`testdrive.cpp`). Fixed (`bug-yurt-doorto-far`) | low | driver |
| S2a | DOORTO the Vivec canton door left the driver on the plaza roof, 345 over the door (uber tg) | test driver only. The street under the canton is roofed wider than the ring search; the FLYTO now also looks for the nearest path grid point of that street whose column is clear (its open end), flies there and walks in (`testdrive.cpp`). Fixed (`bug-doorto-canton`; failed before: stuck at 22210 -82560 2579). |
| S2b | ACTIVATE after a FLYTO that ends over the target with levitation still running timed out (uber tt, Assantushansar) | test driver only. The walk followed the grid's route off the plateau. It now comes down on the spot (L held), beside the ledge that holds it up when it does not get lower; the ring search is shared with FLYTO and prefers ground clear all round. Fixed (`bug-activate-after-fly`; timeout before). |
| S2c | PICKUP:ebony_staff_caper (Therana's Chamber) and DOORTO the Dagoth Ur Outer Facility door stalled (uber tg, tt) | not driver bugs: the staff is 500 over a ramp that ends over a shaft (drink p_levitation_q), and the facility door is behind two gate leaves a crank 1300 south turns (ACTIVATE:ex_dwrv_crank_dagoth first). Cases bug-pickup-therana and bug-doorto-facility show the working steps. |
| Y1 | An item lying on another item (the Gambolpuddy on a pillow, Ald Daedroth) could not be picked up: "hidden from here" | the pillow's box covers the ring and worldPick kept the pillow. OpenMW picks the nearest mesh hit, which is the item on top. An item whose box lies within the picked item's box, or starts in its top half and is centred over it, and is on the ray, now wins (`world.cpp`). Fixed (`bug-pickup-on-misc`) |
| Y2 | Test driver fixes from the uber runs (Therana's Chamber doors, Mehrunes statue, KILL past a bystander) | DOORTO:<cell>@<n> takes the n-th ranked door (opt-in); ACTIVATE of a wide statue counts as reached at its box; KILL circles a blocking actor after 3 s. Fixed (`bug-doorto-nth`, `bug-activate-mehrunes-statue`). The Gnisis platform follower (uber hr) is not fixed: a follower change that fixed it broke uber-hh |
