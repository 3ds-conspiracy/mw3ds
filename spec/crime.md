# Crime: what is seen, reported and paid

Read from OpenMW `mechanicsmanagerimp.cpp` (`commitCrime`, `canReportCrime`, `reportCrime`). In our words.

## Seen

`commitCrime` looks at every actor within `fAlarmRadius` of the player (the victim too, however far). One that can report
(an NPC, alive, not knocked down, not in combat with the victim, not a follower of the player's) has *seen* the crime if:
it is the victim and knows (an assault always counts), or the crime is murder (anyone else, hearing is enough), or it has
line of sight and passes the awareness check. A crime nobody saw is not reported (an unseen assault still starts the
victim's fight, and a guard victim reports it).

No crime at all against someone who could not be its victim (`canCommitCrimeAgainst`): a creature, one already fighting
the player, one engaged in a fight with any actor (Baslod rushing Falco in Raven Rock's Protect Falco), one pursuing the
player, a werewolf, a vampire. The blow still starts their fight (case `crime-hit-fighter`). The same holds for the killing blow: a kill of one engaged with another actor is no murder (case
`crime-kill-fighter`).

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

`Session::crimeBounty` (`mwmechanics/formulas.cpp`) is the bounty column; `openmw-spec-crime` tests it.

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
days pass. When a guard starts a fight with the player, every guard pursuing them fights too, however far, and no other
guard does (`startCombat`: those whose current package is AiPursue switch to AiCombat).
Paying calms the witnesses again and clears their Alarmed flag (the crime id).

A guard the player strikes arrests rather than fights back (`actorAttacked`): the assault is reported first, and the
report gives every guard with Alarm 100 in the radius AiPursue, the one struck too (the victim counts however far
away); the fight back is skipped for one in pursuit. Blows on a pursuing guard are no new crime
(`canCommitCrimeAgainst`) and still bring no fight back. A struck guard not after the player (Alarm under 100, or busy
fighting another actor, which is no crime) fights, and brings the pursuing guards in with him.

Ours: `Session::guardCheck` (`mwmechanics/mechanicsmanagerimp.cpp`) walks the guard over and calls `forceGreeting`; `Ref::alarmed` is the flag
the Alarmed filter reads (with the bounty still standing); the fixed arrest screen (`drawArrest`) is only for a guard
with no greeting that fits. `issue-4` (theft during character creation), `crime-arrest-pay`, `crime-arrest-jail` and
`crime-arrest-resist` test it.

## Findings (2026-09-29)

1. **Theft bounty** ignored `fCrimeStealing`; it is the value as it stood (the same while the GMST is 1). Now
   `crimeBounty(CRIME_THEFT, value)`. Trespass and the other kinds read their GMSTs the same way.

## Open

- Confiscated goods are removed; OpenMW moves them to the nearest prison's `stolen_goods` chest (UESP: the Evidence
  chest of the nearest Imperial Legion fort, lock 50).
- Jail time uses a fixed 100 gold a day rather than `iDaysInPrisonMod` (the same while it is 100).
- A theft's fight term is `fFightStealing` x Alarm / 100, not x the value as OpenMW has it.
- The reaction goes to the witnesses who noticed the player (`lastSeen`: line of sight and the awareness check);
  OpenMW gives it to everyone within `fAlarmRadius` once anyone saw it.

## Findings (2026-11-03)

4. **Guards came for any bounty** (fixed, `bug-guard-unseen-bounty`): every guard within 1500 units walked over and
   greeted the player whenever the bounty was above 0, seen or not. Now a reported crime alarms the guards within
   `fAlarmRadius` with Alarm 100 (OpenMW's reportCrime: setAlarmed, AiPursue); another guard comes on sight only from
   `iCrimeThreshold` (1000), and attacks from `iCrimeThreshold` x `iCrimeThresholdMultiplier` (OpenMW's
   updateCrimePursuit; UESP's Crime: "more than 1000, guards will confront you on sight"). Below that the player
   answers for the bounty when talking to a guard. `Session::guardCheck`, `reportCrime`. A pursuing guard runs (OpenMW's
   AiPursue sets the run flag; ours walked), and no longer stops at 1500 units: seeing the player (`lastSeen`, line of
   sight and the awareness check out to 2000) is what counts. Ubers changed for it: `uber-morag-tong` walks towards the
   ordinator, `uber-trib-main` pays Ivulen Irano's "turned yourself in" greeting (its Pay Gold is choice 5: the driver
   finds it by its label), `uber-bm-colony` and `uber-bm-colony-carnius` talk to the fort's guard to pay for the
   punches (paying calms Falco Galenus, whom they stirred).
5. **Disposition** (fixed, `bug-murder-witness-disposition`): every witness lost the kind's term (`iDispKilling` -50 for
   a murder, `iDispAttackMod` -50 for the blow before it). OpenMW's reportCrime: a theft costs its term x Alarm / 100;
   a pickpocketing the victim only, for good; an assault the victim `fDispAttacking` for good, a witness with Alarm 100
   `iDispAttackMod`, anyone else that x Alarm / 100 only when it turns them hostile; murder and trespass nothing.
6. **Jail** (fixed, `bug-jail-term`): Security and Sneak gained their point only half the time and lost one the other
   half; UESP's Crime: "For Security or Sneak, however, you actually gain one skill point instead".

## Findings (2026-11-12)

7. **A struck guard fought** (fixed, `bug-guard-struck-arrests`): every blow made its victim fight back before the
   crime was reported, so a guard the player struck fought them and never asked (an Ald'ruhn guard fought the player
   for 40 seconds on the console). Now `Session::actorAttacked` (the five places a blow, a miss, a shot or a harmful
   spell reaches someone) reports the assault first and leaves a guard on his way to arrest (`guardPursues`) out of
   the fight; blows on him then add nothing. The five places had drifted apart: spells counted a crime on an
   aggressor or a werewolf, shots on a werewolf; all now follow `canCommitCrimeAgainst`.
8. **Resisting arrest** (fixed, `bug-guards-pursuing-join-fight`): every guard within 1500 units turned on the player,
   pursuing or not, and every guard met later did too while the bounty stood (`arrestDeclined`, now gone). Now
   `makeHostile` on a guard brings in the guards pursuing the player, however far, and no others: Resist Arrest, an
   attack on sight past `iCrimeThreshold` x `iCrimeThresholdMultiplier`, a guard struck while not after the player.
9. **The victim beyond the radius** (fixed, `bug-guard-struck-far-reports`): an assault on someone over
   `fAlarmRadius` from the player went unreported unless someone nearer had Alarm 100. Now the victim counts wherever
   it is, as a reporter and as a guard sent after the player (`reportCrime`).

## Findings (2026-10-07)

2. **Arrest** was a fixed screen (pay / jail / resist) for every guard. Paying kept the stolen goods, and stealing
   during character creation could end in jail or a fight, where Morrowind's guard lets it go (issue #4). Now the
   guard opens the conversation and Greeting 0 decides, as above.

## Findings (2026-10-08)

3. **Witness fight** (fixed, `bug-witness-alarm`): `crimeSeen` used one fight term per kind for every witness, so slaves
   with Alarm 0 turned on the player who assaulted or killed in front of them. Now the term is scaled by the witness's
   Alarm / 100 (a pickpocket victim with Alarm 0 counts as 100, other pickpocket witnesses as 0), a witness of an assault
   uses `iFightAttacking` (the victim `iFightAttack`), and witnesses already in combat are skipped (as before).
   Not done: the disposition and distance biases OpenMW adds to the term. `uber-fg` and `uber-hh` were re-tuned on the
   new rule (a longer speed potion run in the Elith-Pal Mine; walking up to Galyn Arvel so J'Saddha is in range).
