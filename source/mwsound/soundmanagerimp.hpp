#pragma once

#include <string>
#include <vector>

// Sound files are 'SND3' mono DSP-ADPCM, the DSP decoding them (tools/convert/dspadpcm.py), under <dataDir>/snd and
// /music; data converted earlier may still hold 'SND2' (IMA ADPCM, decoded here) or 'SND1' (PCM16).
bool audioInit(const char* dataDir);
void audioExit();
unsigned audioCacheBytes();                       // sounds held (linear memory)
void audioCacheClear();                           // sounds freed (a session ending)
bool audioEnabled();

int audioStartedCount(const std::string& file);   // sounds of this file started so far
float audioStartVolume(const std::string& file);  // the volume it last started at (before the effects slider; -1: never)
int audioDeniedCount();                           // sounds that found no free channel
// Returns a channel id, or -1 if nothing plays (no DSP firmware, missing file, no free channel).
int audioPlay(const std::string& file, float volume, float pan, bool loop = false, float pitch = 1.0f);
bool audioPlaying(int channel);
void audioStop(int channel);
void audioSetMix(int channel, float volume, float pan);
float audioDuration(const std::string& file);      // seconds (a loaded sound's own; else read from the header)
float audioLoudness(int channel);                  // RMS 0..1 around the current play position

// Loops the playlist, streaming from disk. Call audioUpdate() every frame.
// loop: start the list over at its end; false: silence after the last track (the main menu's title music, as OpenMW)
void audioMusicPlay(const std::vector<std::string>& files, float volume, bool loop = true);
// Options: overall effects (sounds and voices) and music volume, 0..1
void audioSetScales(float effects, float music);
void audioUpdate();
