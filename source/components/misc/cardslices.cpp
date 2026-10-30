#include "components/misc/cardslices.hpp"

#include <cstring>

namespace
{
// The header: "MWSL", the store's format, how many slices, where they end (the table of their hashes starts there)
const u32 kHeader = 16;
const uint64_t kFnvBasis = 14695981039346656037ULL, kFnvPrime = 1099511628211ULL;

uint64_t hashOf(const void* p, u32 n)
{
	uint64_t h = (kFnvBasis ^ n) * kFnvPrime;
	const unsigned char* b = (const unsigned char*)p;
	for (u32 i = 0; i < n; i++)
		h = (h ^ b[i]) * kFnvPrime;
	return h;
}
}

u32 CardSlices::check(const void* data, u32 n)
{
	return (u32)hashOf(data, n);
}

bool CardSlices::begin()
{
	at_ = kHeader;
	written_ = count_ = oldCount_ = oldEnd_ = 0;
	failed_ = false;
	hashes_.clear();
	// A whole file of this format: the load's slices are checked against its table as they come
	if ((file_ = fopen(path_, "rb")))
	{
		setvbuf(file_, buf_, _IOFBF, sizeof(buf_));
		char magic[4];
		u32 head[3];
		if (fread(magic, 1, 4, file_) == 4 && memcmp(magic, "MWSL", 4) == 0 && fread(head, 4, 3, file_) == 3 &&
			head[0] == version_ && head[2] >= kHeader && fseek(file_, head[2], SEEK_SET) == 0)
		{
			oldCount_ = head[1];
			oldEnd_ = head[2];
			hashes_.reserve(oldCount_ + 64);
			mode_ = CHECK;
			return true;
		}
		fclose(file_);
		file_ = nullptr;
	}
	hashes_.reserve(4096);
	return startWriting();
}

// From the first slice that differs on: the header cleared first, then that slice and every one after it written
bool CardSlices::startWriting()
{
	if (file_)
		fclose(file_);
	file_ = fopen(path_, count_ == 0 ? "wb" : "r+b");
	if (file_)
	{
		setvbuf(file_, buf_, _IOFBF, sizeof(buf_));
		char zero[kHeader] = {};
		if (fwrite(zero, 1, kHeader, file_) == kHeader && fflush(file_) == 0 && fseek(file_, at_, SEEK_SET) == 0)
		{
			mode_ = WRITE;
			return true;
		}
	}
	failed_ = true;
	mode_ = NONE;
	return false;
}

bool CardSlices::put(const void* data, u32 n, u32& off, u32& check)
{
	if (mode_ == NONE)
		return false;
	uint64_t h = hashOf(data, n);
	if (mode_ == CHECK)
	{
		uint64_t was = 0;
		bool same = count_ < oldCount_ && fread(&was, 8, 1, file_) == 1 && was == h;
		if (!same && !startWriting())
			return false;
	}
	if (mode_ == WRITE && fwrite(data, 1, n, file_) != n)
	{
		failed_ = true;                  // (this one and the rest stay in the heap)
		mode_ = NONE;
		return false;
	}
	if (mode_ == WRITE)
		written_ += n;
	off = at_;
	check = (u32)h;
	at_ += n;
	hashes_.push_back(h);
	count_++;
	return true;
}

bool CardSlices::end()
{
	// (the same slices as the file's but fewer of them: only the table and the header change)
	if (mode_ == CHECK && (count_ != oldCount_ || at_ != oldEnd_))
		startWriting();
	bool ok = !failed_;
	if (mode_ == WRITE)
	{
		u32 head[3] = { version_, count_, at_ };
		ok = ok && fwrite(hashes_.data(), 8, count_, file_) == count_ && fflush(file_) == 0 &&
			fseek(file_, 0, SEEK_SET) == 0 && fwrite("MWSL", 1, 4, file_) == 4 && fwrite(head, 4, 3, file_) == 3;
	}
	if (file_ && fclose(file_) != 0 && mode_ == WRITE)
		ok = false;
	file_ = nullptr;
	mode_ = NONE;
	std::vector<uint64_t>().swap(hashes_);
	return ok;
}

void CardSlices::damaged()
{
	if (FILE* f = fopen(path_, "r+b"))
	{
		fwrite("\0\0\0\0", 1, 4, f);
		fclose(f);
	}
}
