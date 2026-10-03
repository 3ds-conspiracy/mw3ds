# Findings: dialogue (response filter, greetings, choices, topic list, substitution)

Written from OpenMW's `mwdialogue/*.cpp` and `interpreter/defines.cpp` only; `source/dialogue.cpp` was not read.
Full rule text: `spec/dialogue.md` (R1-R28). Already audited and not repeated here: D1-D28, F1-F29 in
`spec/audit-findings.md`; this page extends them with tests.

## Rules

One line each; the page has the detail and the OpenMW function.

| Rule | OpenMW | Extends |
|---|---|---|
| R1 first matching info wins, file order | `filter.cpp Filter::search / list` | |
| R2 Info Refusal fallback (only disposition blocked; only some callers) | `Filter::list` | D1, F6 |
| R3 speaker tests: id, creatures, race, class, faction + rank, FFFF, rank without faction, gender | `Filter::testActor` | D21, D22, D24-25 |
| R4 player tests: PC faction + rank, rank in speaker's faction, PLAYER's cell prefix | `Filter::testPlayer` | D23, D28 |
| R5 Choice false outside a choice; Weather false indoors | `Filter::testSelectStruct` | D7 |
| R6 derived disposition >= info's; inverted for service refusal | `Filter::testDisposition` | D27 |
| R7 PC attributes / skills modified values, level, rep, bounty, gender | `getSelectStructInteger` | |
| R8 PC health percent truncated; magicka / fatigue / health absolute | `testSelectStructNumeric` | D19 |
| R9 NPC rep, level, health percent, Fight / Hello / Alarm / Flee | `getSelectStructInteger` | |
| R10 faction functions: same faction, rank difference (PC minus NPC), reaction low / high, rank requirement, expelled | `getSelectStructInteger / Boolean` | D8-D10 |
| R11 flags: diseases, Corprus, Vampirism (effect), clothing slots 0-15, same sex / race, Detected, Alarmed, Attacked, Should Attack, Werewolf, Creature Target, Friend Hit cap 4, Dead counts, Item count, Journal | same | D11-D20 |
| R12 Not-ID / Faction / Class / Race / Cell ignore operator and value | `getSelectStructBoolean`, `selectwrapper.cpp getType` | D26 |
| R13 missing global ignored; Local / Not Local on speaker's script | `testFunctionLocal`, `testSelectStructNumeric` | D5, D6 |
| R14 Talked-to-PC frozen at conversation start | `startDialogue`, `say` | D3, D4 |
| R15 greetings in sorted id order, no fallback, none = no conversation, script then topics | `startDialogue` | F1, F4 |
| R16 topic click: ignored in a choice, journal history only from own topic | `executeTopic` | |
| R17 service refusal: choice = service, inverted disposition, no fallback | `checkServiceRefused` | D27, F23 |
| R18 Persuasion titles from GMST s + name | `executeTopic` | F24 |
| R19 choices, Goodbye, permanent disposition on leave | `questionAnswered`, `goodbyeSelected` | F27 |
| R20 voiced reactions: choice 0, no fallback, skip conditions | `say` | |
| R21 topic list = player-known and speaker-answerable (refusal counts), sorted ignoring case | `updateActorKnownTopics`, `getAvailableTopics` | D2, F7 |
| R22 learning topics from raw text: separators, longest first, end unchecked, explicit links | `keywordsearch.cpp`, `addTopicsFromText` | F5 |
| R23 exhausted / specific flags | `updateActorKnownTopics`, `topic.cpp` | F8 |
| R24 quest index only rises through entries | `quest.cpp`, `journalimp.cpp` | |
| R25-R28 % and ^ substitution table, prefix match, unmatched escape skips the next char | `defines.cpp fixDefinesReal` | F29 |

Quirks to keep (vanilla bugs OpenMW keeps): the Not family's ignored operator (R12); the missing end-of-word check on
keywords (R22); the character after an unmatched `%` never being an escape (R26). Followed as is.

## Tests written

`tools/test/specgen_dialogue.py` -> `tools/test/cases/openmw-spec-dialogue.txt` (seed 1, count 100: about 1200 lines, 100
cases). Each case: `LOAD` a clean save, `GOTO` the NPC's cell (only NPCs placed in exactly one interior cell), set the
player state the sampled conditions read, `SETDISP`, then:
- topic cases: `TALK`, state tokens, `KNOW:<topic>`, `EXPECT:answer:<topic>:<npc>:eq:<snippet of the oracle's response>`
  plus `ne` lines for earlier infos the oracle rejected and `EXPECT:topiclisted:<topic>`. About half stress the
  first-match order, a quarter the disposition / Info Refusal fallback; some check that NO response is said (`ne` only).
- greeting cases (`EXPECT:greeting`): which of Greeting 0-9 wins, with substitution done (the oracle expands
  `%Name %Race %Class %Faction %Rank %Cell %PCCrimeLevel`; other substitutions are left out of the snippet).
- learn cases: a topic answer with no result script, then `EXPECT:topiclisted` for the topics its raw text names that
  the speaker can answer (keyword rules R22).
State the oracle varies: journal index, globals, PC attributes and skills, level, reputation, bounty, faction
membership and rank, disposition. It covers the conditions R3-R13 where the harness can set the input: faction rank
difference, reaction low / high, same faction, Not-ID / faction / class / race / cell with odd operators,
Local / Not Local, Weather in interiors (always false), Choice outside a choice (always false), NPC Fight / Hello /
Alarm / Flee, level and reputation. Unknown inputs make the oracle drop the case (it counts, does not guess).
`python tools/test/specgen_dialogue.py --selftest` checks the oracle's own pieces (substitution quirks, keyword
matching, faction math, disposition fallback) and writes nothing.
`--pc-race <race> --pc-female <0|1>` adds `PCRACE` / `PCSEX` tokens (hooks below) and makes Same Race / Same Sex /
PC Gender conditions decidable; without them greetings that depend on race or sex are not written.

## Hooks needed in the engine

Existing and used: `TALK`, `KNOW`, `TOPIC`, `SETDISP`, `SETREP`, `SETBOUNTY`, `JOIN`, `JOURNAL`, `SETGLOBAL`,
`SETATTR`, `SETSKILL`, `LEVEL`, `EXPECT:answer`, `EXPECT:greeting`, `EXPECT:said`, `EXPECT:topiclisted`.
Needed (name / args / returns):

1. `PCRACE:<race id, spaces as _>` (setup): the player's race becomes that, including what Same Race and `%PCRace`
   read. `PCSEX:<0|1>` (setup): 0 male, 1 female (Same Sex, PC Gender). Without these about half the greetings
   (they are split by race and sex) cannot be predicted.
2. `PCNAME:<name>` (setup) and an `EXPECT:said`-style check on expanded text, to test `%PCName`, `%PCRank`,
   `%PCNextRank`, `%PCClass`: `%PCRank` / `%NextPCRank` need `JOIN` ranks to be readable too (`EXPECT:said` already
   shows the expanded text).
3. `EXPECT:topicnotlisted:<topic>` (no arg beyond the topic; passes when the open conversation does NOT offer it).
   Needed for: topics hidden because no info passes, learned topics the speaker cannot answer (R21, R22), and the
   no-answer case of the list.
4. `EXPECT:topicflag:<topic>:<exhausted|specific>` returns 0 / 1 (`ge / eq`): R23. The flag is computed in the list
   refresh; `TOPIC:<topic>` then `EXPECT:topicflag:<topic>:exhausted:eq:1` would test it.
5. `EXPECT:answerchoice:<topic>:<npc>:<choice n>:<eq|ne>:<text>`: like `EXPECT:answer` with the filter's choice number
   set to n (R19, R17: Service Refusal takes the service number as n). Today `answer` passes -1 and special-cases
   Service Refusal.
6. `EXPECT:answerid:<topic>:<npc>:<index>` returns the 0-based position of the chosen info within the topic (a
   number, `eq`), so tests need no unique text snippet (many infos share generic lines).
7. `SETITEM:<id>:<n>` (setup): the player holds exactly n of an item (Item conditions need an exact count; today
   `GIVE` adds). `SETDEAD:<id>:<n>` (setup): the death count of an id (Dead conditions). `SETLOCAL` exists, so Local
   conditions are testable when the oracle gets the variable's value from the test it sets.
8. `SETTALKED:<npc>:<0|1>` (setup): the NPC's real talked-to-player flag, to test R14 (frozen flag) from both
   values; and `EXPECT:talked:<npc>` (the real flag).
9. `EXPECT:greetingtopic:<npc>` returns the id of the Greeting topic that won, as text (`eq`): to check R15's order
   without a snippet.
10. `EXPECT:hello` style say(): `EXPECT:voiced:<npc>:<topic>:<eq|ne>:<text>` the response a voiced reaction (Hello,
    Hit, Flee ...) would pick: choice 0, no refusal fallback (R20).
11. `EXPECT:learned:<topic>` (like `topiclisted` but for the player's known set, listed or not).
12. `CLOTHVALUE:<n>` (setup) or a way to read the Clothing Modifier (the sum of values in slots 0-15), to test R11.

## Open questions

- `JOURNAL:<quest>:<n>`: does it add the entry n (raising the index only upward, as R24) or set the index? The oracle
  uses indexes that exist as entries and treats a quest never touched as 0 (except `a1_1_findspymaster`, started by
  chargen). Check that a clean `LOAD` really leaves every quest at 0.
- A fresh test player is assumed: no factions, no diseases, not expelled, no Corprus / Vampirism, 100% health,
  Talked-to-PC false, nobody dead, a speaker's Attacked / Alarmed false, bounty unset until the test sets it. If the
  harness's starting state differs, cases will report mismatches that are setup, not engine.
- `PCName` / `PCRank` / `PCNextRank` / `Faction` / `Rank` getters (the interpreter context) were not read, only the
  table in `defines.cpp`. What `%Rank` prints with no faction or a rank outside the list (the audit says "%") should be
  confirmed from `mwscript/interpretercontext.cpp`.
- A non-member's rank in `getFactionRank` (assumed -1) and what `Rank Requirement` does for a non-member: read from
  `npcstats.cpp`, not in this reading.
- Info order inside a topic is the order in the ESM files as loaded (later plugins' infos are merged by `PREV` / `NEXT`
  links). The converter keeps one linear order; whether it equals OpenMW's merged order is not checked.
- The converter (`tools/convert/build_game.py static_match`) compares Not-ID / Faction / Class / Race / Cell conditions with
  the stored operator and value, but OpenMW ignores them (R12). Infos where the stored form is not "= 1" (56 topics
  have one; the oracle skips them, and Greeting 2 among them) may have been dropped or kept wrongly by the
  converter and cannot be told from the converted data.
- The converter fixes each info's conditions from the LAST actor in the level that matches it (`dynamic = d`):
  the same for every actor, so this is fine, but infos named only by actors in other levels are dropped: the oracle
  sees what the engine sees.
- The converter's static pre-filter also uses the cells an actor appears in for the cell filter; OpenMW uses the
  player's cell (R4). The tests stand the player in the NPC's cell, which makes them agree.
- Quasi-exterior cells (R5): the oracle only samples interiors, so the exception is not tested.
- GetFactionReaction overrides by script (`ModFactionReaction` etc.) are not sampled.
- Persuasion (Admire / Intimidate / Taunt / Bribe) outcomes and `Info Refusal` titles are covered by the existing
  persuasion spec, not here.

## Mismatches

Fix phase (read against `mwdialogue/*.cpp`, `defines.cpp`, `interpretercontext.cpp`). Not run in the emulator yet.

| Rule | Our behaviour | Cause | Status | File |
|---|---|---|---|---|
| R12 Not-ID / Faction / Class / Race | The converter dropped an info when the stored operator / value said "= 0" (or similar): 56 topics, Greeting 2 among them | `static_match` ran `compare(op, truth, value)`; OpenMW needs only the plain truth (the engine already did) | fixed (needs a data rebuild) | tools/convert/build_game.py |
| R11 Same Sex / Same Race (44, 45) | A creature with a record "female" bit equal to the player's sex read true | no creature guard | fixed | source/dialogue.cpp |
| R11 Vampirism (60) | Read the script global `pcvampire` | wrong source | fixed: the Vampirism effect (133) magnitude > 0 | source/dialogue.cpp |
| R16 topic click | `dialogueTopic` answered while a choice was open and for any dialogue type (the UI hid both, a test hook did not) | no guard in the rule code | fixed | source/dialogue.cpp |
| R19 choice answer | A persuasion reply was answered again by a choice; topics a choice answer names were learned after its script | no type test; one order for all | fixed (only Topic / Greeting are re-searched; learn before the script for a choice answer) | source/dialogue.cpp |
| R20 voiced reactions | Hello / Hit lines were searched with choice -1 (Choice conditions false) | callers passed -1 | fixed: `dialogueVoiced` (choice 0, live talked flag, no fallback) | source/dialogue.cpp, combat.cpp, session.cpp (one call each) |
| R22 explicit links | `@topic#` in a text was not a hit | keyword search had no link split | fixed (`topicsNamedIn`) | source/dialogue.cpp |
| R23 exhausted / specific | No such flag in the engine (the 3DS list does not colour topics) | UI only | differs on purpose for the UI; the rule is computed for the tests by `dialogueTopicFlags`. Open: history is the journal Topics text ("who: text", 12 lines per topic), not info ids, so it is an approximation | source/dialogue.cpp |
| R26 unmatched `%` / `^` | The character after an unmatched escape could start an escape ("%%name" gave "%" + name) | loop re-read it | fixed | source/dialogue.cpp |
| R17 test `answer` for Service Refusal | The test hook allowed the Info Refusal fallback | hook only (the engine did not) | fixed: no fallback | source/testdrive.cpp |
| R25 action-key names (`%ActionActivate` ...) | not substituted | not meaningful in 3DS dialogue text | differs on purpose | n/a |
| R25 global values | printed with `%g` for shorts / longs too (a long of 1000000 prints 1e+06) | type not known in the engine's globals | open (rare) | n/a |
| R11 Alarmed (49) | true when the speaker is fighting | no alarmed flag on a Ref (memory) | open: approximation | n/a |
| R11 Creature Target (65) | only for allies / followers that see a hostile creature within 1500 units | no combat target on Ref for others | open: approximation | n/a |
| R5 Weather indoors | tests the speaker's cell, no quasi-exterior flag in our cells | cell data | open (OpenMW tests the player's cell; the same in a talk) | n/a |
| R13 locals | locals are floats; a short read would be truncated in OpenMW | float storage | open (needs a script-side check) | n/a |

Checked by reading, same as OpenMW: R1, R2, R3, R4, R6, R7-R10 (modified values, rank difference, reaction, rank requirement),
R11 clothing slots 0-15 without shield, R14 (frozen flag), R15, R17, R18, R21, R24, R27, R28.

Hooks added to `source/testdrive.cpp`: `PCRACE`, `PCSEX`, `PCNAME`, `SETITEM`, `SETDEAD`, `SETTALKED`, `CLOTHVALUE`,
`EXPECT:topicnotlisted`, `topicflag`, `answerchoice`, `answerid` (`<topic>:<npc>:[<op>:]<n>`; an Info Refusal answer reads
1000 + its place), `talked` (`<npc>[:<op>:<n>]`), `greetingtopic` (`<npc>:[<eq|ne>:]<topic>`, underscores for spaces),
`voiced`, `learned`. Answers to the open questions: `JOURNAL:` sets the index directly (`setJournal`); `%Rank` / `%Faction`
with no faction print `%` (matches `interpretercontext.cpp`); `%PCRank` for a non-member prints the first rank.
