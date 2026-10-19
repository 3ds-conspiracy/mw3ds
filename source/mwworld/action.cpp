// Activating things: what the player's A does to the target, and why it is refused
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include <malloc.h>

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
	if (r.type != "DOOR" && werewolfRefused())
		return;
	// (a light that can't be carried is part of the scenery: nothing happens)
	if (r.type == "LIGH" && r.obj && !(r.obj->flags & 2))
		return;
	if ((r.type == "NPC_" || r.type == "CREA") && r.dead)
	{
		// Search the body
		containerRef = ref;
		openScreen(SCR_CONTAINER);
		return;
	}
	// Creatures that trade or have words of their own are talked to like people (Creeper, the mudcrab
	// merchant, Vivec, Yagrum Bagarn); other living creatures do nothing
	// (one that is only running away still is: OpenMW bug 7631, Creeper and the mudcrab merchant hit and fleeing)
	bool talkingCreature = r.type == "CREA" && r.actor >= 0 && (r.ai != AI_COMBAT || r.fleeing)
		&& ((w.game.actors[r.actor].services & 0x3FFFF) || w.game.speakers.count(r.idLower));
	if (r.type == "CREA" && !r.dead)
		logf("activate: creature %s: services %x, speaker %d, ai %d -> %s", r.idLower.c_str(),
			r.actor >= 0 ? w.game.actors[r.actor].services : 0, (int)w.game.speakers.count(r.idLower), r.ai,
			talkingCreature ? "talk" : "nothing");
	if (r.type == "NPC_" && r.ai == AI_COMBAT)
	{
		notify(w.game.gmst("sactorincombat", "This character is in combat."));
		return;
	}
	if (r.type == "CREA" && !talkingCreature)
		return;
	// Knocked down (not fighting): their pockets are open to anyone, no roll against them. Sneaking up on someone: their
	// pockets, with the pickpocket rolls (OpenMW checks both before talking)
	if (r.type == "NPC_" && r.knockTimer > 0.0f && w.actorEffect(ref, 45) <= 0.0f)
	{
		containerRef = ref;
		pickpocketing = false;
		openScreen(SCR_CONTAINER);
		return;
	}
	if (r.type == "NPC_" && w.player.sneaking && !r.ally)
	{
		containerRef = ref;
		pickpocketing = true;
		openScreen(SCR_CONTAINER);
		return;
	}
	if (r.type == "NPC_" || talkingCreature)
	{
		// Face the speaker's head while talking
		float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
		float dz = r.pos[2] + 115.0f - (w.player.feet[2] + PLAYER_EYE_HEIGHT);
		w.player.yaw = atan2f(dx, dy);
		w.player.pitch = atan2f(dz, fmaxf(1.0f, sqrtf(dx * dx + dy * dy)));
		// With no greeting that fits there is no conversation (OpenMW)
		if (dialogueStart(dlg, w, *this, ref))
		{
			wordsRevision = -1;
			openScreen(SCR_DIALOGUE);
		}
		return;
	}
	if (r.type == "DOOR")
	{
		useDoor(ref);
		return;
	}
	if (r.type == "CONT")
	{
		if (lockGate(ref, "LockedChest") != 2)
			return;
		containerRef = ref;
		openScreen(SCR_CONTAINER);
		// plants and other organic containers open without a chest's creak
		if (!r.obj || !r.obj->organic)
			playSound(ref, "chest open");
		return;
	}
	if (r.type == "BOOK")
	{
		openScreen(SCR_BOOK);
		bookRef = ref;
		playSound(-1, r.obj && r.obj->scroll ? "scroll" : "Book Open");
		return;
	}
	if (r.type == "ACTI" || r.type == "STAT")
		return;
	pickUp(ref);
}

std::string Session::targetLabel(int ref)
{
	Ref& r = w.refs[ref];
	std::string name = r.obj ? r.obj->name.str() : r.id.str();
	if (r.actor >= 0)
		name = w.game.actors[r.actor].name + (r.dead ? " (dead)" : "");
	if (r.type == "DOOR" && r.hasDest && lower(r.destCell) != lower(w.cellName()))
		name += " - " + w.game.cellLabel(r.destCell);
	if (r.lockLevel > 0)
		name += " - Locked";
	if (r.count > 1)
		name += " (" + std::to_string(r.count) + ")";
	return name;
}
