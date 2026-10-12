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
	float fallTop = 0.0f;     // highest point since last on the ground
	float landedFall = 0.0f;  // how far the last landing dropped (read and cleared by the session: fall damage)
};

// What the player collides with: every loaded cell; water and the rest come from the one they're in
struct Scene
{
	std::vector<Cell*> cells;
	Cell* here = nullptr;
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
// Gyro aim's soft threshold (GyroWiki's "tightening"): a turn under `soft` degrees a second is scaled down by how far
// under it is (a shaking hand's jitter all but gone, a slow aim kept), one over it passes whole; no hard dead zone
inline float gyroSoften(float rate, float soft)
{
	float a = fabsf(rate);
	return a >= soft || soft <= 0.0f ? rate : rate * (a / soft);
}
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
