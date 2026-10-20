#pragma once

// A script instance and its local variables (OpenMW's Locals)
#include "mwworld/esmstore.hpp"

struct ScriptInstance
{
	const Script* script = nullptr;
	std::vector<float> locals;
	int ref = -1;                 // owning reference; -1 for global scripts
	int target = -1;              // a global script's implicit reference (ref->StartScript): its calls without one act on it
	std::string targetCell;       // that reference while its cell is out of memory: cell file + index in it (targetIndex),
	int targetIndex = -1;         // bound again (target) when the script next runs
	std::string item;             // item id for scripts carried in the inventory
	bool running = true;
	bool activated = false;       // OnActivate pending for this frame
	bool suppress = false;        // its script asked OnActivate: the activation is held back until the script calls Activate (OpenMW)
	bool buffered = false;        // the player tried to activate it while its script held the activation (Activate then works)
	float lastSayEnd = 0.0f;

	float* local(const std::string& lowerName);
};
