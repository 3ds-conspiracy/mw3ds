#ifndef _GNU_SOURCE
#define _GNU_SOURCE        // newlib: fopencookie
#endif
#include "components/files/zfile.hpp"

#include <3ds.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <zlib.h>

#include "components/debug/debuglog.hpp"
#include "platform/memstat.hpp"

char* zreadAll(const char* path, size_t* sizeOut)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return nullptr;
	fseek(f, 0, SEEK_END);
	long fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* raw = (char*)malloc(fileSize + 1);
	if (!raw)
	{
		fclose(f);
		return nullptr;
	}
	size_t got = fread(raw, 1, fileSize, f);
	fclose(f);
	raw[got] = 0;
	if (got < 8 || memcmp(raw, "MWZ1", 4) != 0)
	{
		if (sizeOut)
			*sizeOut = got;
		return raw;
	}
	u32 size;
	memcpy(&size, raw + 4, 4);
	char* out = (char*)malloc(size + 1);
	uLongf outLen = size;
	int rc = out ? uncompress((Bytef*)out, &outLen, (const Bytef*)raw + 8, got - 8) : Z_MEM_ERROR;
	free(raw);
	if (rc != Z_OK || outLen != size)
	{
		logf("zfile: %s: inflate failed (%d)", path, rc);
		// (out of heap: the main loop gives memory back, as when `new` runs out; zlib's malloc never told it)
		if (rc == Z_MEM_ERROR)
			g_memoryLow = true;
		free(out);
		return nullptr;
	}
	out[size] = 0;
	if (sizeOut)
		*sizeOut = size;
	return out;
}

// fmemopen doesn't own its buffer: remember which buffer belongs to which stream
static std::map<FILE*, char*> s_buffers;
static LightLock s_lock = 1;         // unlocked (LightLock_Init sets 1)

#ifdef __3DS__
// On the 3DS a compressed file is inflated as it is read: a stream holds zlib's state and a small input buffer
// (~60 KB) instead of the whole inflated file. A 3 MB cell file inflated whole needed 3 MB in one block next to the
// compressed copy at the busiest moment (a cell arriving), which a fragmented Old 3DS heap couldn't give
// (zlib's -4 and the cell missing). Readers only read forward (fseek ahead is a skip)
namespace
{
#ifdef __LARGE64_FILES
typedef _off64_t ZOff;
#else
typedef off_t ZOff;
#endif

struct ZStream
{
	FILE* raw;
	z_stream z;
	u32 size, pos;            // inflated size, bytes handed out
	bool end;
	unsigned char in[16 * 1024];
};

volatile int s_streams = 0;

ssize_t zstreamRead(void* cookie, char* buf, size_t n)
{
	ZStream* s = (ZStream*)cookie;
	if (s->end || n == 0)
		return 0;
	s->z.next_out = (Bytef*)buf;
	s->z.avail_out = n;
	while (s->z.avail_out > 0)
	{
		if (s->z.avail_in == 0)
		{
			size_t got = fread(s->in, 1, sizeof(s->in), s->raw);
			if (got == 0)
				break;
			s->z.next_in = s->in;
			s->z.avail_in = got;
		}
		int rc = inflate(&s->z, Z_NO_FLUSH);
		if (rc == Z_STREAM_END)
		{
			s->end = true;
			break;
		}
		if (rc != Z_OK)
		{
			logf("zfile: inflate failed (%d) at %lu", rc, (unsigned long)s->pos);
			if (rc == Z_MEM_ERROR)
				g_memoryLow = true;
			s->end = true;
			break;
		}
	}
	size_t done = n - s->z.avail_out;
	s->pos += done;
	return done;
}

int zstreamSeek(void* cookie, ZOff* off, int whence)
{
	ZStream* s = (ZStream*)cookie;
	ZOff target = whence == SEEK_SET ? *off : whence == SEEK_CUR ? (ZOff)s->pos + *off : (ZOff)s->size + *off;
	if (target < (ZOff)s->pos)
		return -1;                // (never needed: the readers go forward only)
	char skip[256];
	while ((ZOff)s->pos < target)
	{
		size_t want = target - s->pos < (ZOff)sizeof(skip) ? (size_t)(target - s->pos) : sizeof(skip);
		if (zstreamRead(s, skip, want) <= 0)
			return -1;
	}
	*off = s->pos;
	return 0;
}

int zstreamClose(void* cookie)
{
	ZStream* s = (ZStream*)cookie;
	inflateEnd(&s->z);
	fclose(s->raw);
	free(s);
	LightLock_Lock(&s_lock);
	s_streams--;
	LightLock_Unlock(&s_lock);
	return 0;
}
}
#endif

FILE* zopen(const char* path)
{
#ifdef __3DS__
	{
		FILE* raw = fopen(path, "rb");
		if (!raw)
			return nullptr;
		char head[8];
		if (fread(head, 1, 8, raw) == 8 && memcmp(head, "MWZ1", 4) == 0)
		{
			ZStream* s = (ZStream*)malloc(sizeof(ZStream));
			if (!s)
			{
				fclose(raw);
				return nullptr;
			}
			memset(&s->z, 0, sizeof(s->z));
			s->raw = raw;
			memcpy(&s->size, head + 4, 4);
			s->pos = 0;
			s->end = false;
			if (inflateInit(&s->z) != Z_OK)
			{
				fclose(raw);
				free(s);
				return nullptr;
			}
			cookie_io_functions_t io = { zstreamRead, nullptr, zstreamSeek, zstreamClose };
			FILE* f = fopencookie(s, "rb", io);
			if (!f)
			{
				zstreamClose(s);
				return nullptr;
			}
			// Unbuffered: stdio's buffer would read ahead of what the caller has, and its fseek then asks this stream
			// to go back (a cookie stream gets no in-buffer seek), which it can't
			setvbuf(f, nullptr, _IONBF, 0);
			LightLock_Lock(&s_lock);
			s_streams++;
			LightLock_Unlock(&s_lock);
			return f;
		}
		// stored as it is: the file itself
		fseek(raw, 0, SEEK_SET);
		return raw;
	}
#endif

	size_t size = 0;
	char* data = zreadAll(path, &size);
	if (!data)
		return nullptr;
	FILE* f = fmemopen(data, size ? size : 1, "rb");
	if (!f)
	{
		free(data);
		return nullptr;
	}
	LightLock_Lock(&s_lock);
	s_buffers[f] = data;
	LightLock_Unlock(&s_lock);
	return f;
}

void zclose(FILE* f)
{
	if (!f)
		return;
	fclose(f);
	LightLock_Lock(&s_lock);
	auto it = s_buffers.find(f);
	if (it != s_buffers.end())
	{
		free(it->second);
		s_buffers.erase(it);
	}
	LightLock_Unlock(&s_lock);
}

// Debug: streams opened with zopen and not yet closed
int zOpenCount()
{
	LightLock_Lock(&s_lock);
	int n = (int)s_buffers.size();
#ifdef __3DS__
	n += s_streams;
#endif
	LightLock_Unlock(&s_lock);
	return n;
}
