# Trading and Services

Mirrors the OpenMW wiki page *Research:Trading and Services*. Read from OpenMW `MechanicsManager::getBarterOffer`,
`mwgui/tradewindow.cpp` (`haggle`), `trainingwindow.cpp`, `travelwindow.cpp`, `spellbuyingwindow.cpp`,
`spellcreationdialog.cpp`, `merchantrepair.cpp`, `mwmechanics/enchanting.cpp` (`getEnchantPrice`). In our words.

## Barter offer

`pc = (disposition - 50 + min(100, Mercantile) + min(10, 0.1 Luck) + min(10, 0.2 Personality)) x fatigue term`,
`npc = (min(100, Mercantile) + min(10, 0.1 Luck) + min(10, 0.2 Personality)) x fatigue term` (their own numbers;
disposition is the derived one, clamped 0 to 100).
Buying: `int(base x 0.01 x (100 - 0.5 (pc - npc)))`; selling: `int(base x 0.01 x (50 - 0.5 (npc - pc)))`; never below 1,
but a base price of 0 stays 0 and creature merchants charge the base price. Test: `openmw-spec-barter` (also the haggle target, `hagglechance`).

## Haggling

An offer at least as good for the merchant as their price is accepted at once. Creatures never haggle. Otherwise:

    d  = int(100 x (price - offer) / price)  buying;   int(100 x (offer - price) / offer)  selling   (whole numbers)
    pc = (fDispositionMod x (disposition - 50) + Mercantile + 0.1 Luck + 0.2 Personality) x fatigue term   (no caps)
    npc = (Mercantile + 0.1 Luck + 0.2 Personality) x their fatigue term
    x  = fBargainOfferMulti x d + fBargainOfferBase + int(pc - npc);   accepted when a roll 1..100 is at most x

Disposition changes by `iBarterSuccessDisposition` / `iBarterFailDisposition`. An accepted haggle trains Mercantile
scaled by `floor(100 x the difference / the larger of the two prices)`. Selling an item back to the one it was stolen
from confiscates it. The merchant's gold goes up and down with trades and every service paid (training, travel,
spells, repair, spellmaking, enchanting), and is back to the record's after `fBarterGoldResetDelay` hours.

## Spells, spellmaking, enchanting, repair

- **Buying a spell:** `max(1, int(cost x fSpellValueMult))` at the barter offer.
- **Spellmaking:** the spell's cost is built like vanilla: each effect adds `max(1, effect cost)`, and a target effect
  multiplies the **running total** by 1.5; the cost is `int()` of that. Price `max(1, int(cost x fSpellMakingValueMult))`
  at the spellmaker's barter offer.
- **Naming (spellmaking, enchanting):** the made spell or item gets the name the player types (OpenMW's name box,
  `SpellCreationDialog::onBuyButtonClicked`, `EnchantingDialog::onBuyButtonClicked`). A new spell's name starts empty;
  the enchanted item's starts as the chosen item's own name and is reset when another item is chosen
  (`EnchantingDialog::setItem`). Buy refuses, in OpenMW's order: no effects (`sNotifyMessage30` for a spell,
  `sEnchantmentMenu11` for an item), no name (`sNotifyMessage10` for both), not enough gold (`sNotifyMessage18`). On
  the 3DS the name is typed on the system keyboard (the first button of the bottom row). Test: `issue-29`.
- **Enchanting service:** the last running enchantment cost (see `player-craft-skills.md`) x `fEnchantmentValueMult`
  at the enchanter's barter offer, x the number of items, at least 1.
- **Repair service:** `max(1, int(fRepairMult x int((max - current) / max(1, int(max / max(1, value))))))` at the
  barter offer.

## Training

Price: `max(1, int(the player's BASE skill x iTrainingMod))` (iTrainingMod is 10), then the barter offer for buying.
A trainer teaches their three best skills. Refused when the trainer's base skill is not above the player's base skill, or
the player's base skill has reached the governing attribute (the modified attribute). Test: `openmw-spec-training`.

## Travel

Price outdoors: `int(distance / fTravelMult)` (the distance itself if that is 0), where the distance is 3D from the
**player** to the destination; a guide standing indoors charges `fMagesGuildTravel` instead. Multiplied by
`1 + the number of followers`; at least 1; then the barter offer for buying. Time: `int(flat distance from the player /
fTravelTimeMult)` hours, outdoors only. Test: `openmw-spec-travel`.

## Findings (2026-09-29)

1. **Training price** read `ftrainingmod`, which is not a GMST (the real one is `iTrainingMod`), so the fallback of 10 was
   always used, and it used the modified skill (a Fortify or Drain moved the price and the refusals). Now the base skill
   (`Session::baseSkill`, from the creation skills plus training / book / use gains) and `itrainingmod`. The trainer
   refusal and the attribute limit compare base skills too.
2. **Travel price** was measured from the guide to the destination, flat, with no followers cost and no flat Mages Guild
   fare. Now from the player, in 3D, times `1 + followers`, `fMagesGuildTravel` indoors. Travel time is whole hours of the
   flat distance from the player, outdoors only (it was a fraction of an hour from the guide, indoors too).
3. **A base price of 0** came out as 1; OpenMW keeps it 0 (`barterPrice`).
4. **Haggling** took a fraction for d (OpenMW: whole numbers), divided a selling haggle by the merchant's price
   (OpenMW: by the player's), capped Mercantile, Luck and Personality (OpenMW doesn't, in the haggle), rolled even for
   an offer the merchant should simply take, let creatures haggle, and trained Mercantile by one use whatever the
   bargain (OpenMW: scaled by the percent gained). `Session::haggleChance`.
5. **Made spells** multiplied each target effect by 1.5 and rounded; vanilla (and OpenMW) multiply the running total
   and truncate, so a target effect after others costs more. Their price had no barter term. (`madeSpellCost`,
   `spellmakePrice`.) The committed `openmw-spec-magic.txt` expects the old costs: regenerate it.
6. **Enchanting service price** was the total points x the mult with no barter term; OpenMW: the last running cost at
   the enchanter's barter offer.
7. **Services** (training, travel, spells, repair, spellmaking, enchanting) didn't add their price to the merchant's
   gold. Spells with cost 0 were free (the `max(1, ...)` before the barter offer was missing).

8. **Made spells and enchanted items had no name of the player's** (issue #29): a spell was named after its first
   effect, an item "<item> of <first effect>", and an unnamed spell could be bought. Now the typed name, with OpenMW's
   refusals. The made spell's / item's id now hashes the name too, so the same effects under two names are two spells.

## Open

- Followers are counted as loaded actors following the player; OpenMW counts the player's followers and their followers
  who travel with them (`ActionTeleport::getFollowers`, including only those near the player).
