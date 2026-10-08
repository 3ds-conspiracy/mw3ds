// Test driver: harness actions that play the game the way a player would (walk there, face it, press A,
// swing), for the automated story tests (tools/test/cases/*.txt, main.cpp's autoinput). Used only by the harness.
//
//   GOD:1              the player can't die and every blow lands and kills (story tests; combat tests don't)
//   WALKTO:id          walk to that reference (path grid when there's one, straight otherwise)
//   WALKTO:@x,y,z      walk to that spot in the current cell (the uber route planner's legs)
//   FLYTO:@x,y,z[:alt] levitating: climb to alt (default 1500 over the higher end), fly straight over, come down
//   HOPTO:@x,y,z       running jumps toward the spot, steered in the air to land on it (Icarian Flight legs)
//   ESCORT:id          from here walks wait (stand still) while that NPC is over 600 away (ESCORT:- ends it)
//   KILL:id            walk up to it and fight it with the equipped weapon until it's dead
//   STRIKE:id:secs     the same, but just keep swinging for that long (a target that can't die: the Heart)
//   ACTIVATE:id        walk up, look at it, press A (doors, levers, shrines, containers, people)
//   PICKUP:id          the same, for an item lying in the world (its OnPCAdd / pickup scripts run)
//                      A ':' inside a name is written '|' (DOORTO:Wolverine_Hall|_Mage's_Guild, EXPECT:cell:...); a name
//                      ending in '$' must match exactly (EXPECT:cell:Vivec,_Arena$)
//   DOORTO:cell        walk to the nearest door leading to a cell whose name starts so ("outside": any door out
//                      to the exteriors) and open it (then wait for the new cell: EXPECT:cell)
//   LOOT:id:item       walk up to a container or body, open it (A), take that item as its screen does
//   EQUIP:id           equip a carried item as the inventory screen does (a potion / ingredient: drink / eat it)
//   EXPECT:what:...    check and log "expect: PASS|FAIL ..." (see TestDriver::expect)
//   LEGIT              from here only what a player can do (TestDriver::playerToken): no warps, no direct activation,
//                      services only from the one being talked to, spells only known ones (uber quest tests)
//
// Walking, killing and activating block the step until done (or its seconds run out: "drive: FAIL timeout").
// A walk that makes no progress for a few seconds jumps once, then gives up with "drive: stuck" and warps next
// to the target (so one blocked path doesn't sink the rest of a chapter; the log still fails the run).
#pragma once
#include <string>
#include <vector>
#include <3ds/types.h>
#include "player.h"

class Session;

struct TestDriver
{
	enum Kind { NONE, WALK, KILL, ACTIVATE, SEARCH, TAKE, LOOTWAIT, FLY, HOP, TALKWAIT, DOORWAIT };
	Kind kind = NONE;
	std::string id;
	int ref = -1;
	std::vector<int> path;           // path grid points still to walk through
	float progressAt[3] = { 0, 0, 0 };
	float progressTimer = 0.0f, swingTimer = 0.0f, lookTimer = 0.0f, elapsed = 0.0f;
	int stuckTries = 0;
	bool doorTried = false;
	int doorLast = -1;               // the inner door last opened on the way (another one is tried; the same only if a trap ate the press)
	float lineTimer = 0.0f;          // KILL: seconds spent stepping aside from someone in the line to the target
	float paralysedLog = 0.0f;       // seconds until "paralysed" is logged again
	int doorCell = -1;               // DOORWAIT: the cell the press was made in
	bool doorTrapped = false;        // DOORWAIT: the door carried a trap when pressed
	float sidestep = 0.0f, sidestepTimer = 0.0f, lastGoal = 1e9f;
	bool pressA = false;
	bool pickup = false;             // PICKUP: a book opens to read: Take it (TAKE waits for the screen)
	float takeTimer = 0.0f;
	float strikeFor = 0.0f;          // STRIKE: seconds of swings, then done (0: KILL, until dead)
	std::string lootItem;            // LOOT: what to take once its container screen is open
	bool lootPut = false;            // PUT: lootItem goes from the inventory into the container instead
	std::string searchVerb, searchArg;          // SEARCH: what to start once the target is in the loaded cells
	bool pointGoal = false;          // WALKTO:@x,y,z: a spot in the current cell, not a reference
	float goal[3] = { 0, 0, 0 };
	// FLYTO (levitating: climb to `cruise`, straight there, down) and HOPTO (running jumps, steered in the air)
	int phase = 0, climbs = 0, hops = 0;
	float cruise = 0.0f, phaseTimer = 0.0f;
	float outTimer = 0.0f;           // FLYTO: seconds left of a stretch sideways out from under an overhang
	int outTries = 0;
	float flyAllow = 0.0f;           // FLYTO: seconds the leg may take (from its distance), beyond the step's own
	float aim[3] = { 0, 0, 0 };     // FLYTO: the spot asked for (goal moves around it when a roof is in the way)
	bool inAir = false;
	// LEGIT: the target hidden from where we stand (behind a basket): walk round it and try again (three sides)
	std::string retryToken, thenDo;
	int viewTries = 0, talkTries = 0;
	std::string escort;              // ESCORT:<npc>: walks wait for them when they fall behind (ESCORT:- ends it)    // TALKWAIT: presses that opened no talk (they walked off the crosshair)
	bool retrying = false;              // HOPTO: in the air since the last jump (its landing is checked once)
	bool updateFly(Session& s, PlayerInput& in, float dt);
	bool updateHop(Session& s, PlayerInput& in, float dt);
	float searchTimer = 0.0f, repathTimer = 0.0f;
	int findTarget(Session& s, const std::string& verb, const std::string& arg);

	// Starts an action token (true: it blocks the step until done); EXPECT / GOD / EQUIP happen at once
	bool start(Session& s, const std::string& token);
	// While busy: steers the player (the step's input is replaced); false once done
	bool update(Session& s, PlayerInput& in, u32& down, float dt);
	bool busy() const { return kind != NONE; }
	void fail(const char* why);
	static bool expect(Session& s, const std::string& spec);
	static bool canCast(Session& s, const std::string& selected);   // a known spell, or "item:<id>" carried
	static bool playerToken(const std::string& tok);                 // allowed after LEGIT (a player's own action)
};
