// Test driver: harness actions that play the game the way a player would (walk there, face it, press A,
// swing), for the automated story tests (tools/tests/*.txt, main.cpp's autoinput). Used only by the harness.
//
//   GOD:1              the player can't die and every blow lands and kills (story tests; combat tests don't)
//   WALKTO:id          walk to that reference (path grid when there's one, straight otherwise)
//   KILL:id            walk up to it and fight it with the equipped weapon until it's dead
//   STRIKE:id:secs     the same, but just keep swinging for that long (a target that can't die: the Heart)
//   ACTIVATE:id        walk up, look at it, press A (doors, levers, shrines, containers, people)
//   PICKUP:id          the same, for an item lying in the world (its OnPCAdd / pickup scripts run)
//   DOORTO:cell        walk to the nearest door leading to a cell whose name starts so ("outside": any door out
//                      to the exteriors) and open it (then wait for the new cell: EXPECT:cell)
//   LOOT:id:item       walk up to a container or body, open it (A), take that item as its screen does
//   EQUIP:id           equip a carried item as the inventory screen does (a potion / ingredient: drink / eat it)
//   EXPECT:what:...    check and log "expect: PASS|FAIL ..." (see TestDriver::expect)
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
	enum Kind { NONE, WALK, KILL, ACTIVATE, SEARCH, TAKE, LOOTWAIT };
	Kind kind = NONE;
	std::string id;
	int ref = -1;
	std::vector<int> path;           // path grid points still to walk through
	float progressAt[2] = { 0, 0 };
	float progressTimer = 0.0f, swingTimer = 0.0f, lookTimer = 0.0f, elapsed = 0.0f;
	int stuckTries = 0;
	bool doorTried = false;
	float sidestep = 0.0f, sidestepTimer = 0.0f, lastGoal = 1e9f;
	bool pressA = false;
	bool pickup = false;             // PICKUP: a book opens to read: Take it (TAKE waits for the screen)
	float takeTimer = 0.0f;
	float strikeFor = 0.0f;          // STRIKE: seconds of swings, then done (0: KILL, until dead)
	std::string lootItem;            // LOOT: what to take once its container screen is open
	bool lootPut = false;            // PUT: lootItem goes from the inventory into the container instead
	std::string searchVerb, searchArg;          // SEARCH: what to start once the target is in the loaded cells
	float searchTimer = 0.0f, repathTimer = 0.0f;
	int findTarget(Session& s, const std::string& verb, const std::string& arg);

	// Starts an action token (true: it blocks the step until done); EXPECT / GOD / EQUIP happen at once
	bool start(Session& s, const std::string& token);
	// While busy: steers the player (the step's input is replaced); false once done
	bool update(Session& s, PlayerInput& in, u32& down, float dt);
	bool busy() const { return kind != NONE; }
	void fail(const char* why);
	static bool expect(Session& s, const std::string& spec);
};
