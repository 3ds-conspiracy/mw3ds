#pragma once

#include <string>
#include <vector>

struct World;
struct Node;
struct SpellEffect;

// Menus scripts can open (EnableXMenu / ShowRestMenu)
enum ScriptMenu { SMENU_NAME, SMENU_RACE, SMENU_CLASS, SMENU_BIRTH, SMENU_REVIEW, SMENU_REST, SMENU_REST_BED };

// What the interpreter needs from the rest of the game
struct ScriptHost
{
	virtual ~ScriptHost() {}
	virtual bool menuMode() = 0;
	virtual void messageBox(const std::string& text, const std::vector<std::string>& buttons) = 0;
	virtual int takeButtonPressed() = 0;                 // -1 while nothing was chosen
	virtual void say(int ref, const std::string& file, const std::string& text) = 0;
	virtual bool sayDone(int ref) = 0;
	// ref -1 plays 2D; script: from a script (GetSoundPlaying then finds it)
	virtual void playSound(int ref, const std::string& soundId, float volume = 1.0f, float pitch = 1.0f, bool script = false) = 0;
	virtual void openMenu(ScriptMenu menu) = 0;
	virtual bool bedRefused(int bed) { return false; }   // ShowRestMenu on a bed: a werewolf, or an owned one a witness reported (the message is shown)
	virtual void activate(int ref) = 0;                  // the object's default activation
	virtual void forceGreeting(int ref) = 0;
	virtual void dialogueChoice(const std::vector<std::pair<std::string, int>>& choices) = 0;
	virtual void dialogueGoodbye() = 0;
	virtual void notify(const std::string& text) = 0;
	virtual void startCombat(int ref) = 0;
	virtual void stopCombat(int ref) = 0;                // StopCombat: the actor's fight ends, and its allies' with it
	virtual bool talkingTo(int ref) = 0;                 // the open conversation is with this actor (GetTarget "Player")
	virtual void killActor(int ref) = 0;                // SetHealth 0 and the like
	virtual void knockDownActor(int ref) = 0;            // ModCurrentFatigue to 0 or less
	virtual void teleportPlayer(int cell, const float pos[3], float yaw) = 0;   // PositionCell on the player
	virtual void hurtPlayer(float health) = 0;           // HurtStandingActor
	virtual void sheathe() = 0;                          // PayFine: the weapon is put away
	virtual void goToJail() = 0;                         // GotoJail
	virtual void castEffect(int ref, const SpellEffect& e, const std::string& source, int casterRef = -1, float spellCost = 0.0f) = 0;   // ref -1: the player; casterRef: the actor that cast it (Cast)
	virtual void loopSound(int ref, const std::string& soundId, bool start, float volume = 1.0f, float pitch = 1.0f) = 0;   // PlayLoopSound3D / StopSound
	virtual void streamMusic(const std::string& file) = 0;                         // StreamMusic
	virtual bool soundPlaying(int ref, const std::string& soundId) = 0;            // GetSoundPlaying
	virtual void fadeTo(float target, float seconds) = 0;                          // FadeOut (1) / FadeIn (0)
	virtual void equipPlayerItem(const std::string& id) = 0;                       // Equip on the player
	virtual bool playMovie(const std::string& name) = 0;
	virtual bool talking() = 0;                                                    // a conversation is open                           // PlayBink (false: not there)
};

int scriptFunctionId(const std::string& lowerName);

// Runs every running script instance once (local scripts, inventory item scripts, global scripts).
void scriptsRun(World& w, ScriptHost& host, float dt);

// Runs a dialogue result script in the speaker's context.
void scriptRunSnippet(World& w, ScriptHost& host, const Node& block, int speakerRef);
