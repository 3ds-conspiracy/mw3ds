// EXPECT values: references (lights, locks, traps, scripts, sounds), the view model
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive/testdrive.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"
#include "mwsound/soundmanagerimp.hpp"

extern const char* g_playerBlock;
#include <malloc.h>
#include "testdrive/testhelpers.hpp"

std::optional<bool> numberOfRefs(Session& s, const std::string& what, const std::string& arg, float& v)
{
	World& w = s.w;
	if (what == "lighttime")           // lighttime:<light>: the seconds the carried one has left to burn
	{
		int i = carried(w, arg);
		const Object* o = i >= 0 ? w.game.object(w.inventory[i].id) : nullptr;
		if (!o || o->recType != REC_LIGH)
			return false;
		v = w.inventory[i].charge < 0.0f ? o->lightTime : w.inventory[i].charge;
		return true;
	}
	if (what == "charge")
	{
		const InventoryItem* it = arg == "made" ? lastMade(w) : nullptr;
		int i = it ? -1 : carried(w, arg);
		if (!it && i >= 0)
			it = &w.inventory[i];
		if (!it)
			return false;
		v = w.chargeOf(*it);
		return true;
	}
	if (what == "reftrap")        // reftrap:<door or container id>: 1 while its trap is armed
	{
		int ri = findRef(s, arg);
		if (ri < 0)
			return false;
		v = !w.refs[ri].trap.empty() && !w.refs[ri].disarmed ? 1.0f : 0.0f;
		return true;
	}
	if (what == "reflock" || what == "locklevel")
	{
		// reflock:<door or container id>: its lock level (negative: unlocked, the level kept for Lock)
		// reflock:<cell>: the door leading to that cell (door ids repeat: ex_nord_door_01)
		int ri = -1;
		std::string dest = lower(spaced(arg));
		w.forLoadedRefs([&](int i) {
			if (ri < 0 && w.refs[i].recType == REC_DOOR && w.refs[i].hasDest && lower(w.refs[i].destCell) == dest)
				ri = i;
		});
		const bool byDest = ri >= 0;
		if (ri < 0)
			ri = findRef(s, arg);
		if (ri < 0)
			ri = w.findRefAnywhere(spaced(arg));
		if (ri < 0)
			return false;
		// (several share the id: the most locked one, so a case names the door it means whichever the player stands nearer)
		int most = ri;
		w.forLoadedRefs([&](int i) {
			if (!byDest && w.refs[i].idLower == w.refs[ri].idLower && abs(w.refs[i].lockLevel) > abs(w.refs[most].lockLevel))
				most = i;
		});
		v = (float)w.refs[most].lockLevel;
		return true;
	}
	if (what == "scripttarget")
	{
		// scripttarget:<global script>,<reference id>: 1 when that script is running on that reference (StartScript from it)
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		std::string name = lower(arg.substr(0, comma)), id = lower(arg.substr(comma + 1));
		v = 0;
		for (auto& sc : w.scripts)
			if (sc.ref < 0 && sc.item.empty() && lower(sc.script->name) == name && sc.target >= 0
				&& w.refs[sc.target].idLower == id)
				v = 1;
		return true;
	}
	if (what == "reflocal" || what == "reflocalfrac" || what == "scriptlocal")
	{
		// reflocal:<reference id>,<variable>: a local of that object's script (of the first with the id that isn't 0:
		// several share one id); reflocalfrac: what follows its decimal point (0 for a whole number); scriptlocal:<global
		// script>,<variable>
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		std::string id = lower(arg.substr(0, comma)), var = lower(arg.substr(comma + 1));
		bool found = false;
		v = 0;
		if (what == "scriptlocal")
		{
			for (auto& sc : w.scripts)
				if (!found && sc.ref < 0 && sc.item.empty() && lower(sc.script->name) == id)
					if (float* p = sc.local(var))
						v = *p, found = true;
			return found;
		}
		for (auto& r : w.refs)
			if ((r.idLower == id || r.idLower == lower(spaced(id))) && r.script >= 0)
				if (float* p = w.scripts[r.script].local(var))
				{
					found = true;
					v = *p;
					if (v != 0.0f)
						break;
				}
		if (what == "reflocalfrac")
			v -= truncf(v);
		return found;
	}
	if (what == "soundstarted")
	{
		// soundstarted:<sound id>: how many times that sound has been started (a count even with no audio device)
		auto rec = w.game.sounds.find(lower(spaced(arg)));
		// (or a voice line's file name, as a script says it: vo\w\m\hit_wm006.mp3)
		auto voice = w.game.voices.find(lower(arg));
		v = rec != w.game.sounds.end() ? audioStartedCount(rec->second.file)
			: voice != w.game.voices.end() ? audioStartedCount(voice->second) : -1;
		return true;
	}
	if (what == "movieplaying")
	{
		v = s.moviePlaying() ? 1.0f : 0.0f;
		return true;
	}
	if (what == "vmmeshes")           // meshes the first-person view model draws now (arms, then what they hold)
	{
		v = s.vm.ready && !s.vm.set.actors.empty() ? (float)s.vm.set.actors[0].meshes.size() : 0.0f;
		return true;
	}
	if (what == "vmpiece")            // vmpiece:<text>: 1 if a first-person piece's name has that text (robe_extrav)
	{
		v = s.vm.ready && lower(s.vm.builtFor).find(lower(arg)) != std::string::npos ? 1.0f : 0.0f;
		return true;
	}
	if (what == "vmhands")            // the first-person hands (their wrists) in view on the top screen: 0, 1 or 2
	{
		// the renderer's view: 60 degrees high, 400 x 240, near plane 5
		const float tanY = tanf(30.0f / 57.29578f), tanX = tanY * 400.0f / 240.0f;
		v = 0.0f;
		for (const char* hand : { "bip01 l hand", "bip01 r hand" })
		{
			float p[3];
			if (s.vm.boneFromEye(hand, p) && p[1] > 5.0f && fabsf(p[0]) < p[1] * tanX && fabsf(p[2]) < p[1] * tanY)
				v += 1.0f;
		}
		return true;
	}
	if (what == "bowstring")          // how far back the held bow's string is drawn (0 at rest, 1 fully back)
	{
		v = s.vm.ready && !s.vm.set.actors.empty() ? actorMorphWeight(s.vm.set.actors[0]) : 0.0f;
		return true;
	}
	if (what == "swimming")
	{
		v = s.w.player.swimming ? 1.0f : 0.0f;
		return true;
	}
	if (what == "weapondrawn")
	{
		v = s.weaponDrawn ? 1.0f : 0.0f;
		return true;
	}
	// sounddenied: sounds that found every channel busy; loopsounds / loopheld: loops going / holding a channel
	if (what == "sounddenied")
	{
		v = audioDeniedCount();
		return true;
	}
	if (what == "loopsounds" || what == "loopheld")
	{
		v = 0;
		for (auto& l : s.loopSounds)
			if (what == "loopsounds" || l.channel >= 0)
				v++;
		return true;
	}
	if (what == "refloopvol")
	{
		// refloopvol:<reference id>,<sound id>: the volume (x100, before distance) of the sound it loops (PlayLoopSound3D[VP]); -1 none
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		std::string id = lower(arg.substr(0, comma)), snd = lower(spaced(arg.substr(comma + 1)));
		auto rec = w.game.sounds.find(snd);
		v = -1;
		for (auto& l : s.loopSounds)
			if (l.id == snd && w.refs[l.ref].idLower == id)
				v = l.volume * (rec != w.game.sounds.end() ? rec->second.volume : 1.0f) * 100.0f;
		return true;
	}
	if (what == "refgap")
	{
		// refgap:<a>,<b>: how far apart two actors stand, middle to middle, across
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		int ra = findRef(s, arg.substr(0, comma)), rb = findRef(s, arg.substr(comma + 1));
		if (ra < 0 || rb < 0)
			return false;
		float dx = w.refs[ra].pos[0] - w.refs[rb].pos[0], dy = w.refs[ra].pos[1] - w.refs[rb].pos[1];
		v = sqrtf(dx * dx + dy * dy);
		return true;
	}
	if (what == "refitem")
	{
		// refitem:<container or actor>,<item>: how many of the item it holds (AddItem / RemoveItem on it)
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		std::string id = arg.substr(0, comma);
		int ri = s.testFindRef(id);
		if (ri < 0)
			ri = w.findRefAnywhere(spaced(id));
		if (ri < 0)
			return false;
		std::string item = lower(arg.substr(comma + 1)), item2 = spaced(item);
		v = 0;
		for (auto& it : w.refs[ri].contents)
			if (lower(it.second) == item || lower(it.second) == item2)
				v += stockCount(it.first);
		return true;
	}
	if (what == "refcell")
	{
		// refcell:<actor>,<cell>: 1 when that reference stands in a cell whose name begins with that (PositionCell)
		size_t comma = arg.find(',');
		if (comma == std::string::npos)
			return false;
		std::string id = arg.substr(0, comma);
		int ri = s.testFindRef(id);
		if (ri < 0)
			ri = w.findRefAnywhere(spaced(id));
		if (ri < 0)
			return false;
		std::string here = lower(w.cells[w.placeOf(ri)].name), want = lower(spaced(arg.substr(comma + 1)));
		v = here.compare(0, want.size(), want) == 0 ? 1.0f : 0.0f;
		return true;
	}
	if (what == "refspell")
	{
		// refspell:<actor>,<spell>: 1 when that NPC or creature has the spell (AddSpell / RemoveSpell on it)
		size_t comma = arg.find(',');
		int ri = comma == std::string::npos ? -1 : findRef(s, arg.substr(0, comma));
		if (ri < 0)
			return false;
		v = w.actorHasSpell(w.refs[ri], lower(arg.substr(comma + 1))) ? 1.0f : 0.0f;
		return true;
	}
	if (what.compare(0, 3, "ref") == 0)
	{
		int ri = findRef(s, arg);
		if (ri < 0)
			return false;
		const Ref& r = w.refs[ri];
		if (what == "refhealth") v = r.health;
		else if (what == "refrank") v = r.actor >= 0 ? (float)w.game.actors[r.actor].rank : -1.0f;   // RaiseRank / LowerRank
		else if (what == "reflevel") v = r.actor >= 0 ? (float)w.game.actors[r.actor].level : -1.0f;
		else if (what == "reffatigue") v = r.fatigue;
		else if (what == "refmagicka") v = r.magicka;
		else if (what == "refhealthmax") v = r.healthMax;
		else if (what == "refmagickamax") v = r.magickaMax;
		else if (what == "refcommanded") v = r.aiPackage == AIPKG_FOLLOW && r.aiTarget == "player" && !r.aiDone ? 1.0f : 0.0f;
		else if (what == "refdisp") v = (float)w.disposition(ri);
		else if (what == "refflee") v = r.fleeing ? 1.0f : 0.0f;
		else if (what == "refcombat") v = r.ai == AI_COMBAT ? 1.0f : 0.0f;
		else if (what == "refvfx")       // spell visuals kept to it (a cast's while it casts, a hit's)
		{
			v = 0.0f;
			for (auto& x : s.vfx)
				v += x.follow == ri && x.bone.empty();
		}
		else if (what == "refvfxhands")  // glows on its hands while it casts (VFX_Hands, one per hand)
		{
			v = 0.0f;
			for (auto& x : s.vfx)
				v += x.follow == ri && !x.bone.empty();
		}
		else if (what == "reflhandz")    // its left hand's height above its feet (the torch held out raises it)
		{
			float p[3];
			v = s.vfxBonePos(ri, "Bip01 L Hand", p) ? p[2] - r.pos[2] : -1.0f;
		}
		else if (what == "refvfxhandz")  // the highest hand glow above its feet (0 none)
		{
			v = 0.0f;
			for (auto& x : s.vfx)
				if (x.follow == ri && !x.bone.empty())
					v = fmaxf(v, x.pos[2] - r.pos[2]);
		}
		else if (what == "refdying")     // dead, and its fall to the ground still playing
		{
			Actor* ra = w.actorOf(ri);
			v = r.dead && ra && ra->time < actorSkeleton(*w.actorsOf(ri), ra->skeleton).groups[ra->group].stop - 0.05f ? 1.0f : 0.0f;
		}
		else if (what == "refrun")       // running (its run animation plays), not walking
		{
			Actor* ra = w.actorOf(ri);
			// (RunForward, or its weapon stance's RunForward1h ...)
			const Skeleton* rs = ra ? &actorSkeleton(*w.actorsOf(ri), ra->skeleton) : nullptr;
			v = rs && ra->group >= 0 && ra->group < (int)rs->groups.size() && strncmp(rs->groups[ra->group].name, "RunForward", 10) == 0
				? 1.0f : 0.0f;
		}
		else if (what == "refpkg") v = r.aiPackage == AIPKG_IDLE ? -1.0f : (float)r.aiPackage;   // GetCurrentAIPackage
		else if (what == "refpkgdone") v = r.aiDone ? 1.0f : 0.0f;                              // GetAIPackageDone
		else if (what == "refknock") v = r.knockTimer;
		else if (what == "refsidesteps") v = (float)r.sideSteps;     // side steps begun in a fight (OpenMW's combat moves)
		else if (what == "refyaw") v = r.rot[2] * 57.29578f;          // which way it faces, degrees
		else if (what == "refskate") v = s.skates.count(ri) ? s.skates[ri].secs : 0.0f;   // seconds moving while playing an idle
		else if (what == "refidlevoice") v = s.idleVoiceOk(ri) ? 1.0f : 0.0f;   // may say an idle line now
		else if (what == "refheadyaw" || what == "refheadpitch")      // the head turned / tilted, degrees
		{
			const Actor* ha = w.actorOf(ri);
			v = ha ? (what == "refheadyaw" ? ha->headYaw : ha->headPitch) * 57.29578f : 0.0f;
		}
		else if (what == "refsoultrap") v = r.soulTrapUntil > w.time ? 1.0f : 0.0f;
		else if (what == "refally") v = r.ally ? 1.0f : 0.0f;
		else if (what == "refdist") v = w.distanceToPlayer(ri);      // 100000: in another interior
		else if (what == "refaimed") v = s.target == ri ? 1.0f : 0.0f;   // the crosshair is on it
		else if (what == "refabove")        // units over the floor under it, as it is drawn (99999: no floor)
		{
			float floorZ = -1e9f;
			for (LoadedCell* l : w.loaded)
			{
				float z;
				if (collisionFloor(l->cell.collision, r.pos[0], r.pos[1], r.pos[2] + 40.0f, r.pos[2] - 6000.0f, &z) && z > floorZ)
					floorZ = z;
			}
			v = floorZ > -1e8f ? r.pos[2] + r.boxLift - floorZ : 99999.0f;
		}
		else if (what == "refabovefeet")    // units over the highest floor within a foot of it (collisionFootFloor: where it
		{                                   // stands), as it is drawn (a creature's box lift on top, World::actorBoxLift)
			float floorZ = -1e9f;
			for (LoadedCell* l : w.loaded)
			{
				float z;
				if (collisionFootFloor(l->cell.collision, r.pos[0], r.pos[1], kActorFootReach, r.pos[2] + 40.0f, r.pos[2] - 6000.0f, &z)
					&& z > floorZ)
					floorZ = z;
			}
			v = floorZ > -1e8f ? r.pos[2] + r.boxLift - floorZ : 99999.0f;
		}
		else if (what == "refanimtime")     // where its animation is (seconds on its skeleton's timeline): frozen while paralyzed
		{
			const Actor* a = w.actorOf(ri);
			v = a ? a->time : -1.0f;
		}
		else if (what == "reftint")         // the light it was placed in, as its meshes are lit (1: the library's own)
		{
			const Actor* a = w.actorOf(ri);
			v = a ? (a->tint[0] + a->tint[1] + a->tint[2]) / (3.0f * 64.0f) : -1.0f;
		}
		else if (what == "reflampnow")      // the lamps' and others' torches' light where it stands now (Actor::lampNow, 1: 0.5)
		{
			const Actor* a = w.actorOf(ri);
			v = a ? (a->lampNow[0] + a->lampNow[1] + a->lampNow[2]) / (3.0f * 64.0f) : -1.0f;
		}
		else if (what == "refheldlight")    // the light of the torch it holds out, on itself (Actor::heldLight, 0..1)
		{
			const Actor* a = w.actorOf(ri);
			v = a ? (a->heldLight[0] + a->heldLight[1] + a->heldLight[2]) / 3.0f : -1.0f;
		}
		else if (what == "refalpha")        // how solid it is drawn (1; less under Invisibility, Chameleon)
		{
			const Actor* a = w.actorOf(ri);
			v = a ? a->alpha : -1.0f;
		}
		else if (what == "refglow")         // its meshes that shimmer (enchanted armour and clothing)
		{
			v = 0.0f;
			if (const Actor* a = w.actorOf(ri))
				for (const ActorMesh& m : a->meshes)
					v += m.glow ? 1.0f : 0.0f;
		}
		else if (what == "refshield")       // something on its Shield Bone shows now (its shield), 1; else 0
		{
			v = 0.0f;
			if (const Actor* a = w.actorOf(ri))
			{
				const Skeleton& sk = actorSkeleton(*w.actorsOf(ri), a->skeleton);
				for (const ActorMesh& m : a->meshes)
					if (m.bone >= 0 && m.bone < (int)sk.bones.size() && strcasecmp(sk.bones[m.bone].name, "shield bone") == 0
						&& m.hand != 1 && actorMeshShown(*a, m))
						v = 1.0f;
			}
		}
		else if (what == "reftorcharm")     // its left arm holds the torch out (the Torch group, OpenMW's torch pose), 1
		{
			const Actor* a = w.actorOf(ri);
			v = a && a->torchTime >= 0.0f ? 1.0f : 0.0f;
		}
		else if (what == "reftorch")        // its torch shows now (held out at night), 1; else 0
		{
			v = 0.0f;
			if (const Actor* a = w.actorOf(ri))
				for (const ActorMesh& m : a->meshes)
					if (m.hand == 1 && actorMeshShown(*a, m))
						v = 1.0f;
		}
		else if (what == "refcpuskin" || what == "refblendskin" || what == "refmeshes")
		{                                   // its skinned vertices the CPU rewrites each pose (no GPU skin), its blended meshes
			v = 0.0f;                        // skinned on the GPU (each a shader switch), all its meshes
			if (const Actor* a = w.actorOf(ri))
				for (const ActorMesh& m : a->meshes)
					v += what == "refmeshes" ? 1.0f : what == "refcpuskin" ? (m.kind == ACTOR_SKINNED && !m.gpuSkin ? (float)m.numVerts : 0.0f)
						: (m.gpuSkin && (m.flags & BATCH_BLEND) ? 1.0f : 0.0f);
		}
		else if (what == "refclimb")        // the steepest land it walks up now, degrees (npcClimbSlope)
			v = npcClimbSlope(w, w.refs[ri]);
		else if (what == "refz")            // the height of its feet
			v = w.refs[ri].pos[2];
		else if (what == "refdressed")      // 1 once it has been dressed again from the third-person pieces (npcDress)
		{
			const Actor* a = w.actorOf(ri);
			v = a && a->dressed ? 1.0f : 0.0f;
		}
		else if (what == "reftorchfire")    // its torch's fire and smoke quads that flicker and climb as drawn (ActorMesh::rise)
		{
			v = 0.0f;
			if (const Actor* a = w.actorOf(ri))
				for (const ActorMesh& m : a->meshes)
					if (m.hand == 1 && m.rise)
						v += 1.0f;
		}
		else if (what == "refhiddenparts")  // solid meshes of its model its visibility keys hide now (a dead Bonelord's robe;
		{                                   // not its glowing particle sprays, which show only while they burst)
			v = 0.0f;
			if (const Actor* a = w.actorOf(ri))
				for (const ActorMesh& m : a->meshes)
					if (!(m.flags & BATCH_ADDITIVE) && !actorMeshShown(*a, m))
						v += 1.0f;
		}
		else if (what == "refweaponrating") // its weapon's worth in a fight with the player (OpenMW's rateWeapon: a bow with its arrows)
			v = s.npcWeaponRating(ri, w.distanceToPlayer(ri));
		else if (what == "refadditive")     // glowing (additive) parts of its model showing now: flames, sparks
		{
			v = 0.0f;
			if (const Actor* a = w.actorOf(ri))
				for (const ActorMesh& m : a->meshes)
					if ((m.flags & BATCH_ADDITIVE) && actorMeshShown(*a, m))
						v += 1.0f;
		}
		else if (what == "refwater") v = w.here().hasWater() ? r.pos[2] - w.here().waterZ : 99999.0f;   // units over the water here
		else if (what == "refbox") v = r.hasBox ? 1.0f : 0.0f;       // it has a mesh to aim at (a banner's cloth)
		else if (what == "refaimable") v = actorAimable(w, ri) ? 1.0f : 0.0f;   // the crosshair finds them where they stand
		else if (what == "refforcesneak")                                          // ForceSneak on that actor
		{
			auto f = w.moveFlags.find(ri);
			v = f != w.moveFlags.end() && (f->second & 8u) ? 1.0f : 0.0f;
		}
		else return false;
		return true;
	}
	return std::nullopt;
}
