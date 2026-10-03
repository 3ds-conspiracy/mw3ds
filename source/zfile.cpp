#include "zfile.h"

#include <3ds.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <zlib.h>

#include "log.h"

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

FILE* zopen(const char* path)
{
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

// Debug: streams opened with zopen and not yet closed (each holds its whole inflated file)
int zOpenCount()
{
	LightLock_Lock(&s_lock);
	int n = (int)s_buffers.size();
	LightLock_Unlock(&s_lock);
	return n;
}
