#pragma once

// Session's state for drawing: spell visuals, the sky and the weather's effects, the third-person camera,
// stereo 3D, the view distance (OpenMW's RenderingManager)
// Session derives from it: the members are Session's (s.thirdPerson), kept here beside the code that uses them
#include "mwbase/sessiontypes.hpp"

struct RenderState
{
	bool thirdPerson = false, previewFace = false;
	// Character creation's face preview: read ahead behind the boat door's loading screen and not shown yet; the race
	// menu was open this session (Session::preloadRacePreview)
	bool racePreloaded = false, raceMenuSeen = false;
	float thirdDistance = 0.0f;               // eased camera distance (walls pull it in)
	// SELECT held: the camera turns round the player (OpenMW's preview mode, Session::povUpdate). povHold: seconds held
	// (-1: not held); orbitYaw: the camera's turn from behind the player, orbitPitch: its own pitch; orbitFromFirst: the
	// hold began in first person, which comes back on release. thirdZoom: the third-person distance, zoomed while held
	float povHold = -1.0f, orbitYaw = 0.0f, orbitPitch = 0.0f, thirdZoom = 220.0f;
	bool orbiting = false, orbitFromFirst = false;
	// Weather outdoors: its ambient loop (rain, ash storm), thunder, and the rain / ash in the world round the eye
	int weatherChannel = -1;
	std::string weatherSound;
	float thunderIn = 10.0f;
	float moonDir[2][3] = {}, moonAlpha[2] = {};     // where Masser and Secunda are and how much of each shows (updateSky)
	u8 moonTint[2][3] = {};                          // and the colour each picture is drawn in (Secunda red: TurnMoonRed)
	float dressTimer = 0.0f;                       // until the next look at what loaded NPCs still wear (updateDress)
	// Spell visuals: a static's piece for a moment, modelled around an actor's feet and facing as it faces (OpenMW's
	// Animation::addEffect), or at a spot (an area burst, blood)
	struct Vfx { std::string piece; float pos[3]; float age, life; int follow; float yaw = 0.0f, scale = 1.0f; bool loop = false; std::string what; int effect = -1;
		bool world = false; float until = -1.0f; std::string bone; bool rest = false; bool ability = false;
		float drawnLo[3], drawnHi[3]; bool drawnSet = false; };   // drawn*: the box of the sprays it drew last frame (queueBlendedExtras)   // rest: a world object's rest piece   // ability: an actor's ability's continuous effect (updateAbilityVfx)   // bone: on that bone of its follow (a casting hand's glow)   // world: a placed object's own (updateWorldFx); until: its time held there once reached   // what: its static's id   // follow: -2 stays put, -1 the player, else a ref whose feet it keeps to
	std::vector<Vfx> vfx;
	static constexpr int kMaxVfx = 12;             // at once: past it a nearer one takes the farthest one's place
	int vfxSprited = 0, vfxSpritedLast = 0;        // visuals that drew particle sprites: this frame so far, the last one
	float queuedEye[3] = { 0.0f, 0.0f, 0.0f };     // the eye the queued sprays and visuals draw for (queueBlendedExtras)
	bool queuedSecondEye = false;
	// Creatures' sprays as sprites shed from their bones (particles.py make_creature_piece): per creature ref its
	// piece, the animation time last seen, each piece mesh's bone, and the live particles (mesh, pos[3], vel[3], age,
	// life, size)
	struct CreatureFx { std::string piece; float prevTime = -1.0f; int prevGroup = -1; std::vector<int> bones;
		std::vector<float> live; std::vector<float> clock; };
	std::unordered_map<int, CreatureFx> creatureFx;
	static constexpr float kWorldFxRange = 12000.0f;   // (a giant like the Akulakhan chamber's model stands far from its origin)
	std::unordered_map<int, std::pair<std::string, float>> objectGroups;   // ref -> group a script played on it, world time then
	// A door's moving collision to take away or put back (WorldFxDef away / back): at World::time `at`, from a group
	// played at `since` (a load that turns the time back drops it)
	struct CollisionDue { float since, at; bool gone; };
	std::unordered_map<int, CollisionDue> objectCollision;
	static constexpr float kVfxRange = 4096.0f;    // farther from the player none starts (many casters at once: their visuals are the cost)
	float armsEye[3] = { 0.0f, 0.0f, 0.0f };      // where the first-person arms were last drawn from (vfxBonePos)
	bool armsEyeSet = false;
	std::unordered_map<std::string, C3D_Tex*> skyTextures;
	float precipDt = 0.0f;                       // game time since the rain and ash last moved
	bool farRainOff = false;                     // the far rain's farther band left out while frames run slow (sky.cpp)
	float renderCost = 0.0f;          // smoothed per-frame render time (ms) of one eye
	float eyeCmdUse = 0.0f;           // share of the GPU command buffer one eye's world takes
	bool stereoCmdRoom = true;
	// View distance Auto outdoors (fog scale, Session::adaptView): a New 3DS's eases with the frame rate; an Old 3DS's is
	// the console's longest view, or its middle one while the longest runs too slow. An Old 3DS also caps the frame rate
	// at 30 or 20 (frameCap: main hands it to C3D_FrameRate)
	float viewScale = g_profile.viewScaleMax, workCost = 0.0f, frameCost = 0.0f;
	// ... and the share drawn: eased, viewScale in steps of kViewStep, moved only once viewScale has left the step by a
	// whole one (the frame cost hovering at its threshold saws viewScale by a few thousandths every frame, and drawn as
	// it is the fog's reach moved with it: the walkways' edges in the fog flashed frame by frame); in two views the same
	// as viewScale (tests set either)
	float viewDrawn = g_profile.viewScaleMax;
	float viewWindowMs = 0.0f;        // the frame rate measured a second at a time: the time so far, and the frames
	int viewWindowFrames = 0;
	float viewRetryIn = 0.0f;         // at the middle view: seconds until the longest is tried again
	float viewRetryWait = 1.0f;       // ... and how long the next drop waits (twice as long each time it fails again soon)
	float viewHighFor = 0.0f;         // at the longest view: seconds it has held
	int frameCap = g_profile.frameCap, capFrames = 0;
	u64 adaptTick = 0;                // when adaptView last ran (once a frame: the time between frames)
};
