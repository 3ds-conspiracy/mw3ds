#include "log.h"

#include <3ds.h>
#include <arpa/inet.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <malloc.h>
#include <new>
#include <set>
#include <string>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static FILE* s_log;

const char* volatile g_mainAt = "start";
const char* volatile g_workerAt = "idle";
volatile unsigned g_mainFrames = 0;

static void watchdogThread(void*)
{
	unsigned last = 0;
	int still = 0;
	bool told = false;
	for (;;)
	{
		svcSleepThread(1000000000LL);
		unsigned f = g_mainFrames;
		if (f != last)
		{
			last = f;
			still = 0;
			if (told)
				logf("watchdog: running again");
			told = false;
			continue;
		}
		if (++still >= 4 && !told)
		{
			// Its own file first: the log's lock may be held by the stuck thread
			if (FILE* f = fopen("sdmc:/3ds/mw3ds/watchdog.txt", "a"))
			{
				fprintf(f, "[%8llu] no frame for %d s, main at %s, streaming at %s\n",
					osGetTime() % 100000000ULL, still, g_mainAt, g_workerAt);
				fclose(f);
			}
			logf("watchdog: no frame for %d s, main thread at %s, streaming at %s", still, g_mainAt, g_workerAt);
			told = true;
		}
	}
}

static void crashNote(const char* what, u32 pc, u32 lr, u32 far)
{
	if (FILE* f = fopen("sdmc:/3ds/mw3ds/crash.txt", "a"))
	{
		fprintf(f, "[%8llu] %s: pc %08lx lr %08lx address %08lx, main at %s, streaming at %s, %s thread\n",
			osGetTime() % 100000000ULL, what, (unsigned long)pc, (unsigned long)lr, (unsigned long)far, g_mainAt,
			g_workerAt, threadGetCurrent() ? "streaming" : "main");
		fclose(f);
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
	crashNote("out of memory (new)", 0, (u32)__builtin_return_address(0), mallinfo().uordblks);
	for (;;)
		svcSleepThread(1000000000LL);
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
	threadCreate(watchdogThread, nullptr, 8 * 1024, 0x3F, -2, true);
}

// Network copy of the log: every line also goes out as a UDP packet to the PC that built the
// CIA (UDP port 8081), so a hang on real hardware shows
// where it stopped without taking the SD card out.
static int s_sock = -1;
static sockaddr_in s_dest;
static u32* s_socBuf = nullptr;

void logInit()
{
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/mw3ds", 0777);
	s_log = fopen("sdmc:/3ds/mw3ds/log.txt", "w");
}

bool logNetEnsure()
{
	if (s_socBuf)
		return true;
	const u32 size = 0x100000;
	s_socBuf = (u32*)memalign(0x1000, size);
	if (!s_socBuf)
		return false;
	if (R_FAILED(socInit(s_socBuf, size)))
	{
		free(s_socBuf);
		s_socBuf = nullptr;
		return false;
	}
	return true;
}

void logNetStart(const char* hostFile)
{
	FILE* f = fopen(hostFile, "r");
	if (!f)
		return;
	char host[64] = {};
	int port = 0;
	int n = fscanf(f, "%63s %d", host, &port);
	fclose(f);
	if (n != 2)
		return;
	if (!logNetEnsure())
		return;
	s_sock = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&s_dest, 0, sizeof(s_dest));
	s_dest.sin_family = AF_INET;
	s_dest.sin_port = htons(port);
	inet_aton(host, &s_dest.sin_addr);
	logf("log: also sent to %s:%d", host, port);
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
	unsigned long long t = osGetTime() % 100000000ULL;
	if (s_log)
	{
		fprintf(s_log, "[%8llu] %s\n", t, line);
		fflush(s_log);
	}
	if (s_sock >= 0)
	{
		char packet[560];
		int len = snprintf(packet, sizeof(packet), "[%8llu] %s", t, line);
		if (len >= (int)sizeof(packet))
			len = sizeof(packet) - 1;
		sendto(s_sock, packet, len, 0, (sockaddr*)&s_dest, sizeof(s_dest));
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
	if (s_sock >= 0)
		close(s_sock);
	s_sock = -1;
	if (s_socBuf)
	{
		socExit();
		free(s_socBuf);
		s_socBuf = nullptr;
	}
	if (s_log)
		fclose(s_log);
	s_log = nullptr;
}
