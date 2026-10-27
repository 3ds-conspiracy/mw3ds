#pragma once

// Session's state for scripted test runs: the harness's switches and its monitors
// Session derives from it: the members are Session's (s.testKit), kept here beside the code that uses them
#include "mwbase/sessiontypes.hpp"

struct TestState
{
	int testKit = 0;              // ENHANCEV2: the v2 test kit (2); the driver's v2 behaviours read kitV2()
	struct Watch
	{
		float x = 0, y = 0, still = 0; bool told = false;
		float off = 0; bool toldFloor = false;                       // standing off the floor under them
		float anchorX = 0, anchorY = 0, moveT = 0; bool toldStuck = false;   // going somewhere, getting nowhere
	};
	// Engine-side checks (monitor: lines): the player's fall, doors, memory per cell
	float playerAirT = 0.0f;
	bool toldFall = false;
	int monitorCell = -1;
	std::map<std::string, std::vector<unsigned long>> cellMemory;
	std::unordered_map<int, Watch> watches;
	struct Skate { float x = 0, y = 0, secs = 0; bool seen = false; };
	std::unordered_map<int, Skate> skates;  // actors moving while they play an idle, seconds of it (EXPECT:refskate)
	std::string traceActor;                         // TRACE:<id>: that actor's position, heading, group and health logged every frame
	float traceTime = 0.0f;
	float monitorTimer = 0.0f;           // harness GOD: the player can't die, every blow lands and kills
	int loadScreens = 0;                            // trips through the loading screen (finishTravel): EXPECT:loadscreens
	int testPlaced = -1;                            // the actor PLACE last put in front of the player
	std::set<int> testPlacedAll;                    // every actor PLACE / PLACEAT put somewhere (POSE keeps them there)
	// Where the last placed one stands in its cell's own objects (cell file, index in them), for one that was in the
	// cell's data: an unloaded cell's slots are given out again and it comes back in another (the Old 3DS's eviction)
	std::string testPlacedCell;
	int testPlacedLocal = -1;
};
