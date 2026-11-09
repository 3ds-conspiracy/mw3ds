#pragma once

// Session's state for sound: loops, voices, ambient and battle music (OpenMW's SoundManager)
// Session derives from it: the members are Session's (s.waterChannel), kept here beside the code that uses them
#include "mwbase/sessiontypes.hpp"

struct SoundState
{
	int waterChannel = -1;            // looping shore ambience in cells with water
	float waterVolume = 0.0f;         // ... its volume now (0..1, before the sound's own: tests read it)
	float waterNext = 0.0f;           // ... when the shore around the player is sampled next (w.time)
	float listener[4] = {};           // x, y, z, yaw of the camera for 3D sound
	int uiVoiceChannel = -1;          // voice lines not tied to an actor (class questions)
	// Where each actor's animation was last frame, for its sound keys (updateAnimationSounds): ref -> group, time, frame
	struct AnimSoundAt { int group; float time; u32 frame; };
	std::unordered_map<int, AnimSoundAt> animSoundAt;
	// Sounds that loop on an object (PlayLoopSound3D), mixed by distance each frame, until stopped or left
	struct LoopSound { int ref; std::string id; int channel; float volume, pitch; bool heard = false; };   // (heard: loud and near enough for a channel)
	std::vector<LoopSound> loopSounds;
	struct ScriptSound { int ref; std::string id; float until; };   // a sound a script played (GetSoundPlaying)
	std::vector<ScriptSound> scriptSounds;
	// The "Sound:" keys still ahead in the groups scripts played on objects (playObjectGroup), due at w.time
	struct ObjectSoundKey { int ref; float at; std::string id; };
	std::vector<ObjectSoundKey> objectSoundKeys;
	float ambientTimer = 3.0f;                     // until the region's next ambient sound
	// Music: exploring (shuffled), battle while something fights the player (back 5 s after)
	bool battleMusic = false;
	float calmFor = 0.0f;
};
