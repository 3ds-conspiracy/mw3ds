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

## Findings (2026-09-29)

1. **Theft bounty** ignored `fCrimeStealing`; it is the value as it stood (the same while the GMST is 1). Now
   `crimeBounty(CRIME_THEFT, value)`. Trespass and the other kinds read their GMSTs the same way.

## Open (no test yet: needs a world with witnesses)

- `Session::crimeSeen` uses one fight / disposition term per kind for every witness. OpenMW's differ for the victim and
  the others, theft scales with the value, murder and trespass change no disposition, and disposition changes are
  permanent only for victims (pickpocket, assault). Ours applies `idispkilling` / `idisptresspass` to witnesses.
- Witnessing here is "seen by the player's camera within the last second"; OpenMW: line of sight plus an awareness
  (sneak) roll, or hearing for murder.
- Bounty payment, jail time, stolen goods confiscation and the crime-level GMSTs are not in the spec yet.
