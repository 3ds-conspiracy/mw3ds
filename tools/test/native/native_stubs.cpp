// What the 3DS provided and the native test build (tools/test/native) stands in for: the sdmc:/ folder,
// the log, the linear heap, and the parts that only draw, update the build or watch for hangs.
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
#include <direct.h>
#include <io.h>

#include <3ds.h>
#include <citro3d.h>
#include "audio.h"
#include "devupdate.h"
#include "linear.h"
#include "log.h"
#include "renderer.h"
#include "screenshot.h"

extern "C" { u32 __ctru_heap_size = 92380U * 1024, __ctru_linear_heap_size = 32768U * 1024; }

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
	return ::_mkdir(nativePath(path));
}

int native_rmdir(const char* path)
{
	return ::_rmdir(nativePath(path));
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
void* linearAlloc(size_t size)
{
	void* p = malloc(size ? size : 1);
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
	::_mkdir(sdRoot().c_str());
	::_mkdir((sdRoot() + "/3ds").c_str());
	::_mkdir((sdRoot() + "/3ds/mw3ds").c_str());
	s_log = ::fopen((sdRoot() + "/3ds/mw3ds/log.txt").c_str(), "w");
}
void logNetStart(const char*) {}
bool logNetEnsure() { return false; }
void logSetHook(void (*hook)(const char*)) { s_hook = hook; }
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
		fflush(s_log);
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
void crashHandlerInstall(int) {}

// ---- linear lock: one thread
void linearLockInit() {}
LinearGuard::LinearGuard() {}
LinearGuard::~LinearGuard() {}
void* lockedLinearAlloc(size_t size) { return linearAlloc(size); }
void lockedLinearFree(void* p) { linearFree(p); }
void deferredTexDelete(C3D_Tex* tex) { tex->data = nullptr; }
void linearRetire() {}

// ---- nothing to draw, nothing to update
bool screenshotSave(const char*) { return false; }
void devEmptyTrash() {}
DevUpdateResult devUpdate(const std::function<void(const std::string&, const std::string&)>&) { return DEV_NONE; }

u32 g_renderParts = 0;
std::vector<SkyBillboard> g_skyBillboards;
float g_starAlpha = 0;
C3D_Tex* g_starTexture = nullptr;
void rendererInit() {}
void rendererExit() {}
float rendererStereoPixels(float, float) { return 0; }
int rendererDrawWorld(World&, const RenderCamera&, float, bool, float) { return 0; }
void rendererDrawMesh(const ActorMesh&, const C3D_Mtx*, C3D_Tex*) {}
void rendererSetCarriedLight(const float*, float, const float*) {}
void rendererDrawActor(Actor&, const std::vector<C3D_Tex*>&, bool) {}
void rendererDrawGlow(const float*, float, u32) {}
bool rendererDrawLocalMap(World&, float, float, float, float, float) { return false; }
C3D_Tex* rendererLocalMap() { return nullptr; }
void rendererSetDaylight(const float*, const float*, const float*, bool) {}
float g_profActorsMs = 0.0f, g_profWorldMs = 0.0f;
int g_drawnBatches = 0, g_culledBatches = 0, g_skippedDraws = 0;
