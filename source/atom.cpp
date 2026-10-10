#include "atom.h"

#include <3ds.h>
#include <cstdlib>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
// Texts are copied into blocks of this size (a longer one gets a block of its own)
const size_t kBlock = 64 * 1024;

LightLock s_lock = 1;             // unlocked (LightLock_Init sets 1)
std::unordered_set<std::string_view>* s_set = nullptr;
char* s_block = nullptr;
size_t s_left = 0, s_bytes = 0;

const char* copyIn(const char* s, size_t n)
{
	char* out;
	if (n + 1 > kBlock / 4)
		out = (char*)malloc(n + 1);
	else
	{
		if (n + 1 > s_left)
		{
			s_block = (char*)malloc(kBlock);
			s_left = s_block ? kBlock : 0;
		}
		out = s_block;
		if (out)
		{
			s_block += n + 1;
			s_left -= n + 1;
		}
	}
	if (!out)
		return "";
	memcpy(out, s, n);
	out[n] = 0;
	s_bytes += n + 1;
	return out;
}
}

const char* Atom::intern(const char* s, size_t n)
{
	if (n == 0)
		return "";
	LightLock_Lock(&s_lock);
	if (!s_set)
		s_set = new std::unordered_set<std::string_view>(1 << 15);
	auto it = s_set->find(std::string_view(s, n));
	const char* p;
	if (it != s_set->end())
		p = it->data();
	else
	{
		p = copyIn(s, n);
		if (*p)
			s_set->insert(std::string_view(p, n));
	}
	LightLock_Unlock(&s_lock);
	return p;
}

Atom Atom::stored(const std::string& s)
{
	if (s.empty())
		return Atom();
	LightLock_Lock(&s_lock);
	const char* p = copyIn(s.data(), s.size());
	LightLock_Unlock(&s_lock);
	return Atom(p, 0);
}

size_t Atom::poolBytes()
{
	return s_bytes;
}
