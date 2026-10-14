#include "log.h"
#include "memstat.h"

#include <3ds.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <malloc.h>
#include <new>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

static FILE* s_log;
// Each write to the card costs about 12 ms on hardware (a 25-minute log: 12 ms between two lines with nothing between
// them, three lines a second), so in play the lines gather here and the main thread writes them every 2 s
// (logBuffered); a stall the watchdog sees, a crash and the quit write them at once
static char s_logBuf[16 * 1024];
static bool s_buffered = false;
static u64 s_flushedAt = 0;
static volatile size_t s_failedSize = 0;        // the last `new` that found no room (operator new below)
static void* volatile s_failedSite = nullptr;

const char* volatile g_mainAt = "start";
const char* volatile g_workerAt = "idle";
volatile unsigned g_mainFrames = 0;

static void watchdogNote(const char* fmt, ...)
{
	// Its own file first: the stuck thread may hold the log file (a flush that never returns), and
	// then this thread stops in logf after this line
	char line[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	if (FILE* f = fopen("sdmc:/3ds/mw3ds/watchdog.txt", "a"))
	{
		fprintf(f, "[%8llu] %s\n", osGetTime() % 100000000ULL, line);
		fclose(f);
	}
	logf("watchdog: %s", line);
	if (s_log)
		fflush(s_log);
}

static Thread s_watchdog = nullptr;
static LightEvent s_watchdogStop;

static void watchdogThread(void*)
{
	unsigned last = 0;
	int still = 0;
	for (;;)
	{
		if (LightEvent_WaitTimeout(&s_watchdogStop, 1000000000LL) == 0)
			return;                       // watchdogStop
		unsigned f = g_mainFrames;
		if (f != last)
		{
			// Recovery lands in watchdog.txt too: a stall that ran again and a freeze that needed
			// the power button now read apart
			if (still >= 4)
				watchdogNote("ran again after %d s", still);
			last = f;
			still = 0;
			continue;
		}
		// Keeps writing while the game is down (every 30 s): the last line's count says how long it
		// hung, and a mark that changes between lines says it is stuck in a loop, not one wait
		if (++still == 4 || (still > 4 && still % 30 == 0))
			watchdogNote("no frame for %d s, main at %s, streaming at %s, linear free %lu KB",
				still, g_mainAt, g_workerAt, (unsigned long)(linearSpaceFree() / 1024));
	}
}

static void crashNote(const char* what, u32 pc, u32 lr, u32 far)
{
	char line[256];
	snprintf(line, sizeof(line), "%s: pc %08lx lr %08lx address %08lx, main at %s, streaming at %s, %s thread", what,
		(unsigned long)pc, (unsigned long)lr, (unsigned long)far, g_mainAt, g_workerAt, threadGetCurrent() ? "streaming" : "main");
	if (FILE* f = fopen("sdmc:/3ds/mw3ds/crash.txt", "a"))
	{
		fprintf(f, "[%8llu] %s\n", osGetTime() % 100000000ULL, line);
		fclose(f);
	}
	// ... and in the log, already open: with the heap gone, fopen (which allocates) can't open crash.txt, and an
	// Old 3DS run that froze out of memory left no word of it anywhere
	if (s_log)
	{
		fprintf(s_log, "[%8llu] crash: %s\n", osGetTime() % 100000000ULL, line);
		fflush(s_log);
	}
}

static void crashHandler(ERRF_ExceptionInfo* e, CpuRegisters* r)
{
	static const char* kinds[] = { "prefetch abort", "data abort", "undefined instruction", "VFP exception" };
	crashNote(e->type < 4 ? kinds[e->type] : "exception", r->pc, r->lr, e->far);
	for (;;)
		svcSleepThread(1000000000LL);
}

static u8 s_crashStack[2][0x1000] __attribute__((aligned(8)));

static void outOfMemory()
{
	// The reserve first: `new` tries again with it free, and the main loop gives memory back (g_memoryLow)
	if (memoryReserveSpend())
	{
		if (s_log)
		{
			fprintf(s_log, "[%8llu] memory: out of heap, the reserve spent: a new of %u bytes from %08lx (%s thread, main at %s)\n",
				osGetTime() % 100000000ULL, (unsigned)s_failedSize, (unsigned long)s_failedSite,
				threadGetCurrent() ? "streaming" : "main", g_mainAt);
			fflush(s_log);
		}
		return;
	}
	// The streaming thread: wait a little for the main thread to give memory back (g_memoryLow), then try again
	if (threadGetCurrent())
	{
		// A long wait is logged so a route that stalls here names itself (the main thread may have
		// nothing left to give back: Session::relieveMemory escalates while this one waits)
		static int streak = 0;
		if (!g_memoryLow)
			streak = 0;              // the main thread gave some back since the last failure
		if (++streak % 250 == 0)
			logf("memory: streaming thread still waiting for heap (%d tries, %d KB used, wants %u KB from %08lx)", streak,
				mallinfo().uordblks / 1024, (unsigned)(s_failedSize / 1024), (unsigned long)s_failedSite);
		g_memoryLow = true;
		svcSleepThread(20000000LL);
		return;
	}
	// (lr: where the `new` was; address: how many bytes it wanted)
	crashNote("out of memory (new)", 0, (u32)s_failedSite, (u32)s_failedSize);
	g_mainAt = "out of memory";        // (the watchdog's lines say so from here on)
	for (;;)
		svcSleepThread(1000000000LL);
}

// Every `new` of 128 KB or more, by call site (logged when a site is new or asks for a quarter more than before), and
// the size and site of the one that ran out: an Old 3DS's heap ends up with megabytes free in holes of a few hundred KB,
// and these are the requests that can't find room (tools\build\addr2line.ps1 names the site)
static const size_t kBigNew = 128 * 1024;

static void noteBigNew(size_t n, void* site)
{
	static struct { void* site; size_t most; } s_sites[64];
	static LightLock lock = 1;
	static bool inside = false;
	LightLock_Lock(&lock);
	if (!inside)
	{
		inside = true;
		int k = 0;
		while (k < 64 && s_sites[k].site && s_sites[k].site != site)
			k++;
		if (k < 64 && (!s_sites[k].site || n > s_sites[k].most + s_sites[k].most / 4))
		{
			s_sites[k].site = site;
			s_sites[k].most = n;
			if (s_log)
			{
				fprintf(s_log, "[%8llu] memory: new of %u KB from %08lx (%s thread, main at %s, %u KB used)\n",
					osGetTime() % 100000000ULL, (unsigned)(n / 1024), (unsigned long)site,
					threadGetCurrent() ? "streaming" : "main", g_mainAt, (unsigned)(mallinfo().uordblks / 1024));
				fflush(s_log);
			}
		}
		inside = false;
	}
	LightLock_Unlock(&lock);
}

void* operator new(size_t n)
{
	if (n == 0)
		n = 1;
	for (;;)
	{
		if (void* p = malloc(n))
		{
			if (n >= kBigNew)
				noteBigNew(n, __builtin_return_address(0));
			return p;
		}
		s_failedSize = n;
		s_failedSite = __builtin_return_address(0);
		outOfMemory();
	}
}

void* operator new[](size_t n)
{
	return operator new(n);
}

static void terminated()
{
	crashNote("std::terminate", 0, (u32)__builtin_return_address(0), 0);
	for (;;)
		svcSleepThread(1000000000LL);
}

void crashHandlerInstall(int slot)
{
	threadOnException(crashHandler, s_crashStack[slot & 1] + sizeof(s_crashStack[0]), WRITE_DATA_TO_HANDLER_STACK);
	if (slot == 0)
	{
		std::set_new_handler(outOfMemory);
		std::set_terminate(terminated);
	}
}

void watchdogStart()
{
	LightEvent_Init(&s_watchdogStop, RESET_STICKY);
	s_watchdog = threadCreate(watchdogThread, nullptr, 8 * 1024, 0x3F, -2, false);
}

// The quit: the watchdog's thread ends before libctru's exit gives the heap back, its stack with it (a thread still
// there that woke in between would fault on its own stack, at the very end of the quit). Joined with a limit: a
// watchdog stuck in a write to the card doesn't hold the quit
void watchdogStop()
{
	if (!s_watchdog)
		return;
	LightEvent_Signal(&s_watchdogStop);
	if (threadJoin(s_watchdog, 2000000000ULL) == 0)
		threadFree(s_watchdog);
	s_watchdog = nullptr;
}

// libctru's __appExit calls this first (after the static destructors, before the SD card and the services go): the
// watchdog covers the quit up to here
extern "C" void userAppExit(void)
{
	watchdogStop();
}

void logInit()
{
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/mw3ds", 0777);
	s_log = fopen("sdmc:/3ds/mw3ds/log.txt", "w");
	if (s_log)
		setvbuf(s_log, s_logBuf, _IOFBF, sizeof(s_logBuf));
}

void logBuffered(bool on)
{
	s_buffered = on;
	if (!on && s_log)
		fflush(s_log);
}

static void (*s_hook)(const char* line) = nullptr;
static bool s_inHook = false;

void logSetHook(void (*hook)(const char* line))
{
	s_hook = hook;
}

void monitorOnce(const char* key, const char* fmt, ...)
{
	static LightLock lock = 0;
	static std::set<std::string> told;
	static bool init = false;
	if (!init)
	{
		LightLock_Init(&lock);
		init = true;
	}
	LightLock_Lock(&lock);
	bool first = told.size() < 4000 && told.insert(key).second;
	LightLock_Unlock(&lock);
	if (!first)
		return;
	char line[400];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	logf("monitor: %s", line);
}

void logf(const char* fmt, ...)
{
	char line[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	u64 now = osGetTime();
	unsigned long long t = now % 100000000ULL;
	if (s_log)
	{
		// The write to the SD card is marked, so a write that never returns (a wedged card) names itself in
		// watchdog.txt instead of the mark of whatever logged. Buffered (play): the main thread writes every 2 s
		const char* savedAt = g_mainAt;
		bool main = threadGetCurrent() == nullptr;
		if (main)
			g_mainAt = "log write";
		fprintf(s_log, "[%8llu] %s\n", t, line);
		if (!s_buffered || (main && now - s_flushedAt >= 2000))
		{
			fflush(s_log);
			s_flushedAt = now;
		}
		if (main)
			g_mainAt = savedAt;
	}
	// Only on the main thread: the hook draws (a loading screen), and a line from the streaming
	// thread drawing at the same time as the main thread hung the game
	if (s_hook && !s_inHook && threadGetCurrent() == nullptr)
	{
		s_inHook = true;
		s_hook(line);
		s_inHook = false;
	}
}

void logExit()
{
	if (s_log)
		fclose(s_log);
	s_log = nullptr;
}
