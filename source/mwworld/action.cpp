// Activating things: what the player's A does to the target, and why it is refused
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include <malloc.h>
#include "mwbase/sessionhelpers.hpp"
#include "mwclass/classes.hpp"

void Session::playerActivate(int ref)
{
	Ref& r = w.refs[ref];
	// Scripts that test OnActivate take over; they call Activate for the default action. One that stopped asking
	// (a door script that finished, like the Census office's exit) lets the activation through (OpenMW)
	if (r.script >= 0 && w.scripts[r.script].script->usesOnActivate && w.scripts[r.script].suppress)
	{
		w.scripts[r.script].activated = true;
		w.scripts[r.script].buffered = true;
		return;
	}
	activate(ref);
}

// A werewolf can't use things (OpenMW's sWerewolfRefusal)
bool Session::werewolfRefused()
{
	if (!werewolf)
		return false;
	notify(w.game.gmst("swerewolfrefusal", "You cannot use items while in werewolf form."));
	return true;
}

// Reading a book from the pack in a fight is refused (a book on the ground may still be read)
bool Session::readRefused()
{
	if (!enemiesNear())
		return false;
	notify(w.game.gmst("sinventorymessage4", "You cannot read during battle."));
	return true;
}

// ShowRestMenu on a bed (OpenMW's sleepInBed): a werewolf and enemies nearby are refused; an owned bed is a trespass, and
// when someone saw it and reports it the sleep is refused too
bool Session::bedRefused(int bed)
{
	if (werewolf)
	{
		notify(w.game.gmst("swerewolfrefusal", "You cannot use items while in werewolf form."));
		return true;
	}
	if (enemiesNear())
	{
		notify(w.game.gmst("snotifymessage2", "You can't rest here enemies are nearby."));
		return true;
	}
	if (!w.ownedByOther(bed))
		return false;
	int witness = crimeWitness();
	if (witness < 0)
		return false;
	crimeSeen(CRIME_TRESPASS, 0, -1);
	reportCrime(witness, crimeBounty(CRIME_TRESPASS, 0));
	notify(w.game.gmst("snotifymessage64", "You can't sleep in someone else's bed!"));
	return true;
}

void Session::activate(int ref)
{
	Ref& r = w.refs[ref];
	if (!r.visible())
		return;
	// A werewolf is refused by everything but doors (OpenMW's getWerewolfRefusalAction)
	if (r.recType != REC_DOOR && werewolfRefused())
		return;
	// what it does is its type's: talk, search, open, read, take ... (mwclass/)
	MWClass::activate(*this, ref);
}

std::string Session::targetLabel(int ref)
{
	Ref& r = w.refs[ref];
	std::string name = r.obj ? r.obj->name.str() : r.id.str();
	if (r.actor >= 0)
		name = w.game.actors[r.actor].name + (r.dead ? " (dead)" : "");
	if (r.recType == REC_DOOR && r.hasDest && lower(r.destCell) != lower(w.cellName()))
		name += " - " + w.game.cellLabel(r.destCell);
	if (r.lockLevel > 0)
		name += " - Locked";
	if (r.count > 1)
		name += " (" + std::to_string(r.count) + ")";
	return name;
}

void Session::aimAndActivate(const PlayerInput& in, bool menu)
{
	float eye[3] = { w.player.feet[0], w.player.feet[1], playerEyeZ(w.player) };
	float cp = cosf(w.player.pitch);
	float dir[3] = { sinf(w.player.yaw) * cp, cosf(w.player.yaw) * cp, sinf(w.player.pitch) };
	memcpy(listener, eye, sizeof(eye));
	listener[3] = w.player.yaw;

	// Telekinesis reaches farther (magnitude in feet, 22 units each)
	target = menu ? -1 : worldPick(w, eye, dir, kReach + w.effectTotal(59) * 22.0f);
	// (GoldenEye: on a tap of A, let go before it aimed; the press itself is kept from the screens either way)
	bool goldenEye = g_controlLayout == CONTROLS_GOLDENEYE;
	if (!menu && ((uiIn().down & KEY_A) || (goldenEye && in.activate)))
	{
		uiConsume(KEY_A);
		if ((goldenEye ? in.activate : true) && target >= 0 && w.controlsEnabled)
			playerActivate(target);
	}
}
