#include "components/misc/cardtext.hpp"

#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "components/misc/atom.hpp"
#include "components/debug/debuglog.hpp"

namespace
{
const char* const kPath = "sdmc:/3ds/mw3ds/texts.bin";
// The header: "MWTX", the format, where the texts end, the 64-bit hash of all of them. Each text after it is its
// length (4 bytes) and its bytes; a CardText is where that length starts (never 0: the header is there)
const uint32_t kVersion = 1;
const uint32_t kHeader = 24;

enum Mode { HEAP, CHECK, WRITE };

LightLock s_lock = 1;               // unlocked (LightLock_Init sets 1)
Mode s_mode = HEAP;
bool s_loading = false, s_writeFailed = false;
uint32_t s_next = kHeader, s_fileEnd = 0;
uint64_t s_hash = 0;
uint32_t s_parseNext = 0;
uint64_t s_parseHash = 0;
FILE* s_write = nullptr;
FILE* s_read = nullptr;
// (static buffers: a FILE's own would come out of the heap these texts are kept out of)
char s_writeBuf[16 * 1024];
char s_readBuf[2 * 1024];
// The texts kept in the heap (no card file, or made after the load): by where they would start in the file
std::vector<std::pair<uint32_t, const char*>> s_heap;
size_t s_heapBytes = 0;
// The last few read (an open book is drawn every frame; a line is substituted, then searched for topics)
struct Recent { uint32_t off = 0; std::string text; };
Recent s_recent[4];
int s_recentNext = 0;

const uint64_t kFnvBasis = 14695981039346656037ULL, kFnvPrime = 1099511628211ULL;

void hashBytes(const void* p, size_t n)
{
	const unsigned char* b = (const unsigned char*)p;
	for (size_t i = 0; i < n; i++)
		s_hash = (s_hash ^ b[i]) * kFnvPrime;
}

void startWalk()
{
	s_next = kHeader;
	s_hash = kFnvBasis;
	s_heap.clear();
	s_heapBytes = 0;
	for (Recent& r : s_recent)
		r = Recent();
	s_writeFailed = false;
	s_loading = true;
}

// Opens the file for writing (the header comes last: a write cut short leaves a file that doesn't match)
bool openWrite()
{
	s_write = fopen(kPath, "wb");
	if (!s_write)
		return false;
	setvbuf(s_write, s_writeBuf, _IOFBF, sizeof(s_writeBuf));
	char zero[kHeader] = {};
	if (fwrite(zero, 1, kHeader, s_write) != kHeader)
		s_writeFailed = true;
	return true;
}

// Writes the header and opens the file for reading; false if any write failed
bool finishWrite()
{
	bool ok = !s_writeFailed;
	if (ok)
	{
		uint32_t head[2] = { kVersion, s_next };
		ok = fseek(s_write, 0, SEEK_SET) == 0 && fwrite("MWTX", 1, 4, s_write) == 4 && fwrite(head, 4, 2, s_write) == 2 &&
			fwrite(&s_hash, 8, 1, s_write) == 1;
	}
	ok = fclose(s_write) == 0 && ok;
	s_write = nullptr;
	return ok;
}

bool openRead()
{
	s_read = fopen(kPath, "rb");
	if (!s_read)
		return false;
	setvbuf(s_read, s_readBuf, _IOFBF, sizeof(s_readBuf));
	char magic[4];
	uint32_t head[2];
	uint64_t hash;
	bool ok = fread(magic, 1, 4, s_read) == 4 && memcmp(magic, "MWTX", 4) == 0 && fread(head, 4, 2, s_read) == 2 &&
		fread(&hash, 8, 1, s_read) == 1 && head[0] == kVersion && head[1] == s_next && hash == s_hash;
	if (!ok)
	{
		fclose(s_read);
		s_read = nullptr;
		return false;
	}
	s_fileEnd = s_next;
	return true;
}
}

CardText CardText::store(const std::string& s)
{
	CardText t;
	if (s.empty())
		return t;
	uint32_t n = (uint32_t)s.size();
	LightLock_Lock(&s_lock);
	t.off_ = s_next;
	s_next += 4 + n;
	if (s_loading)
	{
		hashBytes(&n, 4);
		hashBytes(s.data(), n);
	}
	if (s_loading && s_mode == WRITE)
	{
		if (fwrite(&n, 4, 1, s_write) != 1 || fwrite(s.data(), 1, n, s_write) != n)
			s_writeFailed = true;
	}
	else if (!s_loading || s_mode == HEAP)
	{
		s_heap.push_back({ t.off_, Atom::stored(s).c_str() });
		s_heapBytes += n + 1;
	}
	LightLock_Unlock(&s_lock);
	return t;
}

std::string CardText::str() const
{
	if (!off_)
		return std::string();
	std::string out;
	LightLock_Lock(&s_lock);
	for (const Recent& r : s_recent)
		if (r.off == off_)
		{
			out = r.text;
			LightLock_Unlock(&s_lock);
			return out;
		}
	if (s_read && off_ < s_fileEnd)
	{
		uint32_t n = 0;
		bool ok = fseek(s_read, off_, SEEK_SET) == 0 && fread(&n, 4, 1, s_read) == 1 && n <= s_fileEnd - off_;
		if (ok)
		{
			out.resize(n);
			ok = fread(&out[0], 1, n, s_read) == n;
		}
		if (!ok)
		{
			out.clear();
			logf("texts: cannot read the text at %lu of %s", (unsigned long)off_, kPath);
		}
		else
		{
			s_recent[s_recentNext].off = off_;
			s_recent[s_recentNext].text = out;
			s_recentNext = (s_recentNext + 1) % 4;
		}
	}
	else
	{
		auto it = std::lower_bound(s_heap.begin(), s_heap.end(), off_,
			[](const std::pair<uint32_t, const char*>& e, uint32_t off) { return e.first < off; });
		if (it != s_heap.end() && it->first == off_)
			out = it->second;
	}
	LightLock_Unlock(&s_lock);
	return out;
}

void CardText::loadBegin()
{
	LightLock_Lock(&s_lock);
	if (s_read)
		fclose(s_read);
	s_read = nullptr;
	s_fileEnd = 0;
	startWalk();
	// A file there: the parse checks it (it is written again only if the data changed). None: written as the parse goes
	if (FILE* f = fopen(kPath, "rb"))
	{
		fclose(f);
		s_mode = CHECK;
	}
	else
		s_mode = openWrite() ? WRITE : HEAP;
	LightLock_Unlock(&s_lock);
}

bool CardText::loadEnd()
{
	LightLock_Lock(&s_lock);
	s_loading = false;
	s_parseNext = s_next;
	s_parseHash = s_hash;
	bool ok = true;
	if (s_mode == WRITE)
		ok = finishWrite() && openRead();
	else if (s_mode == CHECK)
		ok = openRead();
	LightLock_Unlock(&s_lock);
	if (s_mode == HEAP)
		logf("texts: no card file (%s), the texts stay in the heap", kPath);
	else if (ok)
		logf("texts: %lu KB on the card (%s)%s", (unsigned long)(s_next / 1024), kPath, s_mode == WRITE ? ", written" : "");
	else
		logf("texts: %s doesn't match the data, writing it again", kPath);
	return ok;
}

void CardText::rewriteBegin()
{
	LightLock_Lock(&s_lock);
	bool failedBefore = s_mode == WRITE;      // (a write that failed: the heap this time)
	startWalk();
	s_mode = !failedBefore && openWrite() ? WRITE : HEAP;
	LightLock_Unlock(&s_lock);
}

bool CardText::rewriteEnd()
{
	LightLock_Lock(&s_lock);
	s_loading = false;
	bool same = s_next == s_parseNext && s_hash == s_parseHash;
	bool ok = true;
	if (s_mode == WRITE)
		ok = finishWrite() && openRead();
	LightLock_Unlock(&s_lock);
	if (!same)
		logf("texts: the second walk didn't find the parse's texts (%lu / %lu bytes): dialogue and books will be wrong",
			(unsigned long)s_next, (unsigned long)s_parseNext);
	else if (s_mode == WRITE && ok)
		logf("texts: %lu KB written to the card (%s)", (unsigned long)(s_next / 1024), kPath);
	else if (s_mode == WRITE)
		logf("texts: cannot write %s", kPath);
	else
		logf("texts: the texts stay in the heap (%u KB)", (unsigned)(s_heapBytes / 1024));
	return ok;
}

size_t CardText::heapBytes()
{
	return s_heapBytes;
}
