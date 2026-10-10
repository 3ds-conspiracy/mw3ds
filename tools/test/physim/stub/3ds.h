// Host stand-in for libctru's types (tools/test/physim builds the game's physics on the PC)
#pragma once
#include <cstdint>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t s16;
typedef int32_t s32;
// cell.h's per-cell lock (one thread here)
typedef int LightLock;
inline void LightLock_Init(LightLock* l) { *l = 1; }
inline void LightLock_Lock(LightLock*) {}
inline void LightLock_Unlock(LightLock*) {}
