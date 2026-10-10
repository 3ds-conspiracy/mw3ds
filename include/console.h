#pragma once

// Which console the game runs on (main sets these at start, before anything else reads them)
extern bool g_old3ds;            // an Old 3DS: a smaller view, fewer objects in memory, a 30 / 20 fps cap, no 3D by default
extern bool g_sysCoreThreads;    // the system core takes a thread of ours (APT_SetAppCpuTimeLimit worked): cell streaming
