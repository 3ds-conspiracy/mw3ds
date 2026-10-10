// The program's two heaps, set up before main (libctru's weak __system_allocateHeaps, replaced here).
//
// libctru splits what the process may still commit into the linear heap (GPU-visible: textures, vertices,
// sound) and the C++ heap: at most 32 MB linear, the rest heap. That suits a New 3DS (124 MB mode: ~90 MB
// heap). On an Old 3DS (64 MB mode) it gives 32 MB linear and only ~29 MB heap, while the game data alone
// takes most of that; the split here gives the heap more and the linear heap less (the Old 3DS data build
// has quarter-size textures).
//
// An Old 3DS also never gets more than the 64 MB mode here, even when the kernel offers more: Azahar runs
// a .3dsx as an Old 3DS in its 96 MB mode, and a test there should run out of memory where a real console
// would (run-test.ps1 -Old3ds).
#include <3ds.h>

extern "C"
{
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
extern char* fake_heap_start;
extern char* fake_heap_end;

// Old 3DS: the linear heap's size (the C++ heap gets the rest)
static const u32 kOld3dsLinear = 22u << 20;
static const u32 kOld3dsMode = 64u << 20;

void __system_allocateHeaps(void)
{
	Handle reslimit = 0;
	if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE)))
		svcBreak(USERBREAK_PANIC);
	ResourceLimitType which = RESLIMIT_COMMIT;
	s64 limit = 0, used = 0;
	svcGetResourceLimitLimitValues(&limit, reslimit, &which, 1);
	svcGetResourceLimitCurrentValues(&used, reslimit, &which, 1);
	svcCloseHandle(reslimit);

	// app_memtype: 0..5 are the Old 3DS modes (64 / 96 / 80 / 72 / 32 MB), 6 and 7 the New 3DS ones
	bool old3ds = osGetApplicationMemType() < 6;
	if (old3ds && limit > (s64)kOld3dsMode)
		limit = kOld3dsMode;
	u32 remaining = (u32)(limit - used) & ~0xFFFu;

	if (old3ds)
		__ctru_linear_heap_size = remaining / 2 < kOld3dsLinear ? (remaining / 2) & ~0xFFFu : kOld3dsLinear;
	else
	{
		// as libctru: half each up to a 24 MB heap, then linear grows to 32 MB, the rest heap
		__ctru_linear_heap_size = remaining <= (48u << 20) ? (remaining / 2) & ~0xFFFu : remaining - (24u << 20);
		if (__ctru_linear_heap_size > (32u << 20))
			__ctru_linear_heap_size = 32u << 20;
	}
	__ctru_heap_size = remaining - __ctru_linear_heap_size;

	u32 addr;
	if (R_FAILED(svcControlMemory(&addr, OS_HEAP_AREA_BEGIN, 0, __ctru_heap_size, MEMOP_ALLOC, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
		svcBreak(USERBREAK_PANIC);
	__ctru_heap = addr;
	if (R_FAILED(svcControlMemory(&addr, 0, 0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR, (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
		svcBreak(USERBREAK_PANIC);
	__ctru_linear_heap = addr;
	mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
	fake_heap_start = (char*)__ctru_heap;
	fake_heap_end = fake_heap_start + __ctru_heap_size;
}
}
