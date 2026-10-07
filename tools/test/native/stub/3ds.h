// Host stand-in for <3ds.h>: just enough for the game to compile and run headless on the PC
#pragma once
#include "3ds/types.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>

#define BIT(n) (1U << (n))
enum {
	KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3), KEY_DRIGHT = BIT(4), KEY_DLEFT = BIT(5),
	KEY_DUP = BIT(6), KEY_DDOWN = BIT(7), KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10), KEY_Y = BIT(11),
	KEY_ZL = BIT(14), KEY_ZR = BIT(15), KEY_TOUCH = BIT(20), KEY_CSTICK_RIGHT = BIT(24), KEY_CSTICK_LEFT = BIT(25),
	KEY_CSTICK_UP = BIT(26), KEY_CSTICK_DOWN = BIT(27), KEY_CPAD_RIGHT = BIT(28), KEY_CPAD_LEFT = BIT(29),
	KEY_CPAD_UP = BIT(30), KEY_CPAD_DOWN = BIT(31),
	KEY_UP = KEY_DUP | KEY_CPAD_UP, KEY_DOWN = KEY_DDOWN | KEY_CPAD_DOWN, KEY_LEFT = KEY_DLEFT | KEY_CPAD_LEFT,
	KEY_RIGHT = KEY_DRIGHT | KEY_CPAD_RIGHT,
};
struct touchPosition { u16 px, py; };
struct circlePosition { s16 dx, dy; };
inline void hidScanInput() {}
inline u32 hidKeysDown() { return 0; }
inline u32 hidKeysHeld() { return 0; }
inline u32 hidKeysUp() { return 0; }
inline void hidTouchRead(touchPosition* t) { t->px = t->py = 0; }
inline void hidCircleRead(circlePosition* c) { c->dx = c->dy = 0; }
inline void irrstScanInput() {}
inline void hidCstickRead(circlePosition* c) { c->dx = c->dy = 0; }
inline Result svcGetThreadId(u32* id, u32) { *id = 1; return 0; }
#define GX_TRANSFER_FLIP_VERT(x) ((x) << 0)
#define GX_TRANSFER_OUT_TILED(x) ((x) << 1)
#define GX_TRANSFER_RAW_COPY(x) ((x) << 3)
#define GX_TRANSFER_IN_FORMAT(x) ((x) << 8)
#define GX_TRANSFER_OUT_FORMAT(x) ((x) << 12)
#define GX_TRANSFER_SCALING(x) ((x) << 24)
#define GX_TRANSFER_FMT_RGB8 0
#define GX_TRANSFER_FMT_RGBA8 1
#define GX_TRANSFER_SCALE_NO 0
inline void irrstCstickRead(circlePosition* c) { c->dx = c->dy = 0; }
inline Result irrstInit() { return 0; }
inline void irrstExit() {}
inline bool aptMainLoop() { return true; }
inline void gspWaitForVBlank() {}
inline Result APT_CheckNew3DS(bool* b) { *b = true; return 0; }
inline void osSetSpeedupEnable(bool) {}
inline u64 osGetTime() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
#define SYSCLOCK_ARM11 268111856ULL
inline u64 svcGetSystemTick() { return (u64)(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() * SYSCLOCK_ARM11); }
inline void svcSleepThread(s64 ns) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }
inline Result romfsInit() { return -1; }
inline void romfsExit() {}
inline void gfxInitDefault() {}
inline void gfxExit() {}
inline void gfxSet3D(bool) {}
inline float osGet3DSliderState() { return 0; }

// One thread only: threadCreate fails, and the game then does that work on the spot (world.cpp streaming)
typedef int LightLock;
inline void LightLock_Init(LightLock* l) { *l = 1; }
inline void LightLock_Lock(LightLock*) {}
inline void LightLock_Unlock(LightLock*) {}
inline int LightLock_TryLock(LightLock*) { return 0; }
typedef int RecursiveLock;
inline void RecursiveLock_Init(RecursiveLock*) {}
inline void RecursiveLock_Lock(RecursiveLock*) {}
inline void RecursiveLock_Unlock(RecursiveLock*) {}
typedef void* Thread;
typedef void (*ThreadFunc)(void*);
inline Thread threadCreate(ThreadFunc, void*, size_t, int, int, bool) { return nullptr; }
inline Thread threadGetCurrent() { return nullptr; }
inline Result threadJoin(Thread, u64) { return 0; }
inline void threadFree(Thread) {}
inline Result svcGetThreadPriority(s32* p, u32) { *p = 0x30; return 0; }
#define CUR_THREAD_HANDLE 0xFFFF8000u
#define U64_MAX UINT64_MAX
inline Result GSPGPU_FlushDataCache(const void*, u32) { return 0; }
inline Result DSP_FlushDataCache(const void*, u32) { return 0; }
void* linearAlloc(size_t size);
void linearFree(void* p);
size_t linearSpaceFree();
struct mallinfo_s { int uordblks, arena, fordblks; };

// Sound: a buffer is finished the moment it is queued (no real-time playback in the tests)
enum { NDSP_OUTPUT_STEREO, NDSP_INTERP_LINEAR, NDSP_FORMAT_MONO_PCM16, NDSP_WBUF_FREE = 0, NDSP_WBUF_QUEUED, NDSP_WBUF_PLAYING, NDSP_WBUF_DONE };
struct ndspWaveBuf { const void* data_vaddr; u32 nsamples; u32 offset; bool looping; u16 sequence_id; u8 status; };
inline Result ndspInit() { return 0; }
inline void ndspExit() {}
inline void ndspSetOutputMode(int) {}
inline void ndspChnReset(int) {}
inline void ndspChnSetInterp(int, int) {}
inline void ndspChnSetRate(int, float) {}
inline void ndspChnSetFormat(int, int) {}
inline void ndspChnSetMix(int, float*) {}
inline void ndspChnWaveBufAdd(int, ndspWaveBuf* b) { b->status = NDSP_WBUF_DONE; }
inline void ndspChnWaveBufClear(int) {}
inline u32 ndspChnGetSamplePos(int) { return 0; }

// The on-screen keyboard: never opened in a test (the name is set by the harness)
struct SwkbdState { int unused; };
enum { SWKBD_TYPE_NORMAL, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_BUTTON_CONFIRM, SWKBD_BUTTON_NONE };
typedef int SwkbdButton;
inline void swkbdInit(SwkbdState*, int, int, int) {}
inline void swkbdSetHintText(SwkbdState*, const char*) {}
inline void swkbdSetInitialText(SwkbdState*, const char*) {}
inline void swkbdSetValidation(SwkbdState*, int, int, int) {}
inline SwkbdButton swkbdInputText(SwkbdState*, char* buf, size_t) { strcpy(buf, "Tester"); return SWKBD_BUTTON_CONFIRM; }
