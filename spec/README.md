# Formula spec, taken from OpenMW's source

One page per mechanic: the inputs, the formula, the GMSTs it reads, and where OpenMW's source does it (file and
function, for someone who wants to check). The formulas are written in our own words; nothing is copied from
OpenMW (GPLv3), and the OpenMW checkout used for reading lives in `build/openmw-src/` (ignored by git).

`tools/test/openmw_specgen.py` is the same spec as code: an oracle that computes each formula's number for random inputs
and writes `tools/test/cases/spec-*.txt`, which sets those inputs in the engine and `EXPECT`s the number. A failing
test is a place where the engine and the spec disagree; each page ends with what has been found so far.

| Page | Covers | Test |
|---|---|---|
| [magic.md](magic.md) | spell cost, cast chance, the school a spell trains | `openmw-spec-magic` |
| [crafting.md](crafting.md) | enchanting points and chance, alchemy potion strength | `openmw-spec-enchant`, `openmw-spec-alchemy` |
| [combat.md](combat.md) | armor rating, damage, hit and block, repair, recharge, security, persuasion | `openmw-spec-armor`, `-repair`, `-recharge`, `-security`, `-persuasion` |
| [economy.md](economy.md) | barter offer, training price and limits, travel price and time | `openmw-spec-barter`, `-training`, `-travel` |
| [crime.md](crime.md) | bounties per crime, who sees and reports, witness reactions | `openmw-spec-crime` (bounty column) |
| [derived.md](derived.md) | base health, level-up gains, fatigue term, Reflect / Spell Absorption, resistance | `openmw-spec-health` (hand-written), `openmw-spec-resist` (the resistance term and base), `openmw-spec-enchcast` |

What has and hasn't been compared with OpenMW, file by file and rule by rule: [audit.md](audit.md).

Rules for keeping it honest:
- OpenMW is the reference, not the truth. Where OpenMW itself looks wrong (it says so in its changelog, or its
  formula disagrees with UESP), the page says which side we follow and why.
- A finding gets a test before it gets a fix, and stays in the suite.
- Not yet covered: weather and time, AI (combat action choice, flee,
  wander), crime and bounties, dialogue filtering, the mechanics of individual magic effects (drain, absorb,
  reflect, resist), sneaking and detection, respawn.
