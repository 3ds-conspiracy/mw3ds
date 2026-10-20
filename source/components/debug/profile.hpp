#pragma once

// Call counts of the engine's hot paths, summed over a second and logged by main with the profile lines (per
// frame): how many times a second's frames asked for a disposition, cast a ray through a cell's collision, sampled
// a floor, stepped an actor, looked an object up by id. Counts cost nothing and say what a slow bucket did.
enum { PCALL_DISPOSITION, PCALL_RAYS, PCALL_FLOORS, PCALL_STEPS, PCALL_OBJECTS, PCALL_COUNT };
extern int g_profCalls[PCALL_COUNT];

// The per-frame work that runs on a cadence or a shortcut (the Old 3DS's frame): each can be put back to every frame
// on the PC build with MW3DS_NOCADENCE=<bit mask>, to find which one a test blames. On the console all are on
enum { CAD_WEIGHT = 1, CAD_FIGHT = 2, CAD_GREET = 4, CAD_HEAD = 8, CAD_TORCH = 16, CAD_LIGHTS = 32, CAD_LOOPS = 64,
	CAD_FLOOR = 128, CAD_MONITOR = 256 };
bool cadenceOn(int which);
