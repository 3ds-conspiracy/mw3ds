#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "actors.h"
#include "viewmodel.h"

struct World;

// What the player's arms do, taken from the first-person action (in OpenMW one character controller drives both
// views): a blow drawn back, let go, or a spell cast
enum BodyAction { BODY_NONE, BODY_WINDUP, BODY_STRIKE, BODY_CAST };

struct BodyAct
{
	BodyAction action = BODY_NONE;
	const char* group = "";                       // the NPC group: Attack1h ... AttackHH, CastSelf / CastTouch / CastTarget
	float charge = 0.0f;                          // wind-up: 0..1 between the min and max attack marks
	float speed = 1.0f;                           // the weapon's speed: blows play at this rate
	float strength = 1.0f;                        // a blow let go: small / medium / large follow-through by it
	const char* stance = "";                      // the weapon's family for its ready stance: 1h 2c 2w HH
};

// The player seen from outside (third-person view, character creation's face preview): the NPC skeleton
// of their race and sex, dressed from the "third" pieces (tools/convert/firstperson.py third_person): the bare
// body, the chosen head and hair, then clothes, armor, skirts and robes as an NPC's are put together
// (tools/convert/npc.py NpcBuilder.parts), the weapon while it is out.
struct PlayerBody
{
	bool ready = false;
	int skel = -1;                                // index into the shared NPC skeletons
	ActorSet set;                                 // actors[0]: the player
	std::vector<C3D_Tex*> textures;               // indexed by the meshes' tex
	std::unordered_map<std::string, FpPiece*> pieces;   // bones resolved against this skeleton
	std::string builtFor, forRace;
	bool forFemale = false;
	const char* group = "";                       // the walk / idle group asked for last ("" after a blow or cast)
	BodyAction acting = BODY_NONE;                // the arms' action being played
	std::string actGroup;
	bool followCut = false;                       // a blow's follow-through was chosen at its hit
	bool stanceOut = false;                       // the walk / idle was chosen with the weapon out

	void rebuild(World& w, bool weaponOut);      // (re)dress when race, head, hair or equipment changed
	// Animation from what the player does; place at feet, facing yaw
	void update(World& w, float dt, float speed, bool swimming, bool sneaking, bool weaponOut, const BodyAct& act);
	const char* playing() const;                  // the group being played ("" when none)
	void draw(World& w, bool deform);
	void free(World& w);
};

// The heads / hairs a race and sex can choose from (body ids), for character creation
const std::vector<std::pair<std::string, std::string>>* playerHeadChoices(const World& w, bool hair);
