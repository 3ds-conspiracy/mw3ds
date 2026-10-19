// Tokens: bones, poses, view, faces
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
#include "platform/gamma.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"
#include "mwsound/soundmanagerimp.hpp"
#include "platform/console.hpp"

extern const char* g_playerBlock;
#include "mwgui/windowmanagerimp.hpp"
#include "mwphysics/physicssystem.hpp"
#include "platform/linear.hpp"
#include "platform/memstat.hpp"
#include "components/files/zfile.hpp"
#include <malloc.h>
#include "testdrive/testhelpers.hpp"
#include "testdrive/opshelpers.hpp"

std::optional<bool> opsPose(Session& s, const std::vector<std::string>& a)
{
	World& w = s.w;
	const std::string& verb = a[0];
	auto fail = [&](const std::string& why) { logf("drive: FAIL %s: %s", verb.c_str(), why.c_str()); return true; };
	// POSE:<id>:<group>:<at>[:<yaw degrees>]: the actor back where the level placed it (or turned that way), holding that
	// animation group at that point (0 its start key, 1 its stop key: OpenMW's startPoint), no AI, no head turning. For
	// side-by-side screenshots with OpenMW (tools/test/sidebyside.py)
	// POSE:<id>:off: let it go again (its AI, its animations)
	if (verb == "BONES" && a.size() >= 2)
	{
		// BONES:<id>: each bone of that actor where its pose has it now (skeleton space), logged
		int ri = findRef(s, a[1]);
		Actor* ac = ri >= 0 && w.active(ri) ? w.actorOf(ri) : nullptr;
		if (!ac)
			return fail(a[1] + " not loaded");
		const Skeleton& sk = actorSkeleton(*w.actorsOf(ri), ac->skeleton);
		logf("test: %s bones in %s at %.3f", a[1].c_str(), sk.groups[ac->group].name, ac->time);
		for (size_t b = 0; b < sk.bones.size() && (b + 1) * 12 <= ac->pose.size(); b++)
		{
			const float* m = &ac->pose[b * 12];
			logf("test:   %-28s %8.1f %8.1f %8.1f  scale %.2f", sk.bones[b].name, m[3], m[7], m[11],
				sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]));
		}
		// and its meshes: bone, flags, shown, where the first vertex is drawn (world)
		for (size_t k = 0; k < ac->meshes.size(); k++)
		{
			const ActorMesh& me = ac->meshes[k];
			C3D_Mtx mm;
			actorMeshMatrix(*ac, me, &mm);
			float p[3] = { me.src.size() >= 3 ? me.src[0] : 0.0f, me.src.size() >= 3 ? me.src[1] : 0.0f, me.src.size() >= 3 ? me.src[2] : 0.0f };
			float wx = mm.r[0].x * p[0] + mm.r[0].y * p[1] + mm.r[0].z * p[2] + mm.r[0].w;
			float wy = mm.r[1].x * p[0] + mm.r[1].y * p[1] + mm.r[1].z * p[2] + mm.r[1].w;
			float wz = mm.r[2].x * p[0] + mm.r[2].y * p[1] + mm.r[2].z * p[2] + mm.r[2].w;
			logf("test:   mesh %2u bone %-24s flags %02x %s %s verts %lu at %.0f %.0f %.0f", (unsigned)k,
				me.bone >= 0 && me.bone < (int)sk.bones.size() ? sk.bones[me.bone].name : "(root)", me.flags,
				actorMeshShown(*ac, me) ? "shown" : "hidden", me.palBone.empty() ? "rigid" : "skinned",
				(unsigned long)me.numVerts, wx, wy, wz);
		}
		return true;
	}
	if (verb == "POSE" && a.size() == 3 && a[2] == "off")
	{
		int ri = findRef(s, a[1]);
		Actor* ac = ri >= 0 && w.active(ri) ? w.actorOf(ri) : nullptr;
		if (!ac)
			return fail(a[1] + " not loaded");
		ac->frozen = false;
		actorPlay(*w.actorsOf(ri), *ac, "Idle", ANIM_IDLE);
		return true;
	}
	if (verb == "POSE" && a.size() >= 4)
	{
		int ri = findRef(s, a[1]);
		// (it goes back to where it was placed: of several with the id, the one placed nearest the player, as the
		// side-by-side tool's OpenMW half picks it, not the one that has wandered or swum nearest)
		if (ri >= 0 && ri != s.testPlaced)
		{
			const std::string want = w.refs[ri].idLower;
			const float* pf = w.player.feet;
			float best = 1e30f;
			w.forLoadedRefs([&](int i) {
				const Ref& c = w.refs[i];
				float dx = c.start[0] - pf[0], dy = c.start[1] - pf[1], dz = c.start[2] - pf[2], d = dx * dx + dy * dy + dz * dz;
				if (c.idLower == want && !c.dead && w.active(i) && d < best)
				{
					best = d;
					ri = i;
				}
			});
		}
		Actor* ac = ri >= 0 && w.active(ri) ? w.actorOf(ri) : nullptr;
		ActorSet* set = ri >= 0 ? w.actorsOf(ri) : nullptr;
		if (!ac || !set)
			return fail(a[1] + " not loaded");
		const Skeleton& sk = actorSkeleton(*set, ac->skeleton);
		int g = -1;
		for (size_t k = 0; k < sk.groups.size() && g < 0; k++)
			if (strncasecmp(sk.groups[k].name, a[2].c_str(), sizeof(sk.groups[k].name)) == 0)
				g = (int)k;
		if (g < 0)
			return fail(a[1] + " has no group " + a[2]);
		// (an actor PLACE / PLACEAT put somewhere: back where it was put, at the height the floor gave it)
		Ref& r = w.refs[ri];
		if (ri == s.testPlaced || s.testPlacedAll.count(ri))
		{
			r.pos[0] = r.home[0];
			r.pos[1] = r.home[1];
		}
		else
			memcpy(r.pos, r.start, sizeof(r.pos));
		r.rot[2] = a.size() >= 5 ? (float)atof(a[4].c_str()) / 57.29578f : r.start[3];
		r.fitBox();
		r.moved = true;
		r.wandering = false;
		w.syncActor(ri);
		ac->frozen = false;
		actorPlay(*set, *ac, sk.groups[g].name, ANIM_HOLD);
		float at = fmaxf(0.0f, fminf(1.0f, (float)atof(a[3].c_str())));
		ac->time = sk.groups[g].start + at * (sk.groups[g].stop - sk.groups[g].start);
		ac->headYaw = 0.0f;
		ac->headPitch = 0.0f;
		ac->blinkTime = -1.0f;
		ac->frozen = true;
		logf("test: %s holds %.16s at %.2f (%.3f s; skeleton %d, %.3f - %.3f)", a[1].c_str(), sk.groups[g].name, at,
			ac->time - sk.groups[g].start, ac->skeleton, sk.groups[g].start, sk.groups[g].stop);
		return true;
	}
	// VIEWSCALE:<0..1>: the adaptive view distance set there; ADAPTVIEW:<work ms>:<frame ms>:<frames>: that many frames
	// of that measured work arriving that far apart handed to Session::adaptView, as main's loop does after each frame
	// (it leaves the view alone while a script runs: these stand in for the console's frames)
	if (verb == "VIEWSCALE" && a.size() >= 2)
	{
		s.viewScale = s.viewDrawn = (float)atof(a[1].c_str());     // (drawn as set: the step hysteresis starts from it)
		return true;
	}
	if (verb == "ADAPTVIEW" && a.size() >= 4)
	{
		float work = (float)atof(a[1].c_str()), frame = (float)atof(a[2].c_str());
		for (int n = atoi(a[3].c_str()); n > 0; n--)
			s.adaptView(work, frame);
		logf("test: %d frames of %.1f ms work %.1f ms apart: view %.0f%%", atoi(a[3].c_str()), work, frame, s.viewScale * 100.0f);
		return true;
	}
	// POV:first|third: the view, as SELECT toggles it (side-by-side shots seen from elsewhere want the player's body)
	if (verb == "POV" && a.size() >= 2)
	{
		bool third = a[1] == "third";
		if (s.thirdPerson != third)
		{
			s.thirdPerson = third;
			s.thirdDistance = 0.0f;
			s.body.acting = BODY_NONE;
		}
		return true;
	}
	// VMPOSE:<group>:<at>: the first-person arms hold that group (CastSelf, 1hChop, ...) at that point, 0 its start, 1 its
	// stop (side-by-side screenshots, tools/test/sidebyside.py). VMPOSE:off lets them go
	if (verb == "VMPOSE" && a.size() >= 2)
	{
		if (!s.vm.ready)
			return fail("no first-person view model");
		Actor& arms = s.vm.set.actors[0];
		arms.frozen = false;
		if (a[1] == "off")
		{
			s.vm.action = VM_NONE;
			return true;
		}
		if (a.size() < 3)
			return fail("VMPOSE wants a group and a point");
		const Skeleton& sk = s.vm.set.skeletons[0];
		int g = -1;
		for (size_t k = 0; k < sk.groups.size() && g < 0; k++)
			if (strncasecmp(sk.groups[k].name, a[1].c_str(), sizeof(sk.groups[k].name)) == 0)
				g = (int)k;
		if (g < 0)
			return fail("the first-person skeleton has no group " + a[1]);
		// a cast: the arms bare, as a cast shows them (viewModelUpdate drops the weapon while the action is a cast);
		// anything else (1hChop, BowShoot, Block, ...): the weapon (or fists) out, as a blow draws it
		bool cast = strncasecmp(a[1].c_str(), "Cast", 4) == 0 || strncasecmp(a[1].c_str(), "Spell", 5) == 0;
		s.vm.play(cast ? VM_CAST : VM_WINDUP, sk.groups[g].name);
		if (!cast)
		{
			s.weaponDrawn = true;
			s.sheatheTimer = 0.0f;
		}
		float at = fmaxf(0.0f, fminf(1.0f, (float)atof(a[2].c_str())));
		arms.time = sk.groups[g].start + at * (sk.groups[g].stop - sk.groups[g].start);
		arms.frozen = true;
		logf("test: arms hold %.16s at %.2f (%.3f s)", sk.groups[g].name, at, arms.time - sk.groups[g].start);
		return true;
	}
	// BODYPOSE:<group>:<at>: the player's body (third person) holds that NPC group (Attack1h, AttackBow, Block, ...) at
	// that point, the weapon out unless it is a cast (side-by-side screenshots). BODYPOSE:off lets it go
	if (verb == "BODYPOSE" && a.size() >= 2)
	{
		if (!s.body.ready || s.body.set.actors.empty())
			return fail("no third-person body (POV:third first)");
		Actor& body = s.body.set.actors[0];
		body.frozen = false;
		s.body.acting = BODY_NONE;
		s.body.group = "";
		if (a[1] == "off")
			return true;
		if (a.size() < 3)
			return fail("BODYPOSE wants a group and a point");
		const Skeleton& sk = actorSkeleton(s.body.set, body.skeleton);
		int g = -1;
		for (size_t k = 0; k < sk.groups.size() && g < 0; k++)
			if (strncasecmp(sk.groups[k].name, a[1].c_str(), sizeof(sk.groups[k].name)) == 0)
				g = (int)k;
		if (g < 0)
			return fail("the player's skeleton has no group " + a[1]);
		if (strncasecmp(a[1].c_str(), "Cast", 4) != 0)
		{
			s.weaponDrawn = true;
			s.sheatheTimer = 0.0f;
		}
		actorPlay(s.body.set, body, sk.groups[g].name, ANIM_HOLD);
		float at = fmaxf(0.0f, fminf(1.0f, (float)atof(a[2].c_str())));
		body.time = sk.groups[g].start + at * (sk.groups[g].stop - sk.groups[g].start);
		body.headYaw = 0.0f;
		body.blinkTime = -1.0f;
		body.frozen = true;
		logf("test: body holds %.16s at %.2f (%.3f s)", sk.groups[g].name, at, body.time - sk.groups[g].start);
		return true;
	}
	// FACE:@x,y: turn to look at that spot (level), as the stick would
	if (verb == "FACE" && a.size() >= 2 && a[1][0] == '@')
	{
		float x, y;
		if (sscanf(a[1].c_str() + 1, "%f,%f", &x, &y) != 2)
			return fail("FACE:@x,y wants two numbers");
		w.player.yaw = atan2f(x - w.player.feet[0], y - w.player.feet[1]);
		w.player.pitch = 0.0f;
		return true;
	}
	if ((verb == "FACE" || verb == "CASTAT") && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0 || !w.active(ri))
			return fail(a[1] + " not loaded");
		const Ref& r = w.refs[ri];
		// (FACE:<ref>:mid: the middle of its box, as the driver aims at a thing to take it; else a person's chest)
		bool mid = verb == "FACE" && a.size() >= 3 && a[2] == "mid";
		float dx = (mid ? (r.boxMin[0] + r.boxMax[0]) * 0.5f : r.pos[0]) - w.player.feet[0];
		float dy = (mid ? (r.boxMin[1] + r.boxMax[1]) * 0.5f : r.pos[1]) - w.player.feet[1];
		float dz = (mid ? (r.boxMin[2] + r.boxMax[2]) * 0.5f : r.pos[2] + 80.0f) - (w.player.feet[2] + PLAYER_EYE_HEIGHT);
		w.player.yaw = atan2f(dx, dy);
		w.player.pitch = atan2f(dz, fmaxf(1.0f, sqrtf(dx * dx + dy * dy)));
		if (verb == "CASTAT" && a.size() >= 3)
		{
			std::string sp = a[2];
			for (size_t k = 3; k < a.size(); k++)
				sp += ":" + a[k];
			// the id as given when there's such a spell (mw3ds_test_x), else with '_' as spaces ("fire_bite")
			w.stats.selectedSpell = sp.compare(0, 5, "item:") == 0 || w.game.spells.count(lower(sp)) ? lower(sp) : lower(spaced(sp));
			if (s.testLegit && !TestDriver::canCast(s, w.stats.selectedSpell))
				return fail(sp + " isn't a known spell or a carried item (LEGIT)");
			s.castSpell();
			logf("test: cast %s at %s", sp.c_str(), a[1].c_str());
		}
		return true;
	}
	return std::nullopt;
}
