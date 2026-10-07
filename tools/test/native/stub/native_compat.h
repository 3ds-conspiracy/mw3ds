// Force-included into every file of the native test build (tools/test/native). Lets the game's own source run
// headless on the PC: "sdmc:/" paths land in a folder, the POSIX bits Windows lacks are filled in, the 3DS
// font and heap services are stand-ins.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <malloc.h>
#include <algorithm>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <list>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

// ---- paths: sdmc:/3ds/mw3ds/... is NATIVE_SD (default build/native/sd); .../data is MW3DS_DATA (out/world ...)
const char* nativePath(const char* path);
FILE* native_fopen(const char* path, const char* mode);
int native_stat(const char* path, struct stat* st);
int native_remove(const char* path);
int native_rename(const char* from, const char* to);
int native_mkdir(const char* path, int mode);
int native_rmdir(const char* path);
DIR* native_opendir(const char* path);
FILE* native_fmemopen(void* buf, size_t size, const char* mode);
// remove() is also std::remove(first, last, value): the helpers below sort the two apart
namespace std { template<class I, class T> I native_remove_dispatch(I a, I b, const T& v) { return std::remove(a, b, v); }
	inline int native_remove_dispatch(const char* p) { return native_remove(p); } }
template<class I, class T> I native_remove_dispatch(I a, I b, const T& v) { return std::remove(a, b, v); }
inline int native_remove_dispatch(const char* p) { return native_remove(p); }
#define fopen(p, m) native_fopen(p, m)
#define stat(p, s) native_stat(p, s)
#define remove(...) native_remove_dispatch(__VA_ARGS__)
#define rename(a, b) native_rename(a, b)
#define mkdir(p, m) native_mkdir(p, m)
#define rmdir(p) native_rmdir(p)
#define opendir(p) native_opendir(p)
#define fmemopen(b, n, m) native_fmemopen(b, n, m)

// ---- libctru's fonts and utf8
struct CFNT_s { int unused; };
struct FontInfo { int lineFeed; };
struct CharWidthInfo { int charWidth; };
inline void fontEnsureMapped() {}
// The 3DS system font's line feed (30), which also sizes the Morrowind font (ui.cpp fontScale); its glyphs average
// about 13 pixels across
inline FontInfo* fontGetInfo(void*) { static FontInfo f = { 30 }; return &f; }
inline int fontGlyphIndexFromCodePoint(void*, uint32_t cp) { return (int)cp; }
inline CharWidthInfo* fontGetCharWidthInfo(void*, int) { static CharWidthInfo c = { 13 }; return &c; }
ssize_t decode_utf8(uint32_t* out, const unsigned char* in);

// ---- heap
struct mallinfo_t { int uordblks; };
#define mallinfo() mallinfo_t{ 0 }

// ---- the error screen, the crash handler: nothing to install
#define WRITE_DATA_TO_HANDLER_STACK 0

// ---- rand(): devkitARM's newlib generator (64-bit LCG, starts at seed 1), so a run draws the same numbers as the
// 3DS does; the PC's libc has another sequence and (on Windows) RAND_MAX 32767
inline uint64_t& newlib_rand_state() { static uint64_t s = 1; return s; }
inline int newlib_rand() { uint64_t& s = newlib_rand_state(); s = s * 6364136223846793005ULL + 1; return (int)((s >> 32) & 0x7fffffff); }
inline void newlib_srand(unsigned seed) { newlib_rand_state() = seed; }
#undef RAND_MAX
#define RAND_MAX 0x7fffffff
#ifdef NATIVE_NEWLIB_RAND
#define rand() newlib_rand()
#define srand(seed) newlib_srand(seed)
#endif
