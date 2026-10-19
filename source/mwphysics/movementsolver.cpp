// Moving the player and actors through the world: collision, steps, slopes, swimming, gravity
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"
#include "mwbase/session.hpp"
#include "mwmechanics/combathelpers.hpp"

static const float kNpcSwimDepth = 95.0f;          // as the player's: feet below the surface

// Moves an NPC `step` units along (dirX, dirY), following the floor and stopped by walls;
// false when blocked (no floor there)
// The steepest land an actor walks up: up to 60 degrees (OpenMW's actors climb what its navigation mesh allows), except in
// a fight, where it climbs no steeper than the player does (kWalkSlope, 46): backing up a slope too steep to walk was a
// way out of a fight that cost the player nothing, or one a foe took out of the player's reach. One following or
// escorting the player for a quest keeps up wherever they go, fight or not. (Out of a fight the 46 kept town guards from
// paths they walk.) Our own rule, not OpenMW's
float npcClimbSlope(const World& w, const Ref& r)
{
	bool quest = (r.aiPackage == AIPKG_FOLLOW || r.aiPackage == AIPKG_ESCORT) && r.aiTarget == "player" && !r.aiDone && !r.ally;
	bool fighting = r.ai == AI_COMBAT || r.duelFoe >= 0;
	return fighting && !quest ? kWalkSlope : 60.0f;
}

// The room a walking actor needs over the floor it steps up onto: about half its height (people are some 128 tall;
// stairs under a sloping ceiling leave less than all of it straight over the step)
static const float kActorHeadroom = 64.0f;

// The room over a floor at (x, y, z) up to kActorHeadroom: how far up the first thing overhead is
static float actorRoomOver(Cell* const* cells, int count, float x, float y, float z)
{
	float from[3] = { x, y, z + 2.0f }, to[3] = { x, y, z + kActorHeadroom };
	float room = kActorHeadroom;
	for (int k = 0; k < count; k++)
	{
		float t;
		if (collisionRaycast(cells[k]->collision, from, to, &t))
			room = fminf(room, 2.0f + t * (kActorHeadroom - 2.0f));
	}
	return room;
}

bool Session::npcStep(int ri, Cell& cell, float dirX, float dirY, float step)
{
	Ref& r = w.refs[ri];
	float body[3] = { r.pos[0] + dirX * step, r.pos[1] + dirY * step, r.pos[2] + 70.0f };
	// Outdoors the ground and walls past the cell's edge are the next cell's: every loaded cell whose collision
	// reaches the spot counts, as for the player and gravity (OpenMW moves actors in one physics world, whatever
	// cell they belong to). A follower stopped dead at the border before, with no floor ahead
	Cell* around[12] = { &cell };
	int aroundCount = 1;
	for (LoadedCell* l : w.loaded)
		if (&l->cell != &cell && aroundCount < 12 && collisionReaches(l->cell.collision, body[0], body[1], 40.0f))
			around[aroundCount++] = &l->cell;
	for (int k = 0; k < aroundCount; k++)
		collisionPushSphere(around[k]->collision, body, 22.0f, true);   // about a person's half width (Morrowind's boxes: 20..29)
	// The player's body is in the way, as a wall is (OpenMW's actors collide with it; they don't shove it): no step
	// closer than the two bodies' widths (the player's 22, the cylinder they push the player out of, 28). Stepping
	// aside on the ledge in Abernanit, Alonisea Varys shoved the player off it
	{
		const float* pf = w.player.feet;
		float nx = body[0] - pf[0], ny = body[1] - pf[1], ox = r.pos[0] - pf[0], oy = r.pos[1] - pf[1];
		float nd = nx * nx + ny * ny;
		if (nd < 50.0f * 50.0f && nd < ox * ox + oy * oy && pf[2] < r.pos[2] + 128.0f && pf[2] + 128.0f > r.pos[2]
			&& w.distanceToPlayer(ri) < 1000.0f)
			return false;
	}
	float fz = -1e9f;
	// people swim in deep water, the head out (creatures keep to their depth)
	float swimZ = cell.hasWater() && r.type == "NPC_" ? cell.waterZ - kNpcSwimDepth : -1e9f;
	// the floor under the feet, not under one point: the highest of the middle and four spots around it
	// (as the player's), so a crack or a hatch's corner under the middle isn't a drop
	bool found = false;
	for (int k = 0; k < aroundCount; k++)
	{
		float z;
		if (collisionFootFloor(around[k]->collision, body[0], body[1], kActorFootReach, r.pos[2] + 40.0f, r.pos[2] - 300.0f, &z)
			&& z > fz)
		{
			fz = z;
			found = true;
		}
	}
	// A creature that flies, or swims in water, keeps to the air / the water: only kept over the floor (and a swimmer under
	// the surface), no drop refused (OpenMW drops nothing that moves by Z; it goes on its pitch toward its point,
	// npcMoveTo). A cliff racer flying over a hill never took a step, a slaughterfish kept to the sea floor
	float top = zMoverTop(w, r, cell);
	if (top > -1e8f)
	{
		float mx = body[0] - r.pos[0], my = body[1] - r.pos[1];
		r.pos[0] = body[0];
		r.pos[1] = body[1];
		r.pos[2] = fminf(found ? fmaxf(r.pos[2], fz) : r.pos[2], fmaxf(top, found ? fz : -1e9f));
		r.fitBox();
		r.moved = true;
		return mx * mx + my * my > step * step * 0.1f;
	}
	if (!found)
	{
		if (swimZ < -1e8f || r.pos[2] > swimZ + 1.0f)
			return false;
		fz = swimZ;
	}
	fz = fmaxf(fz, swimZ);
	// A step goes down stairs and slopes, not over an edge: dropping more than a stair's height in one
	// step (the prison ship's guard walked into the stairwell opening and ended up inside the stairs)
	// (a follower held up outdoors, going where the player went, steps down a ledge as the player did: OpenMW's
	// actors fall off one where they walk)
	float maxDrop = (r.trailUntil > w.time || r.dropUntil > w.time) && !w.cells[w.placeOf(ri)].interior ? 250.0f : 48.0f;
	if (fz < r.pos[2] - maxDrop && fz > swimZ + 0.5f)
	{
		return false;
	}
	// A drop of more than a stair that lands well under the heights the player walked at that spot is a dead-end ledge below
	// the way (OpenMW's navigation mesh would not route a follower into it), not a short cut: they keep to the way instead
	if (fz < r.pos[2] - 48.0f && fz > swimZ + 0.5f && r.trailUntil > w.time)
	{
		float th = trailHeightNear(body[0], body[1]);
		if (th > -1e8f && fz < th - 45.0f)
			return false;
	}
	// Up land steeper than it may climb (npcClimbSlope): no step. (The floor test at that limit finds no floor there, or one
	// well below the one the 60 degree test stepped up to; a few units' difference is a foot's spot on a small steep patch
	// of an even slope, not a slope too steep. Going down is never refused, nor anything in the water)
	// Objects' faces count as floor up to 60 degrees for the stair ramps a path grid route takes. Off one (walking
	// straight at its point, or stepping aside when stuck) an actor climbs objects no steeper than the player's 46
	// (OpenMW's sMaxSlope; its navigation mesh routes round the rest): the prison ship's guard walked straight at the
	// hold's side, faces of 37 to 58 degrees, and up it. A follower of the player (it walks their footsteps) and a
	// fighter keep 60, on the stairs the player takes
	float climb = npcClimbSlope(w, r);
	bool onRoute = !r.path.empty() && r.stuckTimer >= 0.0f;
	bool fighting = r.ai == AI_COMBAT || r.duelFoe >= 0;
	bool follower = r.aiPackage == AIPKG_FOLLOW && r.aiTarget == "player" && !r.aiDone;
	float objectClimb = onRoute || fighting || follower ? 90.0f : kWalkSlope;
	bool wet = cell.hasWater() && r.pos[2] < cell.waterZ + 10.0f;
	if ((climb < 60.0f || objectClimb < 90.0f) && fz > r.pos[2] + 2.0f && fz > swimZ + 0.5f && !wet)
	{
		float low = -1e9f;
		for (int k = 0; k < aroundCount; k++)
		{
			float z;
			if (collisionFootFloor(around[k]->collision, body[0], body[1], kActorFootReach, r.pos[2] + 40.0f, r.pos[2] - 300.0f, &z, climb,
				objectClimb) && z > low)
				low = z;
		}
		if (low < fz - 8.0f)
			return false;
	}
	// No step up into less room than the body needs under what is overhead (the sphere above only keeps it out of walls),
	// unless it stands in less where it is (put somewhere cramped, it can still get out): a guard at the top of the hold's
	// side stepped onto the hold's ceiling, inside the deck. (A follower of the player and a fighter go where the player
	// went: Larienna Macrina was held on the stairs up out of Nchurdamz's lower hall)
	if (fz > r.pos[2] + 2.0f && !follower)
	{
		float room = fighting ? kActorHeadroom : actorRoomOver(around, aroundCount, body[0], body[1], fz);
		if (room < kActorHeadroom && room < actorRoomOver(around, aroundCount, r.pos[0], r.pos[1], r.pos[2]))
			return false;
		// Nor up onto a spot held by a foot's edge alone with nothing at all under the middle (a crack has a floor below it):
		// Nistacey went up the rock's edge over the gap beside Zainsipilu's floor, and every step from there, pushed back off the
		// rock, came down over the gap
		bool under = false;
		for (int k = 0; k < aroundCount && !under; k++)
		{
			float z;
			under = collisionFloor(around[k]->collision, body[0], body[1], fz + 2.0f, r.pos[2] - 300.0f, &z);
		}
		if (!under)
			return false;
	}
	if (Actor* a = w.actorOf(ri))
		a->swimming = fz <= swimZ + 0.5f;
	float moved[3] = { body[0] - r.pos[0], body[1] - r.pos[1], fz - r.pos[2] };
	r.pos[0] = body[0];
	r.pos[1] = body[1];
	r.pos[2] = fz;
	r.fitBox();
	r.moved = true;
	return moved[0] * moved[0] + moved[1] * moved[1] > step * step * 0.1f;
}

// ---- Gravity for people and creatures

static const float kActorGravity = 627.0f;      // as the player's (mwworld/player.cpp)

// Outdoors: a neighbouring exterior cell (of the 8 round the actor's) that exists but isn't loaded, with the actor
// within 2048 units of the edge it shares. Nothing is known about the ground there, so nobody starts to fall
static bool actorFloorMaybeUnloaded(World& w, const Ref& r)
{
	if (w.current < 0 || w.cells[w.current].interior)
		return false;
	const float kReach = 2048.0f;
	int gx = (int)floorf(r.pos[0] / 8192.0f), gy = (int)floorf(r.pos[1] / 8192.0f);
	float fx = r.pos[0] - gx * 8192.0f, fy = r.pos[1] - gy * 8192.0f;      // 0..8192 inside their cell
	for (int dx = -1; dx <= 1; dx++)
		for (int dy = -1; dy <= 1; dy++)
		{
			if (!dx && !dy)
				continue;
			if ((dx < 0 && fx > kReach) || (dx > 0 && fx < 8192.0f - kReach) || (dy < 0 && fy > kReach)
				|| (dy > 0 && fy < 8192.0f - kReach))
				continue;
			int g = w.gridCell(gx + dx, gy + dy);
			if (g >= 0 && !w.cells[g].live)
				return true;
		}
	return false;
}

void Session::actorGravity(float dt)
{
	for (int i : w.loadedActors)
	{
		Ref& r = w.refs[i];
		if ((r.type != "NPC_" && r.type != "CREA") || !r.visible() || r.actor < 0)
			continue;
		// Creatures fall like anyone, but not the ones that fly, nor the ones that swim while they're in water
		// (as in OpenMW, where every actor falls unless it flies or is held up by water)
		bool creature = r.type == "CREA";
		u8 afloat = w.game.actors[r.actor].afloat;
		// A corpse in water floats up to where it would swim and stays there (OpenMW's CharacterController: the dead
		// rise in water): a slaughterfish killed at the bottom comes up to the surface
		if (r.dead)
		{
			float level = -1e9f;
			for (LoadedCell* l : w.loaded)
				if (l->cell.hasWater() && r.pos[2] < l->cell.waterZ)
					level = fmaxf(level, l->cell.waterZ - (creature ? 50.0f : kNpcSwimDepth));
			if (level > -1e8f)
			{
				if (r.pos[2] < level)
				{
					r.pos[2] = fminf(level, r.pos[2] + 100.0f * dt);
					r.fitBox();
					r.moved = true;
					w.syncActor(i);
				}
				r.falling = false;
				continue;
			}
		}
		if (creature && !r.dead && (afloat & 0x20))
			continue;
		if (creature && !r.dead && (afloat & 0x10) && w.underWater(r.pos[2]))
			continue;
		// The floor under them: every quarter second while standing, every frame while falling. Under the
		// feet, as a step measures it (npcStep): the middle alone dropped a walker on a slope by up to 15 units
		// every quarter second, and the next step lifted them back
		if (!r.falling)
		{
			r.floorCheck -= dt;
			if (r.floorCheck > 0.0f)
				continue;
			r.floorCheck = 0.25f + (i % 8) * 0.01f;
		}
		float best = -1e9f;
		for (LoadedCell* l : w.loaded)
		{
			float z;
			if (collisionFootFloor(l->cell.collision, r.pos[0], r.pos[1], kActorFootReach, r.pos[2] + 40.0f, r.pos[2] - 6000.0f,
					&z) && z > best)
				best = z;
		}
		// deep water holds people up (swimming); creatures that walk wade through it
		float swimZ = -1e9f;
		for (LoadedCell* l : w.loaded)
			if (l->cell.hasWater() && !creature)
				swimZ = fmaxf(swimZ, l->cell.waterZ - kNpcSwimDepth);
		bool swim = swimZ > -1e8f && swimZ > best;
		if (swim)
			best = swimZ;
		if (Actor* a = w.actorOf(i))
			a->swimming = swim && !r.dead && r.pos[2] <= swimZ + 40.0f;
		if (best < -1e8f)
		{
			r.falling = false;          // nothing at all below: the land isn't there, leave them be
			continue;
		}
		if (!r.falling)
		{
			if (best > r.pos[2] - 40.0f)
			{
				// standing; placed a little above the floor (Ganciele Douar), or a step down: onto it
				float dz = best - r.pos[2];
				// (a creature's model drawn where its collision box comes to rest: World::actorBoxLift)
				float lift = w.actorBoxLift(i);
				if (fabsf(lift - r.boxLift) > 0.5f)
				{
					r.boxLift = lift;
					r.moved = true;
					w.syncActor(i);
				}
				if (dz < -0.5f && !r.dead)
				{
					r.pos[2] = best;
					r.fitBox();
					r.moved = true;
					w.syncActor(i);
				}
				continue;
			}
			// The floor they stand on may be in a cell not loaded (yet, or any more) next to theirs: the Gnisis
			// strider platform's collision comes from the cell north of the caravaner's. OpenMW keeps an actor
			// still while the cell under it isn't loaded; so here when the drop is over 400 and one of the 8
			// neighbours within reach of the actor is missing
			if (r.pos[2] - best > 400.0f && actorFloorMaybeUnloaded(w, r))
				continue;
			r.falling = true;
			r.fallVz = 0.0f;
			r.fallTop = r.pos[2];
			if (r.pos[2] - best > 200.0f)
				logf("world: %s falls from %.0f (floor %.0f)", r.id.c_str(), r.pos[2], best);
		}
		r.fallVz = fmaxf(r.fallVz - kActorGravity * dt, -3000.0f);
		float z = r.pos[2] + r.fallVz * dt;
		bool landed = z <= best;
		if (landed)
			z = best;
		r.pos[2] = z;
		r.fitBox();
		r.moved = true;
		w.syncActor(i);
		if (!landed)
			continue;
		r.falling = false;
		// The player's fall damage, with their own Acrobatics
		float h = r.fallTop - z;
		if (r.dead || swim)
			continue;                   // water breaks a fall
		int acro = w.game.actors[r.actor].skills[20];
		float x = fallDamage(h, acro, 0.0f);
		if (x > 0.0f)
		{
			float ft = fatigueTermOf(r.fatigue, r.fatigueMax);
			logf("world: %s lands after %.0f units, %.1f damage", r.id.c_str(), h, x * (1.0f - 0.25f * ft));
			r.health -= x * (1.0f - 0.25f * ft);
			if (r.health <= 0.0f)
				killNpc(i, false);
			else if (x > acro * ft)
				knockDown(i, false);
		}
	}
}

// Morrowind-scale body: ~128 units tall, steps up to 34 units (OpenMW's step size)
static const float kRadius = 22.0f;
static const float kHeight = 128.0f;
static const float kStepUp = 34.0f;
static const float kGravity = 627.0f;
static const float kFlySpeed = 300.0f;
static const float kLookSpeed = 1.8f;
static const float kSwimDepth = 95.0f;      // feet below the surface while swimming: the head stays out
static const float kSneakDrop = 28.0f;      // eye lowered while sneaking
static const float kKnockDrop = 90.0f;      // ... and while knocked down

// Body spheres (height of center above the feet); the lowest one sits above step height
// so low ledges are climbed instead of blocking.
static const float kBodySpheres[] = { kStepUp + kRadius, 84.0f, kHeight - kRadius };

static bool sampleFloor(Scene& scene, const float feet[3], float zTop, float zBottom, float* zOut, float maxSlope = kWalkSlope)
{
	static const float offsets[5][2] = { {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
	bool found = false;
	for (Cell* cell : scene.cells)
		for (auto& o : offsets)
		{
			float z;
			if (collisionFloor(cell->collision, feet[0] + o[0] * kRadius * 0.7f, feet[1] + o[1] * kRadius * 0.7f, zTop, zBottom, &z, maxSlope)
				&& (!found || z > *zOut))
			{
				*zOut = z;
				found = true;
			}
		}
	return found;
}

static bool pushSphere(Scene& scene, float c[3], float r, bool horizontalOnly)
{
	bool touched = false;
	for (Cell* cell : scene.cells)
		touched |= collisionPushSphere(cell->collision, c, r, horizontalOnly);
	return touched;
}

// The land's surface over the feet, whatever its slope (the floor test looks from the feet up and never finds the land
// over a body that got under it)
static bool sampleLand(Scene& scene, const float feet[3], float zTop, float zBottom, float* zOut)
{
	bool found = false;
	for (Cell* cell : scene.cells)
	{
		float z;
		if (collisionLandFloor(cell->collision, feet[0], feet[1], zTop, zBottom, &z) && (!found || z > *zOut))
		{
			*zOut = z;
			found = true;
		}
	}
	return found;
}

const char* g_playerBlock = "";    // debug: what last pushed the player back

// Whether the body has headroom here: nothing crosses its axis between step height and the top of
// the head (a floor above poking through the head would put the eye outside the room)
static bool bodyFits(Scene& scene, const float feet[3])
{
	float a[3] = { feet[0], feet[1], feet[2] + kStepUp }, b[3] = { feet[0], feet[1], feet[2] + kHeight };
	float t;
	for (Cell* cell : scene.cells)
		if (collisionRaycast(cell->collision, a, b, &t))
			return false;
	return true;
}

static void resolveBody(Player& p, Scene& scene)
{
	for (int iter = 0; iter < 2; iter++)
	{
		for (float h : kBodySpheres)
		{
			float c[3] = { p.feet[0], p.feet[1], p.feet[2] + h };
			if (pushSphere(scene, c, kRadius, true))
				g_playerBlock = "mesh";
			// Closed swinging doors block as boxes
			for (Cell* cell : scene.cells)
			for (auto& b : cell->blockers)
			{
				if (c[2] < b.min[2] || c[2] > b.max[2])
					continue;
				float qx = fmaxf(b.min[0], fminf(c[0], b.max[0])), qy = fmaxf(b.min[1], fminf(c[1], b.max[1]));
				float dx = c[0] - qx, dy = c[1] - qy, d2 = dx * dx + dy * dy;
				if (d2 >= kRadius * kRadius)
					continue;
				if (d2 < 1e-4f)
				{
					// Center inside the box: leave through the nearest side
					float exits[4] = { c[0] - b.min[0], b.max[0] - c[0], c[1] - b.min[1], b.max[1] - c[1] };
					int k = 0;
					for (int j = 1; j < 4; j++)
						if (exits[j] < exits[k]) k = j;
					if (k == 0) c[0] = b.min[0] - kRadius;
					if (k == 1) c[0] = b.max[0] + kRadius;
					if (k == 2) c[1] = b.min[1] - kRadius;
					if (k == 3) c[1] = b.max[1] + kRadius;
					continue;
				}
				float d = sqrtf(d2), push = (kRadius - d) / d;
				g_playerBlock = "door";
				c[0] += dx * push;
				c[1] += dy * push;
			}
			p.feet[0] = c[0];
			p.feet[1] = c[1];
		}
		// Actors are upright cylinders
		for (Cell* cell : scene.cells)
		for (auto& a : cell->actors)
		{
			if (a.radius <= 0.0f)
				continue;
			if (p.feet[2] > a.pos[2] + a.height || p.feet[2] + kHeight < a.pos[2])
				continue;
			float dx = p.feet[0] - a.pos[0], dy = p.feet[1] - a.pos[1];
			float minDist = kRadius + a.radius, d2 = dx * dx + dy * dy;
			if (d2 >= minDist * minDist || d2 < 1e-4f)
				continue;
			float d = sqrtf(d2), push = (minDist - d) / d;
			g_playerBlock = a.id;
			p.feet[0] += dx * push;
			p.feet[1] += dy * push;
		}
	}
}

void playerSpawn(Player& p, Scene& scene, const float eye[3], float yaw, float pitch)
{
	p.feet[0] = eye[0];
	p.feet[1] = eye[1];
	p.feet[2] = eye[2] - PLAYER_EYE_HEIGHT;
	p.yaw = yaw;
	p.pitch = pitch;
	p.vz = 0.0f;
	// The floor under the feet (a door's destination is where they go), looked for from a step above
	// them: from the eye, a ceiling or the floor above within head height passed for it and the player
	// arrived on top of the room. From the eye only when nothing is below.
	float z, feetZ = p.feet[2];
	p.onGround = sampleFloor(scene, p.feet, feetZ + kStepUp, feetZ - 1000.0f, &z)
		|| sampleFloor(scene, p.feet, eye[2], eye[2] - 1000.0f, &z);
	// What the step above found is over the destination: a low ceiling (a round tunnel's top, Tel
	// Uvirith) rather than a floor, when there is a floor below the destination
	float below;
	if (p.onGround && z > feetZ + 8.0f && sampleFloor(scene, p.feet, feetZ + 1.0f, feetZ - 400.0f, &below))
		z = below;
	// Nor is a surface near head height over it (a tilted ship's ceiling, Remote Shipwreck): the
	// player drops from the destination instead
	if (p.onGround && z > feetZ + 90.0f)
		p.onGround = false;
	if (p.onGround)
		p.feet[2] = z;
	p.fits = bodyFits(scene, p.feet);
	p.fallTop = p.feet[2];                // a teleport isn't a fall
	p.landedFall = 0.0f;
	p.heldUp = false;
}

// The body hanging on a face too steep to be a floor: the low sphere pushed out of it in 3D, and the sideways part of
// that taken, so the next fall step clears the face. At least minMove a frame: a slide down such a face gathers speed
// (OpenMW's solver turns the fall along the face), so it is passed the sideways share of the fall speed
static void slideOffSteepFace(Player& p, Scene& scene, float minMove = 2.0f)
{
	for (float lift : { 0.0f, kStepUp * 0.5f })
	{
		float c[3] = { p.feet[0], p.feet[1], p.feet[2] + kRadius + lift }, c0[3] = { c[0], c[1], c[2] };
		pushSphere(scene, c, kRadius + 6.0f, false);
		float sx = c[0] - c0[0], sy = c[1] - c0[1], len = sqrtf(sx * sx + sy * sy);
		if (len > 0.05f)
		{
			float move = fmaxf(len, fminf(minMove, kRadius)) / len;
			p.feet[0] += sx * move;
			p.feet[1] += sy * move;
			return;
		}
	}
}

void playerUpdate(Player& p, Scene& scene, const PlayerInput& inRaw, float dt)
{
	PlayerInput in = inRaw;
	if (p.knockTimer > 0.0f)
	{
		p.knockTimer -= dt;
		in.moveX = in.moveY = 0.0f;
		in.jump = false;
	}
	p.sneaking = in.sneak && !p.flying && !p.swimming && p.levitate <= 0.0f;
	float drop = p.knockTimer > 0.0f ? kKnockDrop : p.sneaking ? kSneakDrop : 0.0f;
	p.eyeDrop += (drop - p.eyeDrop) * fminf(1.0f, dt * 8.0f);
	p.yaw += in.lookX * kLookSpeed * dt;
	p.pitch += in.lookY * kLookSpeed * dt;
	// Gyro aim: the console's own turn, as far as it turned
	const float kDegrees = 3.14159265f / 180.0f;
	p.yaw += in.gyroYaw * kDegrees * dt;
	p.pitch += in.gyroPitch * kDegrees * dt;
	p.pitch = fmaxf(-1.5f, fminf(1.5f, p.pitch));

	float sy = sinf(p.yaw), cy = cosf(p.yaw);
	float dirX = sy * in.moveY + cy * in.moveX;
	float dirY = cy * in.moveY - sy * in.moveX;

	if (p.flying)
	{
		float speed = kFlySpeed * (in.fast ? 3.0f : 1.0f);
		p.feet[0] += dirX * speed * dt;
		p.feet[1] += dirY * speed * dt;
		if (in.up) p.feet[2] += speed * dt;
		if (in.down) p.feet[2] -= speed * dt;
		p.vz = 0.0f;
		return;
	}

	float start[3] = { p.feet[0], p.feet[1], p.feet[2] };
	bool startOnGround = p.onGround;

	// Horizontal: move in sub-steps no longer than half the radius, resolving each
	// (levitating wins over swimming: OpenMW's flying actors fly out of the water too)
	bool levitating = p.levitate > 0.0f;
	float levSpeed = p.flySpeed;
	float speed = levitating ? levSpeed * fmaxf(0.3f, cosf(p.pitch))
		: p.runSpeed * (p.swimming ? p.swimFactor * (1.0f + p.swimBoost / 100.0f) * fmaxf(0.3f, cosf(p.pitch))
			: p.sneaking ? p.sneakFactor : 1.0f);
	speed *= p.loadSpeed;
	float dx = dirX * speed * dt, dy = dirY * speed * dt;
	// In the air after a jump (OpenMW's CharacterController and MovementSolver): the pad steers only airControl of
	// the run speed, on top of the take-off's own speed along the ground, which lasts until landing
	if (!p.onGround && !levitating && !p.swimming)
	{
		dx = (dirX * speed * p.airControl + p.inertia[0]) * dt;
		dy = (dirY * speed * p.airControl + p.inertia[1]) * dt;
	}
	int steps = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)) / (kRadius * 0.5f));
	if (steps < 1)
		steps = 1;
	// On the ground, in a long frame (kLongFrame and up: the frame after a stall on the console is capped at 0.1 s, and
	// at a run moves 30 to 55 units), the feet follow the floor up at each part of the move (within a step's height,
	// as OpenMW's solver steps up at each thing it meets). The whole move used to be taken first and the floor looked
	// for after it, from a step above the feet: a long frame's move up a hill (40 up of 50) put the floor out of that
	// reach and the body went on down inside the hill; a little shorter (over 24 up), the low body sphere met the slope
	// and was pushed back off it, a jolt at every long frame. A usual frame keeps the one step at the end of the move,
	// below: it is what the chained quest runs (a fixed 1/30 s, 18 units a frame) are tuned on, and following in them
	// too moved the body by units where it brushed a rock or a wall, enough to send a run another way. Steeper
	// land (past kWalkSlope) is no floor, as ever: the feet don't follow it, the low body sphere pushes the body back
	// off it, and the end of the move drops a body that got onto it, which slides off. Outdoors only: an interior
	// loads whole as it is entered (no long frame on a stair)
	const float kLongFrame = 0.06f;
	bool outdoors = scene.here && !(scene.here->flags & CELL_INTERIOR);
	bool following = p.onGround && !levitating && !p.swimming && outdoors && dt >= kLongFrame;
	for (int s = 0; s < steps; s++)
	{
		p.feet[0] += dx / steps;
		p.feet[1] += dy / steps;
		resolveBody(p, scene);
		float floorZ;
		if (following && sampleFloor(scene, p.feet, p.feet[2] + kStepUp, p.feet[2] - 1.0f, &floorZ) && floorZ > p.feet[2] + 0.01f)
		{
			// (a step up without headroom over it is left to the end of the move, which blocks it like a wall, below)
			float up[3] = { p.feet[0], p.feet[1], floorZ };
			if (!p.fits || bodyFits(scene, up))
				p.feet[2] = floorZ;
		}
	}

	// Levitating: up and down where the view points (B / R up, L down), floors and ceilings stop it
	if (levitating)
	{
		float vz = in.moveY * sinf(p.pitch) * levSpeed + ((in.jump || in.up) ? levSpeed : 0.0f)
			- (in.down ? levSpeed : 0.0f);
		float newZ = p.feet[2] + vz * dt, floorZ;
		// (a flier meeting a slope steeper than the walking limit rises along it, as OpenMW's solver slides the
		// velocity along what it hits; our sideways push alone held it against the ridge: 60 degrees, as before)
		if (sampleFloor(scene, p.feet, p.feet[2] + kStepUp, newZ - 1.0f, &floorZ, 60.0f) && floorZ >= newZ)
			newZ = floorZ;
		float up[3] = { p.feet[0], p.feet[1], newZ };
		if (newZ <= p.feet[2] || bodyFits(scene, up))
			p.feet[2] = newZ;
		p.vz = 0.0f;
		p.onGround = false;
		p.fallTop = p.feet[2];
		p.landedFall = 0.0f;
		p.inertia[0] = p.inertia[1] = 0.0f;
		p.jumpFlight = false;
		p.swimming = false;
	}
	// In the water: swim where the view points (down when looking down, B up), no gravity; the surface
	// (the head just out) and the bottom hold the body; a floor above swimming depth is a shore
	else if (p.swimming && !p.waterWalk && scene.here && scene.here->hasWater())
	{
		float surface = scene.here->waterZ - kSwimDepth;
		float vz = in.moveY * sinf(p.pitch) * p.runSpeed * p.swimFactor;
		if (in.jump)
			vz = p.runSpeed * p.swimFactor;
		float newZ = fminf(surface, p.feet[2] + vz * dt);
		float floorZ;
		bool bottom = sampleFloor(scene, p.feet, p.feet[2] + kStepUp, newZ - 1.0f, &floorZ, 60.0f) && floorZ >= newZ;   // (swimming: as before)
		p.feet[2] = bottom ? floorZ : newZ;
		p.vz = 0.0f;
		p.onGround = true;
		p.fallTop = p.feet[2];
		p.landedFall = 0.0f;
		p.swimming = !(bottom && floorZ > surface + 1.0f);
		p.inertia[0] = p.inertia[1] = 0.0f;
		p.jumpFlight = false;
	}
	else
	{
		// Vertical: gravity, landing, stepping up ledges and down stairs
		if (in.jump && p.onGround)
		{
			// standing: straight up at the jump speed; moving: along the move at 45 degrees, 0.707 of it each way
			// (OpenMW's CharacterController::updateState)
			float len = sqrtf(dirX * dirX + dirY * dirY);
			p.vz = len > 0.01f ? p.jumpSpeed * 0.707f : p.jumpSpeed;
			p.inertia[0] = len > 0.01f ? dirX / len * p.jumpSpeed * 0.707f : 0.0f;
			p.inertia[1] = len > 0.01f ? dirY / len * p.jumpSpeed * 0.707f : 0.0f;
			p.onGround = false;
			p.jumpedNow = true;
			p.jumpFlight = true;
		}
		p.vz = p.slowFall ? fmaxf(p.vz - kGravity * 0.25f * dt, -200.0f) : fmaxf(p.vz - kGravity * dt, -3000.0f);
		float newZ = p.feet[2] + p.vz * dt;
		// (on the ground, the feet follow it down a step's height a frame, or as much as a walkable slope drops over this
		// frame's move if that is more: a fast runner, 50 units a frame, went airborne off every dip and over the edge)
		float moved = hypotf(p.feet[0] - start[0], p.feet[1] - start[1]);
		float snapBelow = (p.onGround && p.vz <= 0.0f) ? fmaxf(kStepUp, moved * 1.04f) : 0.0f;
		float floorZ;
		bool floor = sampleFloor(scene, p.feet, p.feet[2] + kStepUp, newZ - snapBelow - 1.0f, &floorZ);
		if (floor && floorZ > p.feet[2] + 1.0f && p.fits)
		{
			// A step up (or a ledge to land on) without headroom above it, like a crate under the floor
			// above or the top of a wall by a stairwell, blocks like a wall
			float up[3] = { p.feet[0], p.feet[1], floorZ };
			if (!bodyFits(scene, up))
			{
				p.feet[0] = start[0];
				p.feet[1] = start[1];
				floor = sampleFloor(scene, p.feet, p.feet[2] + kStepUp, newZ - snapBelow - 1.0f, &floorZ);
			}
		}
		bool wasOnGround = p.onGround;
		if (floor && floorZ >= newZ - snapBelow && p.vz <= 0.0f)
		{
			if (!wasOnGround && !p.flying)
				p.landedFall = fmaxf(0.0f, p.fallTop - floorZ);
			p.feet[2] = floorZ;
			p.vz = 0.0f;
			p.onGround = true;
			p.heldUp = false;
		}
		else
		{
			float landZ;
			if (wasOnGround && outdoors && !scene.groundThere)
			{
				// The cell under the feet isn't loaded: it is still streaming in (World::streamExterior). The ground
				// isn't gone, it isn't there yet: the feet stay where they are
				p.vz = 0.0f;
				p.onGround = true;
				p.heldUp = true;
			}
			else if (wasOnGround && outdoors
				&& sampleLand(scene, p.feet, p.feet[2] + (p.heldUp ? 4096.0f : kHeight), p.feet[2] + kStepUp, &landZ))
			{
				// Standing, with no floor under the feet but the land more than a step over them: the body is under the
				// land (there is nothing to stand on below it), and is put back on it rather than fall for ever. The land
				// of a cell that came in under feet held up for it may be far over them (a hill), and that is no news;
				// otherwise it says so once: the follow above should never let it happen. (Less than a step over them is
				// the feet a little into a face too steep to walk on: the low body sphere pushes the body back out, as
				// ever.) Land below held feet is left to gravity, as ever: what the feet were on may be a platform or a
				// bridge whose collision is in a cell still to come, and the land far under it. Done here: the never-
				// through-a-surface check below would undo the move up through the hill
				if (!p.heldUp)
					monitorOnce("underland", "player under the land at %.0f %.0f %.0f (the land %.0f above the feet): put back on it",
						p.feet[0], p.feet[1], p.feet[2], landZ - p.feet[2]);
				p.feet[2] = landZ;
				p.vz = 0.0f;
				p.onGround = true;
				p.heldUp = false;
				p.fallTop = landZ;
				p.landedFall = 0.0f;
				p.inertia[0] = p.inertia[1] = 0.0f;
				p.jumpFlight = false;
				p.swimming = false;
				p.fits = bodyFits(scene, p.feet);
				return;
			}
			else
			{
				p.feet[2] = newZ;
				p.onGround = false;
				p.heldUp = false;
			}
		}
		// Landed (or in the water below): the take-off's speed is spent
		if (p.onGround)
		{
			p.inertia[0] = p.inertia[1] = 0.0f;
			p.jumpFlight = false;
		}
		// How high a fall starts (Slow Fall and flying don't count)
		if (p.onGround || p.slowFall || p.flying)
			p.fallTop = p.feet[2];
		else
			p.fallTop = fmaxf(p.fallTop, p.feet[2]);

		// Deep water holds the player at the surface
		p.swimming = false;
		if (p.waterWalk && scene.here && scene.here->hasWater() && p.feet[2] < scene.here->waterZ && p.vz <= 0.0f
			&& p.feet[2] > scene.here->waterZ - kSwimDepth)
		{
			p.feet[2] = scene.here->waterZ;
			p.vz = 0.0f;
			p.onGround = true;
		}
		else if (scene.here && scene.here->hasWater() && p.feet[2] < scene.here->waterZ - kSwimDepth)
		{
			p.feet[2] = scene.here->waterZ - kSwimDepth;
			p.vz = fmaxf(p.vz, 0.0f);
			p.onGround = true;
			p.swimming = true;
			p.landedFall = 0.0f;       // water breaks a fall
			p.fallTop = p.feet[2];
		}
	}

	// Ceiling: a jump stops under it (standing, the floor holds the feet)
	float head[3] = { p.feet[0], p.feet[1], p.feet[2] + kHeight - kRadius };
	float before = head[2];
	pushSphere(scene, head, kRadius, false);
	if (head[2] < before - 0.01f && !p.onGround)
	{
		p.feet[2] -= fminf(before - head[2], kRadius);
		p.vz = fminf(p.vz, 0.0f);
	}

	// Never through a surface: the chest and the head go straight from where they were (a push out of
	// something the legs slipped into can otherwise throw the body through the wall behind it)
	auto crosses = [&](const float to[3]) {
		for (float h : { kStepUp + kRadius, kHeight - kRadius })
		{
			float a[3] = { start[0], start[1], start[2] + h }, b[3] = { to[0], to[1], to[2] + h }, t;
			for (Cell* cell : scene.cells)
				if (collisionRaycast(cell->collision, a, b, &t))
					return true;
		}
		return false;
	};
	float endZ = p.feet[2];
	if (!startOnGround && !levitating && !p.swimming && endZ < start[2] - 0.01f && crosses(p.feet))
	{
		// Falling with the pad (or a jump's speed) pushing the chest into a wall face: the push is refused, the fall
		// goes on straight down (OpenMW's solver clips the velocity at what it hits and gravity goes on). Undoing the
		// whole step held the body in the air against the face for good (Gnisis, by the Arvs-Drelen door). Straight
		// down only what is under the feet stops it (any face not upright): a ledge the body already straddles
		// (higher than a step over the feet) does not. A face too steep to stand on holds the feet, and the body slides
		// off it
		float down[3] = { start[0], start[1], endZ }, floorZ;
		// (from a step above the feet: a body sunk a little into a slope finds that slope, not the land below it, and does
		// not drop through it)
		bool hit = sampleFloor(scene, down, start[2] + kStepUp, endZ - 1.0f, &floorZ, 89.0f) && floorZ >= endZ;
		if (hit)
			down[2] = floorZ;
		float walkZ;
		bool landed = hit && sampleFloor(scene, down, floorZ + 1.0f, floorZ - 1.0f, &walkZ);
		p.feet[0] = down[0];
		p.feet[1] = down[1];
		p.feet[2] = down[2];
		p.inertia[0] = p.inertia[1] = 0.0f;
		if (landed)
		{
			p.landedFall = fmaxf(0.0f, p.fallTop - down[2]);
			p.vz = 0.0f;
			p.onGround = true;
			p.jumpFlight = false;
		}
		else
		{
			p.onGround = false;
			// (the fall speed kept: a slide down a face too steep to stand on gathers speed, as in OpenMW)
			if (hit)
				slideOffSteepFace(p, scene, fmaxf(2.0f, fabsf(p.vz) * dt * 0.7f));
		}
		p.fits = bodyFits(scene, p.feet);
		return;
	}
	for (float h : { kStepUp + kRadius, kHeight - kRadius })
	{
		float a[3] = { start[0], start[1], start[2] + h }, b[3] = { p.feet[0], p.feet[1], p.feet[2] + h }, t;
		bool crossed = false;
		for (Cell* cell : scene.cells)
			crossed |= collisionRaycast(cell->collision, a, b, &t);
		if (crossed)
		{
			p.feet[0] = start[0];
			p.feet[1] = start[1];
			p.feet[2] = start[2];
			p.onGround = startOnGround;
			p.vz = fminf(p.vz, 0.0f);
			// In the air a surface stops the body's own speed too (OpenMW's solver clips the velocity at what it hits):
			// the take-off speed of a huge jump (Icarian Flight) kept pushing into a cliff face, every step undone, and
			// the player hung there for good
			if (!startOnGround && !levitating)
				p.inertia[0] = p.inertia[1] = 0.0f;
			// Falling onto a face steeper than the floor test takes (a ridge between two rocks): the body would hang
			// there for ever, so it is pushed aside off the face, as OpenMW's actors slide down a slope too steep to stand on
			if (!startOnGround && !levitating && endZ < start[2] - 0.01f)
				slideOffSteepFace(p, scene);
			break;
		}
	}

	p.fits = bodyFits(scene, p.feet);
}
