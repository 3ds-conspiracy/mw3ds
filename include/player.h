#pragma once
#include <cmath>

#include <vector>

#include "cell.h"

struct PlayerInput
{
	float moveX, moveY;   // -1..1, strafe / forward
	float lookX, lookY;   // -1..1
	bool jump;            // pressed this frame
	bool fast;
	bool up, down;        // fly mode only
	bool attack;          // held: winding up a blow, released: strike
	bool sheathe;         // put the weapon (or fists) away (classic: ZL + X)
	bool readyToggle;     // ready / put away the weapon (Xbox layout: X)
	bool cast;            // pressed: cast the selected spell
	bool sneak;           // held: sneak (L, outside fly mode)
	int quick = -1;       // quick key used this frame (ZL + D-pad: 0 up, 1 right, 2 down, 3 left)
	bool togglePov = false;  // SELECT: first / third person
	bool activate = false;   // GoldenEye: a tap of A (let go before it aimed): activate (the other layouts: A pressed)
	// Gyro aim: how fast the console turns, degrees a second (sensitivity applied), around its up axis (+ right)
	// and across (+ up). Turned into view angles one to one, not through the stick's look speed
	float gyroYaw = 0.0f, gyroPitch = 0.0f;
};

struct Player
{
	float feet[3];
	float yaw, pitch;     // yaw 0 looks north (+Y), positive turns toward +X
	float vz = 0.0f;
	bool onGround = false;
	bool flying = false;
	bool swimming = false;    // treading water at the surface
	bool sneaking = false;
	float knockTimer = 0.0f;  // > 0: knocked down, no control
	float eyeDrop = 0.0f;     // current lowering of the eye (sneaking, knocked down), eased
	bool waterWalk = false;   // Water Walking: the surface holds
	bool slowFall = false;    // Slow Fall: gentle falls
	float levitate = 0.0f;    // Levitate's magnitude: flying where the view points, with collision
	float loadSpeed = 1.0f;   // how much the load slows walking (1 unburdened, 0 over-encumbered)
	// From the stats each frame (Session: Speed, Athletics, Acrobatics, the load, fatigue)
	float runSpeed = 330.0f;  // full tilt of the Circle Pad
	float flySpeed = 330.0f;  // levitating: fMinFlySpeed .. fMaxFlySpeed by Speed + Levitate (OpenMW)
	float swimFactor = 0.6f;  // of the run speed, in water
	float sneakFactor = 0.4f; // of the run speed, sneaking
	float jumpSpeed = 330.0f; // take-off speed
	float airControl = 1.0f;  // in the air, the share of the run speed the pad still steers (fJumpMoveBase / Mult)
	float inertia[2] = { 0.0f, 0.0f };   // a running jump's take-off along the ground, kept until landing (OpenMW)
	bool jumpFlight = false;  // in the air since a jump (not a fall off a ledge)
	bool jumpedNow = false;   // set by the jump this frame (fatigue)
	float swimBoost = 0.0f;   // Swift Swim's magnitude (percent faster in the water)
	bool fits = false;        // the body had room where it stood at the end of the last update
	bool heldUp = false;      // the feet are held where they were: nothing loaded under them yet (an exterior cell streaming in)
	float fallTop = 0.0f;     // highest point since last on the ground
	float landedFall = 0.0f;  // how far the last landing dropped (read and cleared by the session: fall damage)
};

// What the player collides with: every loaded cell; water and the rest come from the one they're in
struct Scene
{
	std::vector<Cell*> cells;
	Cell* here = nullptr;
	// Outdoors, whether the exterior cell under the player's feet is loaded (false while it streams in: nothing to stand
	// on there yet, and a loaded neighbour's collision grid can reach over the border); indoors always true
	bool groundThere = true;
};

static const float PLAYER_EYE_HEIGHT = 124.0f;   // eye level of a default-height race

// Eye height above the world origin (lowered while sneaking or knocked down)
inline float playerEyeZ(const Player& p) { return p.feet[2] + PLAYER_EYE_HEIGHT - p.eyeDrop; }

// Places the player so the eye is at `eye`, then drops the feet onto the floor below.
void playerSpawn(Player& p, Scene& scene, const float eye[3], float yaw, float pitch);
void playerUpdate(Player& p, Scene& scene, const PlayerInput& in, float dt);
// The gyroscope's raw counts as degrees a second: HIDUSER_GetGyroscopeRawToDpsCoefficient gives the counts for one
// degree a second (14.375 on the console's sensor), so the counts are divided by it (a coefficient under 1 would be
// degrees a second per count, and multiplies)
inline float gyroRawToDps(float raw, float coeff)
{
	return coeff >= 1.0f ? raw / coeff : raw * coeff;
}
// A stick's reading (the Circle Pad's or the C-stick's counts, about 150 at full push) past a round dead zone of
// `zone` counts, rescaled so the push grows from 0 at the zone's edge (no jump) to 1 at full; x and y -1..1. A worn
// Circle Pad or a C-stick that doesn't spring back rests a little off centre: with dead zones of 15 / 20 counts on
// each axis, past them, the player kept walking or the view kept turning after the stick was let go, until it was
// moved again
inline void stickAxes(float dx, float dy, float zone, float& x, float& y)
{
	const float kFull = 150.0f;
	float m = sqrtf(dx * dx + dy * dy);
	if (m <= zone)
	{
		x = y = 0.0f;
		return;
	}
	float s = fminf(1.0f, (m - zone) / (kFull - zone)) / m;
	x = dx * s;
	y = dy * s;
}
// Both sticks' dead zone, percent of a full push (Controls > Look: Stick dead zone; settings.txt stickzone=)
extern int g_stickZonePct;
static const int kStickZoneMinPct = 10, kStickZoneMaxPct = 35, kStickZoneStepPct = 5, kStickZoneDefaultPct = 20;
inline float stickZone()
{
	return g_stickZonePct * 1.5f;            // (of the 150 counts of a full push)
}
// Gyro aim's soft threshold (GyroWiki's "tightening"): a turn under `soft` degrees a second is scaled down by how far
// under it is (a shaking hand's jitter all but gone, a slow aim kept), one over it passes whole; no hard dead zone
inline float gyroSoften(float rate, float soft)
{
	float a = fabsf(rate);
	return a >= soft || soft <= 0.0f ? rate : rate * (a / soft);
}
// The gyroscope's resting offset (it reads a little turn while the console is held still), learned and taken off so a
// steady hand doesn't drift the view. Two ways:
// - held still: every axis wobbling under kStill around its own average of the last moments, for a whole second (a
//   hand holding the console wobbles that little; an aim moves): the offset follows that average (some 1.5 s), up to
//   kMost a second (the sensor's are a few). Before, it was learned only near the offset learned so far, starting from
//   0, so an offset over 1.5 a second was never learned and the view drifted to one side for good
// - otherwise, while within 1.5 a second of it: slowly (some 10 s), so a slow, deliberate aim is not mistaken for it
// All three axes at once: learned one axis at a time, a slow steady tilt to aim (the other axes still moving) was taken
// for the offset, and the view drifted back the other way once it stopped (worse on a 2DS than before).
// And calibrate(): the console laid down, the offsets measured outright over kCalibrate seconds (Options > Controls >
// Look: Gyro aim set to a speed, and once as the game starts). A console that moved meanwhile (any axis straying more
// than kCalStray from its average) measures nothing: the offsets stay as they were, as a hand-held calibration stored
// the hand's turn as the offset.
// And the accelerometer, for the up / down axis (x, the console's left-right, lying level in the hands): a console
// whose gravity hasn't turned isn't tilting up or down, so the x it reads then is its offset, learned in about a second
// while it plays (gravity(): each reading, before update). A 2DS reading 2.5 a second on it, held in moving hands,
// was never still enough to learn it: the view sank for good. (Turns about gravity itself, left and right, leave
// gravity as it was: x is learned only while y and z read little, so a turn isn't taken for it.)
// rate: degrees a second, corrected in place; dt: seconds since the reading
struct GyroRest
{
	static constexpr float kCalibrate = 2.0f, kCalStray = 0.8f;
	float rest[3] = { 0.0f, 0.0f, 0.0f };
	float mean[3] = { 0.0f, 0.0f, 0.0f }, wobble[3] = { 0.0f, 0.0f, 0.0f };
	float still = 0.0f;                          // seconds held still
	float calLeft = 0.0f, calSum[3] = {}, calSq[3] = {}, calTime = 0.0f;   // a calibration under way
	bool started = false;
	float gPrev[3] = {}, gTurn = 99.0f, gLevel = 1.0f;   // gravity's direction, how fast it turns (degrees a second, smoothed)
	bool gHas = false;                                   // and how far x is from level (|gravity's x|, 0..1)
	void gravity(const float g[3], float dt)
	{
		float l = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
		if (l < 1.0f || dt <= 0.0f)
			return;
		float u[3] = { g[0] / l, g[1] / l, g[2] / l };
		if (gHas)
		{
			float d = sqrtf((u[0] - gPrev[0]) * (u[0] - gPrev[0]) + (u[1] - gPrev[1]) * (u[1] - gPrev[1])
				+ (u[2] - gPrev[2]) * (u[2] - gPrev[2]));
			gTurn += (d / dt * 57.2958f - gTurn) * fminf(1.0f, dt / 0.3f);
		}
		for (int k = 0; k < 3; k++)
			gPrev[k] = u[k];
		gHas = true;
		gLevel = fabsf(u[0]);
	}
	void calibrate()
	{
		calLeft = kCalibrate + 0.25f;            // (the first quarter second left out: a sensor just turned on reads 0)
		calTime = 0.0f;
		for (int k = 0; k < 3; k++)
			calSum[k] = calSq[k] = 0.0f;
	}
	// 1 the reading that ends a calibration that took, -1 one that didn't (the console moved), else 0
	int update(float rate[3], float dt)
	{
		const float kStill = 1.0f, kMost = 10.0f;
		if (calLeft > 0.0f)
		{
			// (the view held while it measures: the console lies still, every reading is the offset)
			bool counted = calLeft <= kCalibrate;
			for (int k = 0; k < 3; k++)
			{
				calSum[k] += counted ? rate[k] * dt : 0.0f;
				calSq[k] += counted ? rate[k] * rate[k] * dt : 0.0f;
				rate[k] = 0.0f;
			}
			calTime += counted ? dt : 0.0f;
			if ((calLeft -= dt) > 0.0f || calTime <= 0.0f)
				return 0;
			bool steady = true;
			for (int k = 0; k < 3; k++)
			{
				float m = calSum[k] / calTime;
				steady = steady && calSq[k] / calTime - m * m < kCalStray * kCalStray;
			}
			if (!steady)
				return -1;
			for (int k = 0; k < 3; k++)
			{
				rest[k] = mean[k] = calSum[k] / calTime;
				wobble[k] = 0.0f;
			}
			still = 0.0f;
			return 1;
		}
		if (!started)
		{
			for (int k = 0; k < 3; k++)
				mean[k] = rate[k];
			started = true;
		}
		float a = fminf(1.0f, dt / 0.25f), learn = fminf(1.0f, dt / 1.5f), slow = fminf(1.0f, dt / 10.0f);
		bool calm = true, near = true;
		for (int k = 0; k < 3; k++)
		{
			mean[k] += (rate[k] - mean[k]) * a;
			wobble[k] += (fabsf(rate[k] - mean[k]) - wobble[k]) * a;
			calm = calm && wobble[k] < kStill && fabsf(mean[k]) < kMost;
			near = near && fabsf(rate[k] - rest[k]) < 1.5f;
		}
		still = calm ? still + dt : 0.0f;
		// (gravity not turning, x level, no turn on y or z, and x reading no more than an offset's worth)
		bool level = gHas && gTurn < 1.5f && gLevel < 0.5f && fabsf(rate[1] - rest[1]) < 5.0f
			&& fabsf(rate[2] - rest[2]) < 5.0f && fabsf(rate[0] - rest[0]) < 6.0f;
		for (int k = 0; k < 3; k++)
		{
			// (and while gravity turns, x isn't learned at all: a slow steady tilt up or down in steady hands was taken for
			// the offset)
			bool tilting = k == 0 && gHas && gTurn >= 1.5f;
			if (k == 0 && level && still < 1.0f)
				rest[0] += (rate[0] - rest[0]) * fminf(1.0f, dt / 1.0f);
			else if (tilting)
				;
			else if (still >= 1.0f)
				rest[k] += (mean[k] - rest[k]) * learn;
			else if (near)
				rest[k] += (rate[k] - rest[k]) * slow;
			rate[k] -= rest[k];
		}
		return 0;
	}
};
// Gyro aim in "player space" (JibbSmart's GamepadMotionHelpers, GyroWiki): the turn about the world's up (gravity, from
// the accelerometer) steers left / right whatever way the console is tilted in the hands, the turn about the console's
// own left-right axis steers up / down. The 3DS gyroscope and accelerometer share one frame (Azahar's SDL mapping:
// x right, y out of the bottom screen, z toward the hinge, the accelerometer reading gravity itself, (0, -1, 0) lying
// flat). gyro: degrees a second; grav: any length. yawRate + turns the view right, pitchRate + up, degrees a second
inline void gyroPlayerSpace(const float gyro[3], const float grav[3], float& yawRate, float& pitchRate)
{
	float gl = sqrtf(grav[0] * grav[0] + grav[1] * grav[1] + grav[2] * grav[2]);
	float gy = gl > 0.0f ? grav[1] / gl : -1.0f, gz = gl > 0.0f ? grav[2] / gl : 0.0f;
	// the turn about the world's up (+: to the left), no more than the gyro's own y-z turn, and up to sqrt 2 as much
	// as the projection (GamepadMotionHelpers' yaw relax factor: a console held at an angle loses none of the turn)
	float world = -(gy * gyro[1] + gz * gyro[2]);
	float most = sqrtf(gyro[1] * gyro[1] + gyro[2] * gyro[2]);
	float turn = fminf(fabsf(world) * 1.41f, most);
	yawRate = world < 0.0f ? turn : -turn;
	pitchRate = -gyro[0];
}
