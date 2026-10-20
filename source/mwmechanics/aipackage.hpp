#pragma once

// AI package numbers, as GetCurrentAIPackage reports them, and an actor's AI state

// AI packages, numbered as GetCurrentAIPackage reports them
enum { AIPKG_NONE = -1, AIPKG_WANDER = 0, AIPKG_TRAVEL = 1, AIPKG_ESCORT = 2, AIPKG_FOLLOW = 3, AIPKG_ACTIVATE = 4,
	AIPKG_IDLE = 5 };   // AIPKG_IDLE: the script's package ended and nothing replaced it (OpenMW: an empty sequence, the actor stands)

enum { AI_IDLE = 0, AI_COMBAT = 1 };
