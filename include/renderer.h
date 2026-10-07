#pragma once

#include <vector>

#include "world.h"

// Camera in Morrowind coordinates (Z up). Yaw 0 looks north (+Y), positive turns toward +X.
struct RenderCamera
{
	float pos[3];
	float yaw, pitch;
};

enum
{
	RENDER_SKY = 1, RENDER_OPAQUE = 2, RENDER_LAYERS = 4, RENDER_DOORS = 8, RENDER_ACTORS = 16, RENDER_BLENDED = 32,
	RENDER_EXTRAS = 64,          // projectiles and the first-person arms (Session::drawExtras)
};
extern u32 g_renderParts;

void rendererInit();
void rendererExit();
// Draws the cell (static batches, swinging doors, then transparent batches). Returns draw calls.
// eyeShift: stereo 3D eye offset (world units; 0 = mono). secondEye: the right eye of a stereo frame,
// reusing the left eye's visibility, sorting and animation. fogScale: shorter view distance (3D).
float rendererStereoPixels(float depth, float eyeShift);   // a stereo eye's offset of a point at that depth
int rendererDrawWorld(World& w, const RenderCamera& cam, float eyeShift = 0.0f, bool secondEye = false,
	float fogScale = 1.0f);
// After rendererDrawWorld, in the same frame: one mesh with a model matrix (nullptr = world space)
void rendererDrawMesh(const ActorMesh& m, const C3D_Mtx* model, C3D_Tex* tex);
// Sky bodies for the next frames' sky pass: a textured square facing the eye in direction dir (unit,
// world axes), size its half width at the sky's radius; additive for the sun's glare and the stars
struct SkyBillboard { float dir[3]; float size; C3D_Tex* tex; u32 rgba; bool additive; float roll; };
extern std::vector<SkyBillboard> g_skyBillboards;
extern float g_starAlpha;                         // the star dome: 0 by day
// The light the player carries (a torch): world position, radius (0: none), colour 0..1
void rendererSetCarriedLight(const float pos[3], float radius, const float rgb[3]);
extern C3D_Tex* g_starTexture;
// Enchanted items' shimmer: dataDir/art/magicitem/caust00..31.t3x, read once (without them a plain tint)
void rendererLoadCaustics(const char* dataDir);
int rendererCausticFrames();                    // how many of them were read
// rendererDrawMesh, with the shimmer of an enchantment in that colour (0xRRGGBB) over it; 0: none
void rendererDrawMeshGlow(const ActorMesh& m, const C3D_Mtx* model, C3D_Tex* tex, u32 glow);
// A posed actor whose mesh textures are `textures`; viewModel draws it in front of the world
void rendererDrawActor(Actor& a, const std::vector<C3D_Tex*>& textures, bool viewModel);
// Additive camera-facing quad (spell bolts); rgba as 0xAABBGGRR
void rendererDrawGlow(const float pos[3], float size, u32 rgba);
// Local map: renders the loaded cells seen from above (x/y square from minX, minY, size units wide;
// looking down from topZ through depth units) into a 256 x 256 texture. Inside a frame, before the
// screens are drawn. The texture: rendererLocalMap() (nullptr until the first render).
bool rendererDrawLocalMap(World& w, float minX, float minY, float size, float topZ, float depth);
C3D_Tex* rendererLocalMap();

// Day and night outdoors: tint on the baked light, the sky's tint and the fog / clear colour (0..1)
void rendererSetDaylight(const float land[3], const float sky[3], const float fog[3], bool outdoors);
