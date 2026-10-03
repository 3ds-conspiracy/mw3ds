#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "actors.h"
#include "viewmodel.h"

struct World;

// The player seen from outside (third-person view, character creation's face preview): the NPC skeleton
// of their race and sex, dressed from the "third" pieces (tools/firstperson.py third_person): the bare
// body, the chosen head and hair, then clothes, armor, skirts and robes as an NPC's are put together
// (tools/npc.py NpcBuilder.parts), the weapon while it is out.
struct PlayerBody
{
	bool ready = false;
	int skel = -1;                                // index into the shared NPC skeletons
	ActorSet set;                                 // actors[0]: the player
	std::vector<C3D_Tex*> textures;               // indexed by the meshes' tex
	std::unordered_map<std::string, FpPiece*> pieces;   // bones resolved against this skeleton
	std::string builtFor, forRace;
	bool forFemale = false;
	const char* group = "";

	void rebuild(World& w, bool weaponOut);      // (re)dress when race, head, hair or equipment changed
	// Animation from what the player does; place at feet, facing yaw
	void update(World& w, float dt, float speed, bool swimming, bool sneaking, bool weaponOut);
	void draw(World& w, bool deform);
	void free(World& w);
};

// The heads / hairs a race and sex can choose from (body ids), for character creation
const std::vector<std::pair<std::string, std::string>>* playerHeadChoices(const World& w, bool hair);
