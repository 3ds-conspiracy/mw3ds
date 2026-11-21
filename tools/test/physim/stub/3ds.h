// Host stand-in for libctru's types (tools/test/physim builds the game's physics on the PC)
#pragma once
#include <cstdint>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;                     // mwrender/animation.hpp's loop counts
typedef int64_t s64;                   // mwgui/savegamedialog.hpp's clock shift
typedef int16_t s16;
typedef int32_t s32;
// log.h's MarkScope picks the watchdog variable from the current thread (one thread here: the main one)
struct Thread;
inline Thread* threadGetCurrent() { return nullptr; }
// log.h's per-run monitor line (the harness has no log)
#include <cstdarg>
inline void monitorOnce(const char*, const char*, ...) {}
// renderingmanager.hpp's drawProfAdd: a fake monotonic arm tick (the harness times nothing)
#define SYSCLOCK_ARM11 268123614u
inline u64 svcGetSystemTick() { static u64 t; return t += 100; }
// cell.h's per-cell lock (one thread here)
typedef int LightLock;
inline void LightLock_Init(LightLock* l) { *l = 1; }
inline void LightLock_Lock(LightLock*) {}
inline void LightLock_Unlock(LightLock*) {}
