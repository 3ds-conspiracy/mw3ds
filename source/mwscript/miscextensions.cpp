// Script functions: activation, enable / disable, locks, scripts, message boxes, console commands, the rest
#include "components/interpreter/interpreter.hpp"

#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "components/debug/debuglog.hpp"
#include "mwworld/worldimp.hpp"
#include "components/interpreter/interpreterhelpers.hpp"

// A body touching an object (OpenMW's "colliding": a contact with it, from the side or standing on it): the body's
// circle (radius about its feet) within 2 units of the object's box, its height overlapping the box's or its feet on top
static bool touchesBox(const Ref& o, const float* feet, float radius, float height)
{
	float dx = fmaxf(fmaxf(o.boxMin[0] - feet[0], feet[0] - o.boxMax[0]), 0.0f);
	float dy = fmaxf(fmaxf(o.boxMin[1] - feet[1], feet[1] - o.boxMax[1]), 0.0f);
	float reach = radius + 2.0f;
	return dx * dx + dy * dy <= reach * reach && feet[2] <= o.boxMax[2] + 4.0f && feet[2] + height > o.boxMin[2];
}

// The player's and the actors' bodies for touchesBox (mwphysics/movementsolver.cpp's player, projectilemanager.cpp's actor)
static const float kPlayerBodyRadius = 22.0f, kPlayerBodyHeight = 128.0f, kActorBodyRadius = 34.0f, kActorBodyHeight = 135.0f;

std::optional<float> miscExtensions(Ctx& c, const Node& n, World& w, bool onPlayer, int ref, Ref* r)
{
	switch (n.func)
	{
	case F_ONACTIVATE:
	{
		// asked once: reading it clears it (OpenMW); asking also makes the object hold its activations back
		if (r && r->script >= 0)
			w.scripts[r->script].suppress = true;
		if (r && r->script >= 0 && w.scripts[r->script].activated)
		{
			w.scripts[r->script].activated = false;
			w.scripts[r->script].buffered = true;
			return 1.0f;
		}
		return 0.0f;
	}
	// Activate: OpenMW does it only for something the player tried to activate while its script held the activation
	// back (OnActivate), and for containers and actors
	case F_ACTIVATE:
		if (r && r->script >= 0)
			w.scripts[r->script].suppress = false;
		if (r && ((r->script >= 0 && w.scripts[r->script].buffered) || r->recType == REC_CONT || r->actor >= 0))
			c.host.activate(ref);
		return 0.0f;
	// Which weapon hit this object last (asked once: the flag clears)
	case F_HITONME:
		if (r && !r->hitBy.empty() && r->hitBy == lower(argStr(c, n, 0)))
		{
			r->hitBy.clear();
			return 1.0f;
		}
		return 0.0f;
	// GetEffect sEffectX (or its number): whether that magic effect is running on the player or the reference
	// (whatever its magnitude; creatures and people too: their abilities and the spells on them)
	case F_GETEFFECT:
	{
		int e = magicEffectIndex(argStr(c, n, 0));
		if (e < 0)
			return 0.0f;
		if (onPlayer || !r)
		{
			for (auto& a : w.effects)
				if (a.effect == e)
					return 1.0f;
			for (auto& a : w.abilities)
				if (a.effect == e)
					return 1.0f;
			return 0.0f;
		}
		if (r->actor < 0)
			return 0.0f;
		for (auto& t : r->effects)
			if (t.effect == e)
				return 1.0f;
		for (auto& k : w.game.actors[r->actor].constEffects)
			if (k.first == e)
				return 1.0f;
		return 0.0f;
	}
	// GetSpellEffects id: a spell, potion or enchanted item's effects are running on the player (kept by name here)
	case F_GETSPELLEFFECTS:
	{
		std::string id = lower(argStr(c, n, 0));
		auto sp = w.game.spells.find(id);
		const Object* o = sp == w.game.spells.end() ? w.game.object(id) : nullptr;
		if ((sp == w.game.spells.end() && !o) || (!onPlayer && r))
			return 0.0f;
		std::string name = sp != w.game.spells.end() ? sp->second.name : o->name.str();
		for (auto& a : w.effects)
			if (a.source == name)
				return 1.0f;
		for (auto& a : w.abilities)
			if (a.source == name)
				return 1.0f;
		return 0.0f;
	}
	case F_GOTOJAIL: c.host.goToJail(); return 0.0f;
	// Cast spell target: the spell's effects land on the target (the player or an NPC)
	case F_CAST:
	{
		auto sp = w.game.spells.find(lower(argStr(c, n, 0)));
		if (sp == w.game.spells.end())
			return 0.0f;
		// OpenMW: the player only readies the spell
		if (onPlayer)
		{
			w.stats.selectedSpell = sp->first;
			return 0.0f;
		}
		std::string target = lower(argStr(c, n, 1));
		int t = target == "player" ? -1 : w.findRef(target);
		if (t < 0 && target != "player")
			return 0.0f;
		// an actor casts it (Reflect can send it back); an object's spell (a shrine's blessing) comes from someone else
		// too, as OpenMW's caster is the object: Spell Absorption takes it (an Atronach's magicka back at a shrine, issue
		// #63), Reflect sends it back to nothing. It had no caster at all: neither ever rolled
		int caster = r && r->actor >= 0 ? ref : r ? kCasterObject : -1;
		if (caster == kCasterObject)
			c.host.objectCastShown(ref, t, sp->first);
		for (auto& e : sp->second.effects)
			c.host.castEffect(t, e, sp->second.name, caster, (float)sp->second.cost);
		return 0.0f;
	}
	case F_ENABLE: w.setEnabled(ref, true); return 0.0f;
	case F_DISABLE: w.setEnabled(ref, false); return 0.0f;
	case F_GETDISABLED: return r && !r->enabled ? 1.0f : 0.0f;
	// StartScript name: global, with the reference it is called on (or the one the calling script runs on) as its
	// own. StopScript only stops global scripts (a local one can't stop itself)
	case F_STARTSCRIPT: w.startGlobalScript(argStr(c, n, 0), onPlayer ? -1 : ref); return 0.0f;
	case F_STOPSCRIPT: w.stopGlobalScript(argStr(c, n, 0)); return 0.0f;
	case F_SCRIPTRUNNING:
	{
		std::string name = lower(argStr(c, n, 0));
		for (auto& s : w.scripts)
			if (s.running && s.ref < 0 && s.item.empty() && lower(s.script->name) == name)
				return 1.0f;
		return 0.0f;
	}
	case F_MESSAGEBOX: messageBox(c, n); return 0.0f;
	case F_GETBUTTONPRESSED: return (float)c.host.takeButtonPressed();
	case F_XBOX: return 1.0f;           // console wording: "Press A to ..."
	case F_GETSECONDSPASSED: return c.dt;
	// Locked is a positive level; Unlock keeps the level as a negative one (OpenMW keeps it too), so Lock with no
	// level locks again at it, or at 100 for what never had one. A level of 0 or less given still locks (as 1).
	// Locking a door shuts it at once (not a teleport door)
	case F_UNLOCK: if (r && r->lockLevel > 0) r->lockLevel = -r->lockLevel; return 0.0f;
	case F_LOCK:
	{
		if (!r)
			return 0.0f;
		int level = r->lockLevel < 0 ? -r->lockLevel : r->lockLevel;
		if (n.kids.size() > 0)
			level = std::max(1, (int)argNum(c, n, 0));
		r->lockLevel = level ? level : 100;
		if (r->recType == REC_DOOR && !r->hasDest)
			r->doorAngle = r->doorTarget = 0.0f;
		return 0.0f;
	}
	case F_GETLOCKED: return r && r->lockLevel > 0 ? 1.0f : 0.0f;
	// The player standing on this object (OpenMW's getPlayerStandingOn; World::feetOnObject)
	case F_GETSTANDINGPC:
		return r && w.player.onGround && w.feetOnObject(ref, w.player.feet) ? 1.0f : 0.0f;
	case F_RANDOM: return (float)(rand() % (int)fmaxf(1.0f, argNum(c, n, 0, 100.0f)));
	case F_GETCURRENTTIME: return fmodf(w.gameHour, 24.0f);
	case F_FALL: return 0.0f;       // (OpenMW runs nothing for Fall)
	case F_FADEOUT: c.host.fadeTo(1.0f, argNum(c, n, 0, 1)); return 0.0f;
	case F_FADEIN: c.host.fadeTo(0.0f, argNum(c, n, 0, 1)); return 0.0f;
	case F_PLAYBINK:
		if (!c.host.playMovie(argStr(c, n, 0)) && lower(argStr(c, n, 0)).find("end") != std::string::npos)
			c.host.notify("Dagoth Ur is defeated, the Heart of Lorkhan severed. The Blight lifts from Vvardenfell.");
		return 0.0f;
	case F_TGM: case F_TOGGLEGODMODE:
		c.host.report(c.host.toggleGodMode() ? "God Mode -> On" : "God Mode -> Off");
		return 0.0f;
	// ShowVars: the object's script variables, else the globals (sorted) and the running global scripts' (OpenMW)
	case F_SV: case F_SHOWVARS:
	{
		std::string out;
		char buf[160];
		auto vars = [&](const ScriptInstance& si) {
			for (size_t k = 0; k < si.locals.size() && k < si.script->localNames.size(); k++)
			{
				char t = k < si.script->localTypes.size() ? si.script->localTypes[k] : 'f';
				snprintf(buf, sizeof(buf), "\n  %s = %g (%s)", si.script->localNames[k].c_str(), si.locals[k],
					t == 's' ? "short" : t == 'l' ? "long" : "float");
				out += buf;
			}
		};
		if (r)
		{
			if (r->script < 0 || !w.scripts[r->script].script)
				out = r->id.str() + " does not have a script.";
			else
			{
				out = "Local variables for " + r->id.str();
				vars(w.scripts[r->script]);
			}
		}
		else
		{
			out = "Global Variables:";
			std::vector<std::string> names;
			for (auto& g : w.globals)
				names.push_back(g.first);
			std::sort(names.begin(), names.end());
			for (auto& nm : names)
			{
				auto def = w.game.globals.find(nm);
				char t = def != w.game.globals.end() ? def->second.type : 'f';
				snprintf(buf, sizeof(buf), "\n %s = %g (%s)", nm.c_str(), w.globals[nm],
					t == 's' ? "short" : t == 'l' ? "long" : "float");
				out += buf;
			}
			out += "\nGlobal Scripts:";
			for (auto& si : w.scripts)
				if (si.ref < 0 && si.item.empty() && si.running && si.script && !si.locals.empty())
				{
					out += "\n " + si.script->name;
					vars(si);
				}
		}
		c.host.report(out);
		return 0.0f;
	}
	// ORI / BC: about the object (OpenMW's BetaComment, cut to what this engine keeps)
	case F_ORI: case F_OUTPUTREFINFO: case F_BC: case F_BETACOMMENT:
	{
		if (!r)
			return 0.0f;
		char buf[200];
		const LevelCell& cell = w.cells[w.placeOf(ref)];
		snprintf(buf, sizeof(buf), "RefID: %s\nCell: %s\nCoordinates: %.1f %.1f %.1f\nRotation: %.1f\nScript: %s%s%s",
			r->id.c_str(), cell.name.c_str(), r->pos[0], r->pos[1], r->pos[2], r->rot[2] * 57.29578f,
			r->script >= 0 && w.scripts[r->script].script ? w.scripts[r->script].script->name.c_str() : "none",
			!r->enabled ? "\n[Disabled]" : "", r->dead ? "\n[Dead]" : "");
		c.host.report(buf);
		return 0.0f;
	}
	// Help: the functions the engine runs (OpenMW lists its keywords)
	case F_HELP:
	{
		std::vector<std::string> names(kFunctionNames + 1, kFunctionNames + F_COUNT);
		std::sort(names.begin(), names.end());
		std::string out = "Script functions (also Get / Set / Mod + attributes, skills, Health, Magicka, Fatigue):";
		std::string line;
		for (auto& nm : names)
		{
			if (line.size() + nm.size() > 44)
			{
				out += "\n" + line;
				line.clear();
			}
			line += (line.empty() ? "" : " ") + nm;
		}
		c.host.report(out + "\n" + line);
		return 0.0f;
	}
	case F_ENABLETELEPORTING: case F_DISABLETELEPORTING:
		w.teleportDisabled = n.func == F_DISABLETELEPORTING;
		return 0.0f;
	// Tribunal's Mournhold: no levitating there. Levitate on anyone ends at once while it is off, the player told so
	// (mwmechanics/activespells.cpp; OpenMW's World::enableLevitation and shouldRemoveEffect)
	case F_ENABLELEVITATION: case F_DISABLELEVITATION:
		w.levitationDisabled = n.func == F_DISABLELEVITATION;
		return 0.0f;
	case F_GETSQUAREROOT:
	{
		// (OpenMW stops the script on a negative number; it reads 0 here, logged)
		float v = argNum(c, n, 0);
		if (v < 0.0f)
		{
			logf("script: GetSquareRoot of %g", v);
			return 0.0f;
		}
		return sqrtf(v);
	}
	// The player, or a person or creature, touching this object from the side this frame (OpenMW's GetCollidingPC,
	// GetCollidingActor); standing on top is GetStandingPC / GetStandingActor
	case F_GETCOLLIDINGPC:
		return r && r->hasBox && r->visible() && touchesBox(*r, w.player.feet, kPlayerBodyRadius, kPlayerBodyHeight) ? 1.0f : 0.0f;
	case F_GETCOLLIDINGACTOR:
	{
		if (!r || !r->hasBox || !r->visible())
			return 0.0f;
		bool any = false;
		w.forLoadedActors([&](int i) {
			const Ref& a = w.refs[i];
			if (i != ref && !a.dead && a.visible() && a.actor >= 0 && touchesBox(*r, a.pos, kActorBodyRadius, kActorBodyHeight))
				any = true;
		});
		return any ? 1.0f : 0.0f;
	}
	// Anyone standing on this object: a person or creature, or the player (OpenMW's getActorStandingOn)
	case F_GETSTANDINGACTOR:
	{
		if (!r || !r->hasBox || !r->visible())
			return 0.0f;
		bool any = w.player.onGround && w.feetOnObject(ref, w.player.feet);
		w.forLoadedActors([&](int i) {
			const Ref& a = w.refs[i];
			if (i != ref && !a.dead && a.visible() && a.actor >= 0 && w.feetOnObject(ref, a.pos))
				any = true;
		});
		return any ? 1.0f : 0.0f;
	}
	// Sotha Sil's spikes and blades: whoever touches the object loses this much health a second (a negative one heals);
	// nothing while a menu is up (OpenMW's hurtCollidingActors)
	case F_HURTCOLLIDINGACTOR:
	{
		if (!r || !r->hasBox || !r->visible() || c.host.menuMode())
			return 0.0f;
		float rate = argNum(c, n, 0);
		if (touchesBox(*r, w.player.feet, kPlayerBodyRadius, kPlayerBodyHeight))
		{
			if (rate > 0.0f)
				c.host.hurtPlayer(rate * c.dt);
			else
				w.stats.health = fminf(w.stats.healthMax, w.stats.health - rate * c.dt);
		}
		w.forLoadedActors([&](int i) {
			Ref& a = w.refs[i];
			if (i == ref || a.dead || !a.visible() || a.actor < 0 || !touchesBox(*r, a.pos, kActorBodyRadius, kActorBodyHeight))
				return;
			a.health = fminf(a.healthMax > 0.0f ? a.healthMax : a.health, a.health - rate * c.dt);
			if (a.health <= 0.0f)
				c.host.killActor(i);
		});
		return 0.0f;
	}
	// The player's or a person's weapon (or fists) out and ready (OpenMW: the draw state is Weapon)
	case F_GETWEAPONDRAWN:
		return c.host.isWeaponDrawn(onPlayer || !r ? -1 : ref) ? 1.0f : 0.0f;
	// SetDelete 1: gone for good, as disabled (and saved so); 0 brings it back (OpenMW's deleteObject / undeleteObject)
	case F_SETDELETE:
		if (r && !onPlayer)
			w.setEnabled(ref, argNum(c, n, 0) == 0.0f);
		return 0.0f;
	// ExplodeSpell id: the player only readies it, a person casts it; an object (Tribunal's dart traps) sets its area
	// effects off where it stands, always succeeding (OpenMW's ExplodeSpell)
	case F_EXPLODESPELL:
	{
		std::string id = lower(argStr(c, n, 0));
		auto sp = w.game.spells.find(id);
		if (sp == w.game.spells.end())
		{
			c.host.report("spellcasting failed: cannot find spell \"" + id + "\"");
			return 0.0f;
		}
		if (onPlayer || !r)
			w.stats.selectedSpell = sp->first;
		else
			c.host.explodeSpell(ref, sp->first);
		return 0.0f;
	}
	case F_DONTSAVEOBJECT:
		return 0.0f;
	default:
		return std::nullopt;
	}
}
