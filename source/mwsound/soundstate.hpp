#pragma once

// Session's state for sound: loops, voices, ambient and battle music (OpenMW's SoundManager)
// Session derives from it: the members are Session's (s.waterChannel), kept here beside the code that uses them
#include "mwbase/sessiontypes.hpp"

struct SoundState
{
	int waterChannel = -1;            // looping shore ambience in cells with water
	float listener[4] = {};           // x, y, z, yaw of the camera for 3D sound
	int uiVoiceChannel = -1;          // voice lines not tied to an actor (class questions)
	// Sounds that loop on an object (PlayLoopSound3D), mixed by distance each frame, until stopped or left
	struct LoopSound { int ref; std::string id; int channel; float volume, pitch; };
	std::vector<LoopSound> loopSounds;
	struct ScriptSound { int ref; std::string id; float until; };   // a sound a script played (GetSoundPlaying)
	std::vector<ScriptSound> scriptSounds;
	float ambientTimer = 3.0f;                     // until the region's next ambient sound
	// Music: exploring (shuffled), battle while something fights the player (back 5 s after)
	bool battleMusic = false;
	float calmFor = 0.0f;
};
