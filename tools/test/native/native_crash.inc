// Native test build only: logs the faulting address of a crash (offset in the exe plus a stack walk), so an access
// violation can be mapped to a function with the linker map (zig c++ ... -Wl,-Map=...). Included by native_stubs.cpp.
extern "C" {
struct NcRecord { unsigned long code, flags; NcRecord *chain; void *addr; unsigned long nparams; unsigned long long info[15]; };
struct NcPointers { NcRecord *rec; void *ctx; };
typedef long (__stdcall *NcFilter)(NcPointers *);
__declspec(dllimport) NcFilter __stdcall SetUnhandledExceptionFilter(NcFilter);
__declspec(dllimport) void *__stdcall GetModuleHandleA(const char *);
__declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void **, unsigned long *);
}
static long __stdcall ncFilter(NcPointers *p)
{
	char *base = (char *)GetModuleHandleA(0);
	FILE *f = fopen("native_crash.txt", "a");
	if (!f)
		f = stderr;
	fprintf(f, "CRASH code %08lx at +0x%llx, access %llu addr %llx\n", p->rec->code, (unsigned long long)((char *)p->rec->addr - base),
		p->rec->nparams > 0 ? p->rec->info[0] : 0ull, p->rec->nparams > 1 ? p->rec->info[1] : 0ull);
	void *st[62];
	unsigned short n = RtlCaptureStackBackTrace(0, 62, st, 0);
	for (unsigned short i = 0; i < n; i++)
		fprintf(f, "  +0x%llx\n", (unsigned long long)((char *)st[i] - base));
	if (f != stderr)
		fclose(f);
	return 0;
}
static void nativeCrashInstall() { SetUnhandledExceptionFilter(ncFilter); }

// NATIVE_GUARD=1: every operator new gets its own pages with a no-access page right behind it, and a freed block is
// decommitted and its address never reused: a use after free or an overrun then faults at the access, and ncFilter
// logs where. Off by default (runtime switch), slow.
#include <new>
extern "C" {
__declspec(dllimport) void *__stdcall VirtualAlloc(void *, unsigned long long, unsigned long, unsigned long);
__declspec(dllimport) unsigned long __stdcall GetLastError();
__declspec(dllimport) int __stdcall VirtualFree(void *, unsigned long long, unsigned long);
__declspec(dllimport) int __stdcall VirtualProtect(void *, unsigned long long, unsigned long, unsigned long *);
}
static int ncGuard = -1;
static long long ncLive, ncLivePages;
static void *ncAlloc(size_t n)
{
	if (ncGuard < 0)
		ncGuard = getenv("NATIVE_GUARD") ? 1 : 0;
	if (!ncGuard || n > (1u << 20))
	{
		char *m = (char *)malloc(n + 16);
		if (!m)
			return 0;
		((unsigned long long *)m)[0] = 0;
		return m + 16;
	}
	size_t r = (n + 15) & ~(size_t)15;
	size_t pages = (r + 16 + 4095) / 4096;
	if (ncLivePages > 300000)       // ~1.2 GB of guarded blocks alive: a runaway allocation, report where and stop
	{
		FILE *f = fopen("native_crash.txt", "a");
		if (f)
		{
			char *base = (char *)GetModuleHandleA(0);
			void *st[62];
			unsigned short k = RtlCaptureStackBackTrace(0, 62, st, 0);
			fprintf(f, "RUNAWAY: %lld live blocks, %lld pages, request %zu bytes\n", ncLive, ncLivePages, n);
			for (unsigned short i = 0; i < k; i++)
				fprintf(f, "  +0x%llx\n", (unsigned long long)((char *)st[i] - base));
			fclose(f);
		}
		exit(3);
	}
	char *b = (char *)VirtualAlloc(0, (pages + 1) * 4096, 0x3000, 4);
	if (!b)
	{
		FILE *f = fopen("native_crash.txt", "a");
		if (f)
		{
			fprintf(f, "guard alloc of %zu bytes failed, error %lu, live %lld blocks %lld pages\n", n, GetLastError(), ncLive, ncLivePages);
			fclose(f);
		}
		return 0;
	}
	ncLive++;
	ncLivePages += pages;
	unsigned long old;
	VirtualProtect(b + pages * 4096, 4096, 1, &old);
	char *p = b + pages * 4096 - r;
	((unsigned long long *)(p - 16))[0] = (unsigned long long)b;
	((unsigned long long *)(p - 16))[1] = pages;
	memset(p, 0xCD, r);                  // not zero: an uninitialised pointer or count then fails at its use
	return p;
}
static void *ncQuar[1 << 17];                    // the last freed blocks stay reserved (faulting); older ones are released
static unsigned ncQuarAt;
static volatile int ncLock;
static void ncFree(void *p)
{
	if (!p)
		return;
	unsigned long long *h = (unsigned long long *)((char *)p - 16);
	if (h[0] == 0)
	{
		free(h);
		return;
	}
	void *base = (void *)h[0];
	ncLive--;
	ncLivePages -= h[1];
	VirtualFree(base, h[1] * 4096, 0x4000);     // MEM_DECOMMIT
	while (__sync_lock_test_and_set(&ncLock, 1))
		;
	void *old = ncQuar[ncQuarAt & ((1 << 17) - 1)];
	ncQuar[ncQuarAt++ & ((1 << 17) - 1)] = base;
	__sync_lock_release(&ncLock);
	if (old)
		VirtualFree(old, 0, 0x8000);            // MEM_RELEASE
}
void *operator new(size_t n) { void *p = ncAlloc(n); if (!p) abort(); return p; }
void *operator new[](size_t n) { void *p = ncAlloc(n); if (!p) abort(); return p; }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return ncAlloc(n); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return ncAlloc(n); }
void operator delete(void *p) noexcept { ncFree(p); }
void operator delete[](void *p) noexcept { ncFree(p); }
void operator delete(void *p, size_t) noexcept { ncFree(p); }
void operator delete[](void *p, size_t) noexcept { ncFree(p); }
