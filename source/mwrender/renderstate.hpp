#pragma once

// Session's state for drawing: spell visuals, the sky and the weather's effects, the third-person camera,
// stereo 3D, the view distance (OpenMW's RenderingManager)
// Session derives from it: the members are Session's (s.thirdPerson), kept here beside the code that uses them
#include "mwbase/sessiontypes.hpp"

struct RenderState
{
	bool thirdPerson = false, previewFace = false;
	float thirdDistance = 0.0f;               // eased camera distance (walls pull it in)
	// Weather outdoors: its ambient loop (rain, ash storm), thunder, and the rain / ash in the world round the eye
	int weatherChannel = -1;
	std::string weatherSound;
	float thunderIn = 10.0f;
	float moonDir[2][3] = {}, moonAlpha[2] = {};     // where Masser and Secunda are and how much of each shows (updateSky)
	float dressTimer = 0.0f;                       // until the next look at what loaded NPCs still wear (updateDress)
	// Spell visuals: a static's piece for a moment, modelled around an actor's feet and facing as it faces (OpenMW's
	// Animation::addEffect), or at a spot (an area burst, blood)
	struct Vfx { std::string piece; float pos[3]; float age, life; int follow; float yaw = 0.0f, scale = 1.0f; bool loop = false; std::string what; int effect = -1;
		bool world = false; float until = -1.0f; std::string bone; };   // bone: on that bone of its follow (a casting hand's glow)   // world: a placed object's own (updateWorldFx); until: its time held there once reached   // what: its static's id   // follow: -2 stays put, -1 the player, else a ref whose feet it keeps to
	std::vector<Vfx> vfx;
	static constexpr int kMaxVfx = 12;             // at once: past it a nearer one takes the farthest one's place
	int vfxSprited = 0, vfxSpritedLast = 0;        // visuals that drew particle sprites: this frame so far, the last one
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
	// Adaptive view distance outdoors (fog scale), from the frame's CPU / GPU work and the time between frames. An Old
	// 3DS starts nearer and may close in further, and caps the frame rate at 30 or 20 (frameCap: main hands it to
	// C3D_FrameRate)
	float viewScale = g_profile.viewStartsShort ? g_profile.viewScaleMin : g_profile.viewScaleMax, workCost = 0.0f, frameCost = 0.0f;
	// ... and the share actually drawn: viewScale in steps of kViewStep, moved only once viewScale has left the step
	// by a whole one. The frame cost hovering at its threshold saws viewScale by a few thousandths every frame, and
	// drawn as it is, the fog's reach moved with it: on an Old 3DS at its short view the walkways' edges in the fog
	// flashed frame by frame
	float viewDrawn = g_profile.viewStartsShort ? g_profile.viewScaleMin : g_profile.viewScaleMax;
	int frameCap = g_profile.frameCap, capFrames = 0;
	u64 adaptTick = 0;                // when adaptView last ran (once a frame: the time between frames)
};
