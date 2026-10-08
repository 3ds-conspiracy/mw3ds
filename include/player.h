#pragma once

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
