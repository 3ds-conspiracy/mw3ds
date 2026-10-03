#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "actors.h"

struct World;

// One converted part / item model (data/fp/<name>.fpm, tools/convert/firstperson.py), bones resolved
// against the first-person skeleton
struct FpPiece
{
	std::vector<ActorMesh> meshes;
	std::vector<std::string> texNames;       // what each mesh's tex indexes
	std::vector<C3D_Tex*> textures;
	u32 bytes = 0;
};

// Reads data/fp/<name>.fpm, bones named in it resolved against sk (nullptr when missing)
struct World;
struct Skeleton;
// (*missing: set when the file isn't there at all, as against a load that ran out of memory)
FpPiece* fpLoadPiece(World& w, const Skeleton& sk, const std::string& name, bool* missing = nullptr);
// Gives back a piece's geometry and the textures it holds (a texture that failed to load held a count too)
void fpFreePiece(World& w, FpPiece* p);
// Tries again the textures that failed to load, and says so once if one still has none (it draws white)
void fpRetryTextures(World& w, FpPiece* p);
int fpUntextured(const FpPiece* p);       // meshes that should have a texture and have none

// What the arms are doing; main input drives it through Session (combat.cpp)
enum VmAction
{
	VM_NONE,          // weapon away: nothing drawn (or the empty hands idle when fighting bare-handed)
	VM_EQUIP, VM_IDLE, VM_WINDUP, VM_FOLLOW, VM_UNEQUIP, VM_BLOCK, VM_HIT, VM_KNOCKDOWN, VM_CAST,
};

// The player's first-person arms and what they hold (viewmodel.cpp)
struct ViewModel
{
	bool ready = false;                       // skeleton loaded
	ActorSet set;                             // skeletons[0]: the first-person skeleton; actors[0]: the arms
	std::vector<C3D_Tex*> textures;           // indexed by the arm meshes' tex
	std::unordered_map<std::string, FpPiece*> pieces;
	std::string builtFor;                     // equipment key the meshes were put together for
	bool incomplete = false;                  // a piece or texture was short when it was built: tried again
	std::unordered_map<std::string, u32> failedAt;    // pieces that ran out of memory: when (ms), to try again later
	u32 retryAt = 0;
	VmAction action = VM_NONE;
	std::string group;                        // weapon animation family: 1h 2c 2w HH Bow Xbow Throw
	std::string kind = "Chop";                // Chop / Slash / Thrust (melee), Shoot (ranged)
	float charge = 0.0f;                      // wind-up: 0..1 between min and max attack
	float speed = 1.0f;                       // the weapon's speed: the swing's animation plays at this rate
	bool visible = false;

	bool load(World& w);
	void free(World& w);
	FpPiece* piece(World& w, const std::string& name);    // cached; nullptr when missing (or short of memory: retried)
	// Puts the arms together for the race and equipment (cheap when nothing changed)
	void rebuild(World& w, const std::string& weaponId, const std::string& shieldId);
	void play(VmAction a, const char* groupName, bool hold = false);
	// Length of a group in seconds (0 if the skeleton lacks it)
	float groupLength(const char* groupName) const;
	bool finished() const;                    // the current one-shot group reached its end
	bool windupReached() const;               // the wind-up got to its min attack mark (a blow can be let go)
	bool releaseReached() const;              // a cast got to its release mark (the spell takes effect)
	void update(float dt, bool moving, bool running);
	void draw(const float eye[3], float yaw, float pitch, bool deform = true);
};
