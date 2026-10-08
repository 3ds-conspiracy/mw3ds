"""Writes tools/test/cases/mech-magic.txt: magic effects checked by what they do (spells the harness puts together
from single effects, cast as the player casts; target effects at a creature and an NPC; potions, scrolls).

  python tools/test/mechmagic.py ; tools\\test\\run-test.ps1 mech-magic -Data out\\world -Start 'Seyda Neen'
"""
from pathlib import Path

L = []


def step(tok, secs=0.3):
    L.append(f"{secs} 0 0 0 0 {tok}")


def wait(secs):
    L.append(f"{secs} 0 0 0")


def spell(name, *effects):
    """effects: 'effect:min:max:duration:range[:arg]'"""
    step("FILL")
    for e in effects:
        step("ADDEFFECT:" + e)
    step(f"MAKESPELL:{name}")


def cast(name, after=1.0):
    step(f"CAST:mw3ds_test_{name}")
    wait(after)


def expect(spec):
    step("EXPECT:" + spec)


def snap(what):
    step("SNAP:" + what)


# setup: a strong caster (every cast lands), stats away from the 100 cap
step("MSG", 3)
step("GOD")
step("HOUR:10")
step("CLASS:battlemage")
step("BOOST:100")
step("SETATTR:strength:50")
step("SETSKILL:longblade:50")
step("SETSKILL:alchemy:50")
step("FILL")
wait(0.5)

# ---- fortify / drain: a stat up or down while it lasts, back after
for eff, what, arg, sign in [(79, "attr", "strength", 1), (83, "skill", "longblade", 1),
                             (17, "attr", "strength", -1), (21, "skill", "longblade", -1)]:
    name = f"e{eff}"
    spell(name, f"{eff}:10:10:4:0:{arg}")
    snap(f"{what}:{arg}")
    cast(name)
    expect(f"{what}:{arg}:eq:@{10 * sign}")
    wait(4.5)
    expect(f"{what}:{arg}:eq:@0")
for eff, what in [(80, "healthmax"), (81, "magickamax"), (82, "fatiguemax")]:
    name = f"e{eff}"
    spell(name, f"{eff}:20:20:4:0")
    snap(what)
    cast(name)
    expect(f"{what}:eq:@20")
    wait(4.5)
    expect(f"{what}:eq:@0")

# ---- Drain Health: 20 off at once, given back when it ends (not 20 a second)
spell("e18", "18:20:20:5:0")
snap("health")
cast("e18", 3)
expect("health:gt:@-25")
expect("health:lt:@-15")
wait(3.5)
expect("health:gt:@-5")
# Drain Fatigue the same
# (60, not 20: fatigue regrows a few points a second, and the check comes a while after the cast)
spell("e20", "20:60:60:5:0")
snap("fatigue")
cast("e20", 3)
expect("fatigue:gt:@-70")
expect("fatigue:lt:@-40")
wait(3.5)
expect("fatigue:gt:@-10")

# ---- Damage Health: magnitude a second; Restore Health gives it back the same way
spell("e75", "75:10:10:3:0")
spell("e23", "23:5:5:4:0")
snap("health")
cast("e23", 5)
expect("health:lt:@-15")
expect("health:gt:@-25")
snap("health")
step("CAST:mw3ds_test_e75")
wait(4)
expect("health:gt:@15")

# ---- Damage Attribute stays until restored; Restore Attribute brings it back
step("SETATTR:strength:50")
spell("e22", "22:2:2:3:0:strength")
snap("attr:strength")
cast("e22", 4)
expect("attr:strength:eq:@-6")
spell("e74", "74:10:10:2:0:strength")
cast("e74", 3)
expect("attr:strength:eq:@0")
step("SETSKILL:longblade:50")
spell("e26", "26:2:2:3:0:longblade")
snap("skill:longblade")
cast("e26", 4)
expect("skill:longblade:eq:@-6")
spell("e78", "78:10:10:2:0:longblade")
cast("e78", 3)
expect("skill:longblade:eq:@0")

# ---- Levitate: up while it lasts
spell("e10", "10:10:10:3:0")
cast("e10")
expect("levitating:eq:1")
wait(3.5)
expect("levitating:eq:0")
wait(3)

# ---- effects read while they last (movement, stealth, sight, resistances)
for group in [(0, 2, 8, 11, 9, 1), (39, 40, 41, 43, 42, 3), (90, 91, 92, 97, 68, 67), (64, 65, 66, 59, 4, 5)]:
    name = "p" + "_".join(map(str, group))
    spell(name, *[f"{e}:10:10:4:0" for e in group])
    cast(name)
    for e in group:
        expect(f"effect:{e}:eq:10")
    wait(4.5)
    for e in group:
        expect(f"effect:{e}:eq:0")

# ---- Resist Fire 100: fire does nothing
spell("e90", "90:100:100:5:0")
cast("e90")
snap("health")
step("EFFECT:14:10:1")
wait(2)
expect("health:eq:@0")
wait(3.5)

# ---- cures, dispel
step("EFFECT:27:2:20")
wait(0.5)
expect("effect:27:gt:0")
spell("e72", "72:1:1:0:0")
cast("e72")
expect("effect:27:eq:0")
step("SPELL:collywobbles")
spell("e69", "69:1:1:0:0")
snap("spells")
cast("e69")
expect("spells:eq:@-1")
spell("e79b", "79:10:10:30:0:strength")
snap("attr:strength")
cast("e79b")
expect("attr:strength:eq:@10")
spell("e57", "57:100:100:0:0")
cast("e57")
expect("attr:strength:eq:@0")

# ---- summons and bound items: there while it lasts
spell("e102", "102:1:1:4:0")
snap("effects")
cast("e102")
expect("refally:scamp_summon:eq:1")
expect("effects:eq:@1")
wait(4.5)
expect("effects:eq:@0")
spell("e121", "121:1:1:4:0")
cast("e121")
expect("count:bound_longsword:eq:1")
wait(4.5)
expect("count:bound_longsword:eq:0")

# ---- potions and scrolls
step("EFFECT:23:10:2")
wait(2.5)
snap("health")
step("GIVE:p_restore_health_s")
step("EQUIP:p_restore_health_s")
wait(3)
expect("health:gt:@5")
expect("count:p_restore_health_s:eq:0")
step("GIVE:sc_vitality")
snap("effects")
step("CAST:item:sc_vitality")
wait(1)
expect("count:sc_vitality:eq:0")

# ---- target and touch effects on a creature
step("PLACE:scamp:250")
wait(1.5)
spell("t14", "14:10:10:1:2")
snap("refhealth:scamp")
step("FILL")
step("CASTAT:scamp:mw3ds_test_t14")
wait(2)
# the scamp's partial resist is a roll (OpenMW's), so only that some damage landed is certain
expect("refhealth:scamp:lt:@-0.01")
spell("t45", "45:1:1:5:2")
step("CASTAT:scamp:mw3ds_test_t45")
wait(2)
expect("refknock:scamp:gt:0")
wait(4)
spell("t52", "52:100:100:10:2")
step("CASTAT:scamp:mw3ds_test_t52")
wait(2)
expect("refcombat:scamp:eq:1")
spell("t50", "50:100:100:10:2")
step("CASTAT:scamp:mw3ds_test_t50")
wait(2)
expect("refcombat:scamp:eq:0")
spell("t54", "54:100:100:10:2")
step("CASTAT:scamp:mw3ds_test_t54")
wait(2)
expect("refflee:scamp:eq:1")
step("PLACE:mudcrab:110")
wait(1)
spell("t86", "86:5:5:1:1")
step("EFFECT:23:10:1")
wait(1.5)
snap("refhealth:mudcrab")
snap("health")
step("CASTAT:mudcrab:mw3ds_test_t86")
wait(1)
expect("refhealth:mudcrab:lt:@-4")
expect("health:gt:@4")
spell("t58", "58:1:1:30:1")
step("CASTAT:mudcrab:mw3ds_test_t58")
wait(1)
expect("refsoultrap:mudcrab:eq:1")

# ---- Charm on a person: disposition up
step("PLACE:fargoth:90")     # right in front, nothing nearer to catch the bolt
wait(1)
step("FACE:fargoth")      # the bolt goes where the crosshair is: at Fargoth, not at whatever stands nearer
wait(0.5)
spell("t44", "44:30:30:1:2")
snap("refdisp:fargoth")
step("CASTAT:fargoth:mw3ds_test_t44")
wait(2)
expect("refdisp:fargoth:gt:@0")
expect("bounty:eq:0")

# ---- Mark here, Almsivi Intervention away, Recall back
spell("e60", "60:1:1:0:0")
cast("e60")
spell("e63", "63:1:1:0:0")
cast("e63", 6)
expect("notcell:Seyda%Neen")
spell("e61", "61:1:1:0:0")
cast("e61", 6)
expect("cell:Seyda%Neen")
spell("e62", "62:1:1:0:0")
cast("e62", 6)
expect("notcell:Seyda%Neen")
wait(1)

out = Path(__file__).resolve().parent / "cases" / "mech-magic.txt"
out.write_text("\n".join(L) + "\n")
print(f"{out}: {len(L)} steps")
