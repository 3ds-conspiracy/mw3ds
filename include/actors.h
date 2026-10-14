#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <string>
#include <vector>

// Animated NPCs from cells/<cell>.act (tools/convert/actors.py documents the layout).

struct RotKey { float t, w, x, y, z; };
struct VecKey { float t, x, y, z; };
struct ScaleKey { float t, s; };

struct Track
{
	int bone;
	std::vector<RotKey> rot;
	std::vector<VecKey> trans;
	std::vector<ScaleKey> scale;
};

struct AnimGroup { char name[16]; float start, loopStart, loopStop, stop; };
struct SkelBone { char name[32]; int parent; float rest[12]; };

struct Skeleton
{
	int numIdle = 0;                   // leading groups named Idle*
	std::vector<SkelBone> bones;
	std::vector<Track> tracks;
	std::vector<int> trackOfBone;      // -1 when the bone isn't animated
	std::vector<AnimGroup> groups;     // [0] = Idle, then Idle2 .. Idle9 when present
	// The movement root ("Bip01" / "Root Bone"): walk and run cycles carry the distance walked in its
	// horizontal translation; the game moves the actor itself, so that part is left out (else the body
	// ran ahead over each cycle and snapped back when it looped)
	int accumBone = -1;
	int headBone = -1;                 // "Bip01 Head": turned toward the player (Actor::headYaw)
	std::vector<float> groupSpeed;     // per group: how fast its root walks (skeleton units / s), 0 in place
};

struct MorphTrack
{
	std::vector<float> keys;           // time, weight pairs
	std::vector<float> delta;          // xyz per vertex
};

enum { ACTOR_RIGID = 0, ACTOR_SKINNED = 1 };
enum { ACTOR_MESH_WEAPON = 128 };      // mesh flag: the weapon, drawn only while fighting

// How the current animation group plays
enum AnimMode { ANIM_IDLE, ANIM_LOOP, ANIM_ONCE, ANIM_HOLD };   // random idles / repeat / then idle / freeze at end

constexpr float kVfxFps = 20.0f;             // track samples a second (firstperson.py VFX_FPS)

struct ActorMesh
{
	int tex;
	u8 flags, alphaRef, kind, hasMorph;
	int bone;                          // rigid: skeleton bone (-1 = actor root)
	u32 numVerts, numIndices;
	void* verts;                       // linear memory, 24-byte cell vertex layout
	u16* indices;                      // linear memory
	std::vector<float> src;            // source positions (skin space or bone space)
	std::vector<int> palBone;
	std::vector<float> palMat;         // 12 floats (3x4) per palette entry
	std::vector<u8> infIndex;          // per vertex: palette index[4]
	std::vector<float> infWeight;      // per vertex: weight[4]
	std::vector<u8> infCount;          // influences in use per vertex (sorted strongest first)
	float talk[2], blink[2];
	// Skinned on the GPU (skin.v.pica): verts are then 24-byte packed vertices (s16 position x posQ, s16 texture
	// coordinates x uvQ, colour, four palette entries, four weights) that are never rewritten; src and the
	// influences are dropped
	bool gpuSkin = false;
	float posQ = 1.0f, uvQ = 1.0f;
	float morphT = -1.0f;              // morph position the vertices hold (-1: none yet)
	u32 glow = 0;                      // enchanted: a pulsing glow in this colour (0xRRGGBB), 0 none
	std::vector<MorphTrack> morphs;
	// A spell visual's mesh that moves (tools/convert/firstperson.py vfx_tracks): 12 floats (3x4, its space to the
	// piece's root) kVfxFps times a second; billboard: it turns to face the eye
	std::vector<float> track;
	std::vector<float> vis;            // shown (1) or hidden (0) from each time on: pairs (time, shown), empty: always shown
	bool driven = false;               // the track runs on the actor's driveT (an arrow on its bow's ArrowBone) at its bone
	u8 hand = 0;                       // 1 a torch (shown while the actor holds it out), 2 a shield (put away then)
	bool billboard = false;
	bool fx = false;                   // a spell visual's shape that takes its magic effect's particle texture (MGEF PTEX)
	// Particle sprays made drawable without simulating them (tools/convert/particles.py, prototype): a flipbook steps
	// its UVs one cell of flipCols x flipRows a frame, flipFps a second; baked paths swap in a frame's vertices
	u16 flipFrames = 0, flipCols = 1, flipRows = 1;
	float flipFps = 20.0f, pathFps = 20.0f;
	std::vector<void*> pathFrames;     // linear memory, numVerts x 24 bytes each
	// Particle records (drawn as sprites): each born, pos[3], vel[3], life, size in piece space; their acceleration,
	// grow / fade times and colour keys (time of a life, 0xAABBGGRR)
	std::vector<float> sprites;
	float spriteAccel[3] = { 0, 0, 0 }, spriteGrow = 0.0f, spriteFade = 0.0f;
	std::vector<float> spriteKeyT;
	std::vector<u32> spriteKeyC;
	float spriteRepeat[3] = { 0, 0, 0 };     // shed again every [0] s from [1] until [2] (a creature's long fire); 0 once
	std::string boneName;                    // piece meshes: the bone the file names (creature particle pieces)
	// A spell visual's texture slide (u, v pairs) and fade (a share of its alpha), kVfxFps samples a second
	std::vector<float> uvTrack, alphaTrack;
	float radius = 0.0f;               // with a track: how far its vertices reach from its origin
};

struct Actor
{
	int ref, skeleton;
	float place[12];                   // actor space -> world (3x4, includes race scale)
	u8 idle[8];                        // Idle2..Idle9 chances
	std::vector<ActorMesh> meshes;

	int group = 0;
	float time = 0.0f;
	std::vector<float> pose;           // 12 floats per bone, skeleton space
	std::vector<float> skinPose;       // the pose the skins were last built from (rigid parts follow it, so hands and arms stay joined); empty without skins
	float talkLevel = 0.0f;            // 0..1 voice loudness
	bool swimming = false;             // in deep water: actorPlay picks the swimming groups
	float blinkTimer = 2.0f, blinkTime = -1.0f;
	bool deformed = false;             // vertex buffers hold a posed mesh
	u32 poseSerial = 0, skinSerial = ~0u;   // bumped by each posing / the pose the skins hold
	AnimMode mode = ANIM_IDLE;
	s8 loopsLeft = -1;                 // looping group: more times round (LoopGroup's count), -1 for ever
	float pendingDt = 0.0f;            // animation time owed while it was skipped (far away)
	float headYaw = 0.0f;             // head turned this far (radians, around the actor's up axis)
	float headPitch = 0.0f;           // and tilted (radians, up positive), after the turn
	float rate = 1.0f;                 // animation speed: walk / run cycles follow how fast the actor moves
	float lastXY[2] = { 1e9f, 1e9f };  // where it stood last frame
	bool showWeapon = false;
	bool torchOut = false;             // holds its torch out (night, out of a fight: World::actorTorchOut)
	bool driveMorph = false;           // the morphing meshes follow driveT (a bow's string) instead of talking and blinking
	float driveT = 0.0f;               // the morph's time on the held weapon's own timeline
	bool frozen = false;               // held where it is in its group (the test driver's POSE): no time passes, no other group starts
};

struct ActorSet
{
	std::vector<Skeleton> skeletons;
	std::vector<Actor> actors;
	u32 bytes = 0;
	u32 skinnedGpu = 0, skinnedCpu = 0;   // skinned meshes read: drawn by skin.v.pica / re-skinned on the CPU
};

// A cell's .act: version 3 lists placements whose meshes are in library files (under dataDir), their
// textures by name in the cell's list (texNames)
bool actorsLoad(ActorSet& set, const char* path, const char* dataDir, const std::vector<std::string>& texNames);
struct Cell;
struct TextureCache;
// A creature / NPC placed while playing: its meshes from a library file (dataDir/lib); textures the cell
// doesn't list yet are added to it. Returns its index in set.actors, -1 when it couldn't be read.
int actorsAddFromLibrary(ActorSet& set, Cell& cell, TextureCache& cache, const char* dataDir, const std::string& lib,
	int ref, int skeleton, const float place[12]);
// cells/skeletons.skl: skeletons shared by every cell's actors (.act version 2 carries none). With
// cells/skel_<k>.skl present each is read when an actor first needs it (skeletonEnsure)
extern std::vector<Skeleton> g_sharedSkeletons;
void skeletonsFree();                          // a session ending: the shared skeletons freed
bool skeletonsLoad(const char* path);
void skeletonEnsure(int i);          // any thread (the streaming one reads cells' actors)
// Frees the shared skeletons not in `keep` (indices): each is read the first time an actor of its kind is met and, kept for
// ever, the ones a long walk meets added up to 14 MB, a third of an Old 3DS's heap. Main thread, no cell being streamed
// (its actors would be reading one), nothing holding a reference into one. Returns the KB freed.
int skeletonsTrim(const std::vector<int>& keep);
bool skeletonsLoadInto(const char* path, std::vector<Skeleton>& out);    // any 'MWS1' file
inline const Skeleton& actorSkeleton(const ActorSet& set, int i)
{
	return set.skeletons.empty() ? g_sharedSkeletons[i] : set.skeletons[i];
}
void actorsFree(ActorSet& set);
// Advances the idle animation and blinking, and poses the skeleton.
void actorAnimate(ActorSet& set, Actor& a, float dt);
// Rewrites animated vertex positions (skinned meshes, morphing heads). Call after C3D_FrameBegin.
void actorDeform(ActorSet& set, Actor& a);
// How rarely (in frames) an actor squared-distance-from-the-camera away is posed and re-skinned: the far ones less
// often, but never so rarely that an animal across a field moves in jerks
u32 actorPoseEvery(float dist2);
// The largest weight any of an actor's driven morphs (a bow's string) holds at its drive time: 0 at rest, 1 drawn
float actorMorphWeight(const Actor& a);
u32 actorSkinEvery(float dist2);
// GPU skinning (on unless a test turns it off): skinned meshes with at most kSkinBones palette entries are drawn by
// skin.v.pica from bone matrices, the rest still re-skinned on the CPU
extern bool g_gpuSkinning;
static const int kSkinBones = 25;
// A GPU-skinned mesh's palette: 12 floats (3x4, actor space) per entry, from the pose its rigid neighbours use
void actorSkinPalette(const Actor& a, const ActorMesh& m, float* out);
// Group index by name, or -1
int actorFindGroup(const Skeleton& sk, const char* name);
// Starts a group; false when the skeleton doesn't have it
bool actorPlay(ActorSet& set, Actor& a, const char* group, AnimMode mode);
// Moves / turns the actor (keeps its race scale); yaw 0 faces +Y
void actorSetPlacement(Actor& a, float x, float y, float z, float yaw);
// place x bone pose for a rigid mesh
void actorMeshMatrix(const Actor& a, const ActorMesh& m, C3D_Mtx* out);
bool actorMeshShown(const Actor& a, const ActorMesh& m);   // its visibility keys allow it now
// One of the groups prefix1, prefix2, ... the skeleton has, at random (OpenMW's chooseRandomGroup: Hit1..Hit5,
// Death1..Death5, a creature's Attack1..3); "" when it has none
std::string actorRandomGroup(const ActorSet& set, const Actor& a, const char* prefix);
