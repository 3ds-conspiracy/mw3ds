// What the 3DS provided and the native test build (tools/test/native) stands in for: the sdmc:/ folder,
// the log, the linear heap, and the parts that update the build or watch for hangs.
#undef fopen
#undef stat
#undef remove
#undef rename
#undef mkdir
#undef rmdir
#undef opendir
#undef fmemopen
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define sysMkdir(p) ::_mkdir(p)
#define sysRmdir(p) ::_rmdir(p)
#else
#include <unistd.h>
#undef mkdir        // (native_compat.h redirects these two to the functions below)
#undef rmdir
#define sysMkdir(p) ::mkdir(p, 0777)
#define sysRmdir(p) ::rmdir(p)
#endif
#ifdef _WIN32
#include "native_crash.inc"        // (Windows only: a crash logger, a profiler and a guard-page allocator)
#endif

#include <3ds.h>
#include <citro3d.h>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"

extern "C" { u32 __ctru_heap_size = 92380U * 1024, __ctru_linear_heap_size = 32768U * 1024; }
// NATIVE_OLD3DS=1: the Old 3DS split (source/platform/heaps.cpp): the linear heap the game counts against is smaller
static struct NativeOld3dsHeaps
{
	NativeOld3dsHeaps()
	{
		const char* o = getenv("NATIVE_OLD3DS");
		if (o && o[0] == '1')
		{
			__ctru_linear_heap_size = 22U << 20;
			__ctru_heap_size = 39U << 20;
		}
		// NATIVE_HEAP_KB=n: the heap that much smaller or larger, as the console's own use leaves it (the Old 3DS had
		// 3.3 MB more of its heap in use than this checker at the same point of a save's load)
		if (const char* k = getenv("NATIVE_HEAP_KB"))
			__ctru_heap_size = (u32)atoi(k) * 1024U;
	}
} s_nativeOld3dsHeaps;

// ---- paths
static std::string sdRoot()
{
	const char* e = getenv("NATIVE_SD");
	return e ? e : "build/native/sd";
}

static std::string dataRoot()
{
	const char* e = getenv("MW3DS_DATA");
	return e ? e : "out/data";
}

static std::string s_mapped;

const char* nativePath(const char* path)
{
	static const char kSd[] = "sdmc:";
	static const char kData[] = "sdmc:/3ds/mw3ds/data";
	if (strncmp(path, kData, sizeof(kData) - 1) == 0 && (path[sizeof(kData) - 1] == 0 || path[sizeof(kData) - 1] == '/'))
		s_mapped = dataRoot() + (path + sizeof(kData) - 1);
	else if (strncmp(path, kSd, sizeof(kSd) - 1) == 0)
		s_mapped = sdRoot() + (path + sizeof(kSd) - 1);
	else if (strncmp(path, "romfs:", 6) == 0)
		s_mapped = sdRoot() + "/no-romfs" + (path + 6);
	else
		return path;
	return s_mapped.c_str();
}

FILE* native_fopen(const char* path, const char* mode)
{
	return ::fopen(nativePath(path), mode);
}

int native_stat(const char* path, struct stat* st)
{
	std::string p = nativePath(path);
	while (p.size() > 1 && (p.back() == '/' || p.back() == '\\'))
		p.pop_back();       // Windows stat refuses a trailing slash
	return ::stat(p.c_str(), st);
}

int native_remove(const char* path)
{
	return ::remove(nativePath(path));
}

int native_rename(const char* from, const char* to)
{
	std::string a = nativePath(from);
	std::string b = nativePath(to);
	::remove(b.c_str());      // POSIX rename replaces the target
	return ::rename(a.c_str(), b.c_str());
}

int native_mkdir(const char* path, int)
{
	return sysMkdir(nativePath(path));
}

int native_rmdir(const char* path)
{
	return sysRmdir(nativePath(path));
}

DIR* native_opendir(const char* path)
{
	return ::opendir(nativePath(path));
}

FILE* native_fmemopen(void* buf, size_t size, const char* mode)
{
	FILE* f = ::tmpfile();
	if (!f)
		return nullptr;
	fwrite(buf, 1, size, f);
	rewind(f);
	(void)mode;
	return f;
}

ssize_t decode_utf8(uint32_t* out, const unsigned char* p)
{
	if (p[0] < 0x80) { *out = p[0]; return 1; }
	if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { *out = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); return 2; }
	if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
	{ *out = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); return 3; }
	*out = p[0];
	return 1;
}

// ---- linear heap: plain memory
static size_t s_linearUsed = 0;
static std::map<void*, size_t> s_linearSizes;
#ifdef NATIVE_MEMCAP
extern "C" void* nativeOutsideAlloc(size_t n);     // (the memory checker: not out of the capped C++ heap)
#endif
void* linearAlloc(size_t size)
{
#ifdef NATIVE_MEMCAP
	void* p = nativeOutsideAlloc(size);
#else
	void* p = malloc(size ? size : 1);
#endif
	if (p)
	{
		s_linearUsed += size;
		s_linearSizes[p] = size;
	}
	return p;
}
void linearFree(void* p)
{
	auto it = s_linearSizes.find(p);
	if (it != s_linearSizes.end())
	{
		s_linearUsed -= it->second;
		s_linearSizes.erase(it);
	}
	free(p);
}
size_t linearSpaceFree()
{
	size_t total = __ctru_linear_heap_size;
	return s_linearUsed < total ? total - s_linearUsed : 0;
}

// ---- log (the same lines and file the 3DS build writes, no network, no watchdog)
static FILE* s_log;
const char* volatile g_mainAt = "start";
const char* volatile g_workerAt = "idle";
volatile unsigned g_mainFrames = 0;
static void (*s_hook)(const char*) = nullptr;

void logInit()
{
	sysMkdir(sdRoot().c_str());
	sysMkdir((sdRoot() + "/3ds").c_str());
	sysMkdir((sdRoot() + "/3ds/mw3ds").c_str());
	s_log = ::fopen((sdRoot() + "/3ds/mw3ds/log.txt").c_str(), "w");
}
void logSetHook(void (*hook)(const char*)) { s_hook = hook; }
void logBuffered(bool) {}          // (native runs are tests: the flush below)
void logf(const char* fmt, ...)
{
	char line[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	unsigned long long t = osGetTime() % 100000000ULL;
	if (s_log)
	{
		fprintf(s_log, "[%8llu] %s\n", t, line);
		// (a flush is a system write, about a millisecond with a virus scanner watching: a fifth of a run went to them. Now the
		// verdict lines at once, the rest each half second of real time; a crash flushes all in ncFilter, a killed run loses
		// under half a second)
		static std::chrono::steady_clock::time_point last;
		auto now = std::chrono::steady_clock::now();
		if (strncmp(line, "expect:", 7) == 0 || now - last > std::chrono::milliseconds(500))
		{
			fflush(s_log);
			last = now;
		}
	}
}
void monitorOnce(const char* key, const char* fmt, ...)
{
	static std::set<std::string> told;
	if (told.size() >= 4000 || !told.insert(key).second)
		return;
	char line[400];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	logf("monitor: %s", line);
}
void logExit()
{
	if (s_log)
		::fclose(s_log);
	s_log = nullptr;
}
void watchdogStart() {}
void watchdogStop() {}
#ifdef _WIN32
void crashHandlerInstall(int) { nativeCrashInstall(); }
#else
void crashHandlerInstall(int) {}
#endif

// ---- linear lock: one thread
void linearLockInit() {}
LinearGuard::LinearGuard() {}
LinearGuard::~LinearGuard() {}
void* lockedLinearAlloc(size_t size) { return linearAlloc(size); }
void lockedLinearFree(void* p) { linearFree(p); }
bool inVram(const void*) { return false; }
void texToVramLater(C3D_Tex*) {}       // (no VRAM: the pictures stay where they are)
void texToVramNow() {}
void deferredTexDelete(C3D_Tex* tex) { C3D_TexDelete(tex); }      // one thread: the GPU is done with it
void linearRetire() {}
bool linearReclaim() { return false; }

// ---- nothing to update (drawing and screenshots: native_gpu.cpp)

// ---- NATIVE_AUDIO=<wav>: a mixer for the ndsp stand-ins. Every frame (nativeAudioFrame, called by the GPU
// stand-in's C3D_FrameEnd) mixes 1/30 s of every playing channel into a 16-bit stereo WAV at 32768 Hz, so the
// sound stays in step with the fixed 1/30 s game frames. Off, a queued buffer is finished at once.
namespace
{
const int kDspChannels = 24, kDspQueue = 8, kDspOutRate = 32768, kDspFrameSamples = kDspOutRate / 30;
struct DspChannel
{
	float rate = 32728.0f, mixL = 1.0f, mixR = 1.0f;
	ndspWaveBuf* queue[kDspQueue];
	int count = 0;
	double pos = 0;
	// DSP-ADPCM: the frames are decoded as play reaches them, the last few kept
	int format = NDSP_FORMAT_MONO_PCM16;
	s16 coefs[16] = {};
	int h1 = 0, h2 = 0;
	u32 decoded = 0;               // samples of the playing buffer decoded so far
	s16 recent[4] = {};
};
DspChannel s_dsp[kDspChannels];
FILE* s_wav = nullptr;
bool s_wavTried = false;
u32 s_wavBytes = 0;

bool wavOn()
{
	if (!s_wavTried)
	{
		s_wavTried = true;
		const char* e = getenv("NATIVE_AUDIO");
		if (e && *e)
		{
			s_wav = fopen(e, "wb");
			if (s_wav)
			{
				u8 header[44] = {};
				fwrite(header, sizeof(header), 1, s_wav);
			}
		}
	}
	return s_wav != nullptr;
}
}

void nativeDspReset(int ch)
{
	if (ch < 0 || ch >= kDspChannels)
		return;
	for (int i = 0; i < s_dsp[ch].count; i++)
		s_dsp[ch].queue[i]->status = NDSP_WBUF_DONE;
	s_dsp[ch] = DspChannel();
}
void nativeDspFormat(int ch, int format)
{
	if (ch >= 0 && ch < kDspChannels)
		s_dsp[ch].format = format;
}
void nativeDspCoefs(int ch, const u16* coefs)
{
	if (ch >= 0 && ch < kDspChannels)
		memcpy(s_dsp[ch].coefs, coefs, sizeof(s_dsp[ch].coefs));
}
void nativeDspRate(int ch, float rate)
{
	if (ch >= 0 && ch < kDspChannels)
		s_dsp[ch].rate = rate;
}
void nativeDspMix(int ch, const float* mix)
{
	if (ch >= 0 && ch < kDspChannels)
	{
		s_dsp[ch].mixL = mix[0];
		s_dsp[ch].mixR = mix[1];
	}
}
bool nativeDspAdd(int ch, ndspWaveBuf* b)
{
	if (!wavOn() || ch < 0 || ch >= kDspChannels || s_dsp[ch].count >= kDspQueue)
		return false;
	b->status = s_dsp[ch].count == 0 ? NDSP_WBUF_PLAYING : NDSP_WBUF_QUEUED;
	s_dsp[ch].queue[s_dsp[ch].count++] = b;
	return true;
}
u32 nativeDspPos(int ch)
{
	return ch >= 0 && ch < kDspChannels && s_dsp[ch].count ? (u32)s_dsp[ch].pos : 0;
}

namespace
{
// A buffer starting (or looping): the DSP-ADPCM decoder takes the buffer's start state, or carries on without one
void adpcmStart(DspChannel& c, const ndspWaveBuf* b)
{
	c.decoded = 0;
	if (b->adpcm_data)
	{
		c.h1 = b->adpcm_data->history0;
		c.h2 = b->adpcm_data->history1;
	}
}

// Sample p of the playing buffer (decoded up to p: play only moves forward)
s16 adpcmAt(DspChannel& c, const ndspWaveBuf* b, u32 p)
{
	const u8* d = (const u8*)b->data_vaddr;
	while (c.decoded <= p && c.decoded < b->nsamples)
	{
		u32 k = c.decoded, frame = k / 14, at = k % 14;
		u8 head = d[frame * 8], byte = d[frame * 8 + 1 + at / 2];
		int r = at % 2 ? byte & 15 : byte >> 4;
		r = r >= 8 ? r - 16 : r;
		int pi = (head >> 4) & 7;
		int v = (((r * (1 << (head & 15))) * 2048) + 1024 + c.coefs[pi * 2] * c.h1 + c.coefs[pi * 2 + 1] * c.h2) >> 11;
		v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
		c.h2 = c.h1;
		c.h1 = v;
		c.recent[k & 3] = (s16)v;
		c.decoded++;
	}
	return c.recent[(p < c.decoded ? p : c.decoded - 1) & 3];
}
}

void nativeAudioFrame()
{
	if (!wavOn())
		return;
	static s16 out[kDspFrameSamples * 2];
	static float acc[kDspFrameSamples * 2];
	memset(acc, 0, sizeof(acc));
	for (auto& c : s_dsp)
	{
		double step = c.rate / kDspOutRate;
		for (int i = 0; i < kDspFrameSamples && c.count; i++)
		{
			ndspWaveBuf* b = c.queue[0];
			u32 n = b->nsamples, p = (u32)c.pos;
			float f = (float)(c.pos - p);
			float s0, s1;
			if (c.format == NDSP_FORMAT_ADPCM)
			{
				if (c.decoded == 0)
					adpcmStart(c, b);
				s1 = adpcmAt(c, b, p + 1 < n ? p + 1 : p);
				s0 = adpcmAt(c, b, p);
				if (p + 1 >= n)
					s1 = s0;
			}
			else
			{
				const s16* d = (const s16*)b->data_vaddr;
				s0 = d[p], s1 = p + 1 < n ? d[p + 1] : s0;
			}
			float v = s0 + (s1 - s0) * f;
			acc[i * 2] += v * c.mixL;
			acc[i * 2 + 1] += v * c.mixR;
			c.pos += step;
			if (c.pos >= n)
			{
				c.decoded = 0;
				if (b->looping)
					c.pos -= n;
				else
				{
					b->status = NDSP_WBUF_DONE;
					memmove(c.queue, c.queue + 1, sizeof(c.queue[0]) * (--c.count));
					c.pos = 0;
					if (c.count)
						c.queue[0]->status = NDSP_WBUF_PLAYING;
				}
			}
		}
	}
	for (int i = 0; i < kDspFrameSamples * 2; i++)
		out[i] = (s16)(acc[i] > 32767.0f ? 32767 : acc[i] < -32768.0f ? -32768 : acc[i]);
	fwrite(out, sizeof(out), 1, s_wav);
	s_wavBytes += sizeof(out);
}

void nativeAudioClose()
{
	if (!s_wav)
		return;
	u8 h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
	auto put32 = [&](int at, u32 v) { for (int i = 0; i < 4; i++) h[at + i] = (v >> (i * 8)) & 255; };
	put32(4, 36 + s_wavBytes);
	put32(24, kDspOutRate);
	put32(28, kDspOutRate * 4);
	h[32] = 4;
	h[34] = 16;
	memcpy(h + 36, "data", 4);
	put32(40, s_wavBytes);
	fseek(s_wav, 0, SEEK_SET);
	fwrite(h, sizeof(h), 1, s_wav);
	fclose(s_wav);
	s_wav = nullptr;
}
