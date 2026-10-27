#pragma once

// The renderer's GPU state (mwrender/renderingmanager.cpp and the files split from it): the shader programs and
// their uniforms, what is bound now, the lights, fog and gamma as last set, the frame's queues. One instance, g_gpu
#include <vector>

#include <3ds.h>
#include <citro3d.h>

#include "mwrender/animation.hpp"
#include "mwrender/objects.hpp"

// GPU-skinned actor meshes (ActorMesh::gpuSkin) the actor loop queued, drawn with shaders/skin.v.pica after it: one switch of
// program for them all. Each sends its bone palette (3 rows a bone) and draws its packed vertices as they are
struct SkinDraw { Cell* cell; const Actor* a; const ActorMesh* m; C3D_Mtx mv; float alpha = 1.0f; float tint[3] = { 1, 1, 1 }; };

// Blended draws of the frame, drawn back to front after everything solid (OpenMW's SORT_BACK_TO_FRONT bin): the
// cell's blended batches by their bounds' middle, actors' rigid blended meshes by the actor. A Khajiit's ears (blended
// hair) were drawn before the tapestry behind them, which then painted over them
struct BlendDraw { float d2; Cell* cell; const CellBatch* batch; const ActorMesh* mesh; C3D_Mtx mv; u32 shimmer; const Actor* skinned = nullptr;
	float alpha = 1.0f;               // (an actor drawn see-through: Invisibility, Chameleon)
	float tint[3] = { 1, 1, 1 };      // (an actor's: actorTint; a cell batch's: the world's)
	const void* colors = nullptr; };  // (a shared model's batch: the placement's colours, CellBatch::colors)

struct GpuState
{
	DVLB_s* dvlb;
	shaderProgram_s program;
	int uProjection, uModelView, uUvOffset, uFogVec, uPosOffset, uPosScale, uUvScale, uTint = -1;
	int uLightPos = -1, uLightColor = -1;
	int uLamp = -1;                    // shaders/cell.v.pica lamp: the batch's lamp light the night leaves alone
	int uTorchVec = -1;                // shaders/cell.v.pica / shaders/skin.v.pica torchVec: how far out the torch's light is (per pixel)
	bool torchStageOn = false;         // ... its combiner stage is in (setLight; setBatchState writes it)
	float lampSet[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	int uSide = -1;                    // shaders/cell.v.pica sideLight (rendererSetSunSide), on side-lit batches only
	float sideNow[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, sideSet[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	bool lampValid = false;            // ... as last uploaded (a skinned run's palette overwrites the register)
	// GPU skinning (shaders/skin.v.pica): its own program, the same uniforms in the same registers plus the bone palette
	DVLB_s* skinDvlb;
	shaderProgram_s skinProgram;
	int uBones = -1;
	bool skinReady = false;
	float light[3], lightRadius = 0.0f, lightRgb[3] = { 0, 0, 0 };
	int uAmbient = -1;
	float ambientAdd = 0.0f;           // Night Eye's added light (rendererSetNightEye)
	int bGamma = -1;
	float invGamma = 1.0f;             // Gamma's exponent, 1 / gamma, in ambientAdd.w (rendererSetGamma)
	bool gammaOn = false;
	int bLightOn = -1;                 // shaders/cell.v.pica / shaders/skin.v.pica lightOn: the carried light's terms, skipped without one
	int uStereoCurve = -1, bStereoOn = -1;   // ... stereoCurve / stereoOn: stereo 3D's far depth (rendererSetStereoCurve)
	float stereoCurve[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	bool stereoCurveOn = false;
	C3D_Mtx lightView;               // the view the lights were last put in (set again after a skinned run)
	bool lightViewOn = false;
	// Day and night outdoors (World::daylight): tint on the baked light, the sky's, and the fog colour
	float land[3] = { 1, 1, 1 }, skyTint[3] = { 1, 1, 1 };
	u32 dayFog = 0;
	bool daylight = false;
	bool formatValid = false;                // vertex format / quantization currently bound
	bool packedBound = false;
	float boundQuant[8];                     // pos offset, pos scale, uv scale
	C3D_Mtx view;                            // of the last rendererDrawWorld, for the extra draws
	float eyeSnap[3] = { 0.0f, 0.0f, 0.0f }; // its camera position on a 64-unit grid
	float eye[3] = { 0.0f, 0.0f, 0.0f };     // ... and as it is
	float uvOffset[2];
	C3D_Tex fogRamp;
	C3D_Tex localTex;                        // local map (rendererDrawLocalMap)
	C3D_RenderTarget* localTarget = nullptr;
	C3D_Tex water[64];                       // the water surface's frames (rendererLoadWaterFrames)
	int waterCount = 0;
	float waterFps = 12.0f;
	C3D_Tex caust[32];                       // enchanted items' shimmer frames (rendererLoadCaustics)
	int caustCount = 0;

	// Model-view and position offset uniforms, uploaded only when they change (skinned parts of an actor
	// share one).
	// The shader works in 24-bit floats (17 significant bits). Rebuilding world positions there
	// (posOffset + value * posScale, tens of thousands of units) rounded each cell's copy of a shared
	// terrain edge its own way: hairline cracks with the sky showing through on hardware (emulators
	// compute in 32-bit floats). Cell batches quantize on a power-of-two grid with their offset on it
	// (convert_cell.py write_batches), so for them the shader gets the offset less the camera snapped to
	// 64 units, and the snap goes into the matrix. posOffset + value * posScale is then a multiple of the
	// grid step within +-32768 units of the camera: exact in 24 bits, the same in every cell that shares
	// the vertex
	C3D_Mtx mv;                              // as the caller set it
	float batchOffset[3] = { 0.0f, 0.0f, 0.0f };
	bool batchPacked = false;
	C3D_Mtx boundMv;
	float boundOffset[3];
	bool mvValid = false;

	bool uvOverrideOn = false;
	float uvOverride[2] = { 0.0f, 0.0f };

	float lastFog[2] = { 0.0f, 0.0f };
	float lastHorizonFog[2] = { 0.0f, 0.0f };   // the distant land's past the detailed view (0: none drawn)

	bool underwaterOn = false;
	float underwaterFog = 3.0f;
	float rampRgb[3] = { -1.0f, -1.0f, -1.0f };     // the torch colour and gamma the ramp's rgb was built for
	float rampGamma = -1.0f;
	bool fogOn = false;
	int spritesLeft = 0;          // this frame's particle sprites still to draw (g_profile.spriteBudget)
	int spriteSlot = 0;           // where the frame's next sprite vertices go in rendererDrawSprites' ring
	bool opaqueWater = false;     // rendererDrawLocalMap's opaqueWater, while it draws
	int spritesAsked = 0, spritesDrawn = 0;      // this view's so far: wanted, and drawn within the budget
	int spritesAskedLast = 0, spritesDrawnLast = 0;
	float spritesWanted = 0, spritesWantedLast = 0;   // every visual's live particles, each weighted by nearness
	u32 fogColor = 0;

	// The horizon's scale of the exterior fog distance (main: the weather's alone): distant land is drawn that far with its
	// own fog ramp, whatever closed the detailed view in (the frame time, an Old 3DS's shorter view). Morrowind's
	// mountains stay on the horizon while the expensive world ends nearer (the MGE way)
	float horizonScale = 0.0f;

	C3D_Mtx projection;
	float eyeShift = 0.0f, farPlane = 1000.0f;
	float time;
	int glowCount = 0;

	// The last render state set, so consecutive draws with the same state skip re-setting it (each
	// citro3d state call re-sends GPU commands). Anything else that touches state invalidates it.
	struct { bool valid; C3D_Tex* tex; u8 flags, alphaRef; int mode; float scrollU, scrollV; } last;
	std::vector<SkinDraw> skinQueue;
	std::vector<BlendDraw> blendQueue;

	// ---- Extra meshes after the world: projectiles in flight, then the first-person view model

	// (rendererDrawActor's actor drawn see-through: its alpha, Invisibility or Chameleon)
	float meshAlpha = 1.0f;
};

extern GpuState g_gpu;
