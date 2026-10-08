# Crime: what is seen, reported and paid

Read from OpenMW `mechanicsmanagerimp.cpp` (`commitCrime`, `canReportCrime`, `reportCrime`). In our words.

## Seen

`commitCrime` looks at every actor within `fAlarmRadius` of the player (the victim too, however far). One that can report
(an NPC, alive, not knocked down, not in combat with the victim, not a follower of the player's) has *seen* the crime if:
it is the victim and knows (an assault always counts), or the crime is murder (anyone else, hearing is enough), or it has
line of sight and passes the awareness check. A crime nobody saw is not reported (an unseen assault still starts the
victim's fight, and a guard victim reports it).

## Reported and paid

A seen crime is put on record when a witness within the radius has AI Alarm of at least 100. The bounty added:

| Crime | Bounty |
|---|---|
| trespass / sleeping in an owned bed | `iCrimeTresspass` (5) |
| pickpocket | `iCrimePickPocket` (25) |
| assault | `iCrimeAttack` (40) |
| murder | `iCrimeKilling` (1000) |
| theft | `max(1, int(value x fCrimeStealing))` |

Witnesses' responses (disposition and fight raised by the crime; guards pursue when Alarm is at least 100):

| Crime | Fight (witness / victim) | Disposition change |
|---|---|---|
| trespass | `iFightTrespass` | none |
| pickpocket | `iFightPickpocket` / 4 x that | victim: `fDispPickPocketMod`, permanent; a guard with Alarm >= 100: the same |
| assault | `iFightAttacking` / `iFightAttack` | victim (not a guard): `iDispAttackMod`, permanent; Alarm >= 100: `iDispAttackMod`; else only if hostile, x Alarm / 100 |
| murder | `iFightKilling` | none |
| theft | `fFightStealing x value` | `int(fDispStealing x value x Alarm / 100)` (not permanent) |

`Session::crimeBounty` (`formulas.cpp`) is the bounty column; `openmw-spec-crime` tests it.

## Arrest (OpenMW `aipursue.cpp`, `actors.cpp`, `miscextensions.cpp`)

A guard who saw the crime (Alarm 100 or more) is marked *Alarmed* and pursues the player (`AiPursue`): it puts its
weapon away, runs to the player and, once there with line of sight, opens the conversation itself. Nothing in the
engine arrests: the guard's Greeting 0 decides, through the dialogue filters (Alarmed, PC crime level, gold, the
CharGenState global):

- during character creation (`CharGenState` not -1): "We'll let your actions go for now", `PayFine`, `SetPCCrimeLevel 0`;
- otherwise the court's terms with a Choice: Pay Gold (`RemoveItem Gold_001`, `PayFine`), Go to Jail (`GoToJail`),
  Resist Arrest (`StartCombat Player`);
- a bounty past `iCrimeThreshold x iCrimeThresholdMultiplier`: the death warrant line and a fight.

`PayFine` clears the bounty, takes the stolen goods and puts the weapon away; `PayFineThief` (the Thieves Guild) only
clears the bounty. `GoToJail` clears the bounty and takes the stolen goods too, then `max(1, bounty / iDaysInPrisonMod)`
days pass. When a guard starts a fight with the player, every guard pursuing them fights too (`startCombat`).
Paying calms the witnesses again and clears their Alarmed flag (the crime id).

Ours: `Session::guardCheck` (`combat.cpp`) walks the guard over and calls `forceGreeting`; `Ref::alarmed` is the flag
the Alarmed filter reads (with the bounty still standing); the fixed arrest screen (`drawArrest`) is only for a guard
with no greeting that fits. `issue-4` (theft during character creation), `crime-arrest-pay`, `crime-arrest-jail` and
`crime-arrest-resist` test it.

## Findings (2026-09-29)

1. **Theft bounty** ignored `fCrimeStealing`; it is the value as it stood (the same while the GMST is 1). Now
   `crimeBounty(CRIME_THEFT, value)`. Trespass and the other kinds read their GMSTs the same way.

## Open (no test yet: needs a world with witnesses)

- `Session::crimeSeen` uses one fight / disposition term per kind for every witness. OpenMW's differ for the victim and
  the others, theft scales with the value, murder and trespass change no disposition, and disposition changes are
  permanent only for victims (pickpocket, assault). Ours applies `idispkilling` / `idisptresspass` to witnesses.
- Witnessing here is "seen by the player's camera within the last second"; OpenMW: line of sight plus an awareness
  (sneak) roll, or hearing for murder.
- Guards here come for any bounty within 1500 units; OpenMW's come only if they saw the crime, or on sight once the
  bounty reaches `iCrimeThreshold`.
- Confiscated goods are removed; OpenMW moves them to the nearest prison's `stolen_goods` chest.
- Jail time uses a fixed 100 gold a day rather than `iDaysInPrisonMod`.

## Findings (2026-10-07)

2. **Arrest** was a fixed screen (pay / jail / resist) for every guard. Paying kept the stolen goods, and stealing
   during character creation could end in jail or a fight, where Morrowind's guard lets it go (issue #4). Now the
   guard opens the conversation and Greeting 0 decides, as above.

## Findings (2026-10-08)

3. **Witness fight** (open, `zz-open-witness-alarm`): one term per kind for every witness, so slaves with Alarm 0 turn on
   the player who assaults or kills in front of them. OpenMW scales the fight term by the witness's Alarm / 100, uses
   `iFightAttacking` for a witness of an assault (`iFightAttack` for the victim), and skips witnesses already in combat with
   the victim (`canReportCrime`). Tried (Alarm scaling, and the skip alone) and each shifted the committed `uber-fg` and `uber-hh`
   timelines (tuned on witnesses that attack), so none of it is in.
