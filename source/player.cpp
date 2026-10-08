#include "player.h"

#include <cmath>

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

static bool sampleFloor(Scene& scene, const float feet[3], float zTop, float zBottom, float* zOut)
{
	static const float offsets[5][2] = { {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
	bool found = false;
	for (Cell* cell : scene.cells)
		for (auto& o : offsets)
		{
			float z;
			if (collisionFloor(cell->collision, feet[0] + o[0] * kRadius * 0.7f, feet[1] + o[1] * kRadius * 0.7f, zTop, zBottom, &z)
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
}

// The body hanging on a face too steep to be a floor: the low sphere pushed out of it in 3D, and the sideways part of
// that taken, so the next fall step clears the face
static void slideOffSteepFace(Player& p, Scene& scene)
{
	for (float lift : { 0.0f, kStepUp * 0.5f })
	{
		float c[3] = { p.feet[0], p.feet[1], p.feet[2] + kRadius + lift }, c0[3] = { c[0], c[1], c[2] };
		pushSphere(scene, c, kRadius + 6.0f, false);
		float sx = c[0] - c0[0], sy = c[1] - c0[1], len = sqrtf(sx * sx + sy * sy);
		if (len > 0.05f)
		{
			float move = fmaxf(len, 2.0f) / len;
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
	for (int s = 0; s < steps; s++)
	{
		p.feet[0] += dx / steps;
		p.feet[1] += dy / steps;
		resolveBody(p, scene);
	}

	// Levitating: up and down where the view points (B / R up, L down), floors and ceilings stop it
	if (levitating)
	{
		float vz = in.moveY * sinf(p.pitch) * levSpeed + ((in.jump || in.up) ? levSpeed : 0.0f)
			- (in.down ? levSpeed : 0.0f);
		float newZ = p.feet[2] + vz * dt, floorZ;
		if (sampleFloor(scene, p.feet, p.feet[2] + kStepUp, newZ - 1.0f, &floorZ) && floorZ >= newZ)
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
		bool bottom = sampleFloor(scene, p.feet, p.feet[2] + kStepUp, newZ - 1.0f, &floorZ) && floorZ >= newZ;
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
		float snapBelow = (p.onGround && p.vz <= 0.0f) ? kStepUp : 0.0f;
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
		}
		else
		{
			p.feet[2] = newZ;
			p.onGround = false;
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
	float endZ = p.feet[2];
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
			// Falling onto a face steeper than the floor test takes (a ridge between two rocks): the body would hang
			// there for ever, so it is pushed aside off the face, as OpenMW's actors slide down a slope too steep to stand on
			if (!startOnGround && !levitating && endZ < start[2] - 0.01f)
				slideOffSteepFace(p, scene);
			break;
		}
	}

	p.fits = bodyFits(scene, p.feet);
}
