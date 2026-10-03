#pragma once

#include <string>
#include <vector>

// Sound files are 'SND1' 22 kHz mono PCM16 (tools/level.py) under <dataDir>/sound and /music.
bool audioInit(const char* dataDir);
void audioExit();
void audioCacheClear();                        // decoded sounds freed (a session ending)
bool audioEnabled();

int audioStartedCount(const std::string& file);   // sounds of this file started so far
int audioDeniedCount();                           // sounds that found no free channel
// Returns a channel id, or -1 if nothing plays (no DSP firmware, missing file, no free channel).
int audioPlay(const std::string& file, float volume, float pan, bool loop = false, float pitch = 1.0f);
bool audioPlaying(int channel);
void audioStop(int channel);
void audioSetMix(int channel, float volume, float pan);
float audioDuration(const std::string& file);      // seconds, read from the header
float audioLoudness(int channel);                  // RMS 0..1 around the current play position

// Loops the playlist, streaming from disk. Call audioUpdate() every frame.
void audioMusicPlay(const std::vector<std::string>& files, float volume);
// Options: overall effects (sounds and voices) and music volume, 0..1
void audioSetScales(float effects, float music);
void audioUpdate();
