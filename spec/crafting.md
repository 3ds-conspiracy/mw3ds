# Crafting: enchanting and alchemy

Reference: OpenMW `mwmechanics/enchanting.cpp` (`getEffectCosts`, `getEnchantPoints`, `getEnchantChance`),
`mwmechanics/alchemy.cpp` (`updateEffects`, `applyTools`, `listEffects`, `getAlchemyFactor`).
GMSTs: `fEnchantmentChanceMult` (3), `fEnchantmentConstantChanceMult` (0.5),
`fEnchantmentConstantDurationMult` (100), `fEffectCostMult`, `fPotionStrengthMult` (0.5),
`fPotionT1MagMult` (1.5), `fPotionT1DurMult` (0.5), `iAlchemyMod` (2).

## Enchanting points

Running cost over the effects (each effect's cost is added to the total so far, so the list is cumulative):

    cost += ((magMin + magMax) * duration + area) * base * fEffectCostMult * 0.05     (mags and area at least 1)
    duration = fEnchantmentConstantDurationMult for constant effect
    cost = max(1, cost);  if the effect is target range: cost *= 1.5
    costs.push(cost)

- item capacity check and the cast cost use the **floored** sum of `costs`;
- the chance uses the **unrounded** sum.

## Enchanting chance

    chance = (Enchant - points * fEnchantmentChanceMult + 0.2 * Intelligence + 0.1 * Luck) * fatigueTerm
    constant effect: chance *= fEnchantmentConstantChanceMult

(For thrown weapons and ammunition the points count per item; not ported.)

## Alchemy

`factor = Alchemy + 0.1 * Intelligence + 0.1 * Luck`; `x = factor * mortarQuality * fPotionStrengthMult`;
value `= int(x * iAlchemyMod)`.

Effects: for each ingredient in slot order, each of its four effects (by its id and, for skill or attribute
effects, the skill or attribute) that is not listed yet and is also on a later ingredient.

Per effect: magnitude `= (x / fPotionT1MagMult) / base` (1 with no magnitude), duration
`= (x / fPotionT1DurMult) / base` (1 with no duration); the apparatus then changes each of the two that exist:

| tools present (retort for helpful effects, alembic for harmful) | quality |
|---|---|
| both tool and calcinator, harmful | `2 * tool + 3 * calc` |
| both, helpful, effect has magnitude and duration | `2 * tool + calc` |
| both, helpful, otherwise | `2/3 * (tool + calc) + 0.5` |
| tool only, harmful | `1 + tool` |
| tool only, helpful | `tool` (with magnitude and duration) or `tool + 0.5` |
| calcinator only | `calc` (with both) or `calc + 0.5` |

Helpful effects, and anything with the calcinator alone, add the quality; a harmful effect with a tool divides
by it. Both numbers are rounded; an effect where either is 0 is dropped.

A brew fails when a roll 0..99 is over the factor; the ingredients are used either way, and only a brew that works
trains Alchemy. The same potion brewed again joins the first one's stack (`alchemy-fail-stack`).

Which effects the player can read: an ingredient's, one per `fWortChanceValue` (15) of Alchemy, in its own order; a
potion's (its tooltip, the brewing screen's list of the potion to be), two at a time, the first two at 15, the next
two at 30 and so on (`Alchemy::knownEffect`); the rest read "?". Ours named every effect of a potion, and the brewing
screen counted by the ingredients (2026-10-24, `bug-potion-known-effects`).

Eating an ingredient (`rollIngredientEffect`, index 0): only its first effect, on a roll 0..99 at or under
`x = (Alchemy + 0.2 * Intelligence + 0.1 * Luck) * fatigue term`, for `int(roll / min(x, 100) * 0.25 * x)` seconds at a
magnitude of `floor(0.05 * that / (0.1 * base))`, at least 1 (no duration: 1 s at `floor(that / (0.1 * base))`; no
magnitude: 1). Over x: "%s has no effect on you" (`sNotifyMessage50`). Eaten either way; Alchemy trains only when it
works. Ours put on all four effects at the record's numbers and never failed (2026-10-24, `bug-eat-ingredient`).

## Findings (2026-09-28, all fixed, tested by `openmw-spec-enchant`, `openmw-spec-alchemy`)

1. **Enchant chance** had another formula (`Enchant + 0.25 Int + 0.125 Luck - 7.5 / chanceMult * points`, no
   fatigue term). OpenMW's changelog lists "Enchanting success chance calculations are blatantly wrong"
   (bug 5038) as fixed, with the formula above. `Session::enchantCalc`.
2. **Enchant points** ignored the area (always 1), and the chance used the floored sum.
3. **Alchemy apparatus** used another formula for the tools' effect (averages and a plain add), and the
   potion's value used `fPotionT1MagMult` where OpenMW uses `iAlchemyMod`. `Session::brewPotion`.

Open question: OpenMW sums the cumulative costs of a multi-effect enchantment, which reads like a bug (its
changelog has "Multi-effect enchantments are too expensive", bug 8340, fixed). We follow master. Vanilla
Morrowind through MWSE would settle it.
