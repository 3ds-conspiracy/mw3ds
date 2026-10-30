#pragma once

#include <3ds.h>
#include <cstdint>
#include <cstdio>
#include <vector>

// A card file of slices the load puts out of the heap (dialogue.bin: each topic's responses, scripts.bin: each script's
// tree), kept from one start to the next. The file ends with a hash of each slice; the load hashes each slice it makes
// and, while they match the file's, writes nothing. The first one that differs (a new data build, a new program that
// writes them differently) and every one after it are written again. The header is written last and cleared before
// the first write: a write cut short leaves a file the next start writes again whole. Nothing of a save points into it.
// The main thread only, between begin and end.
class CardSlices
{
public:
	CardSlices(const char* path, u32 version) : path_(path), version_(version) {}
	// false when the card can't take the slices (the caller keeps them in the heap)
	bool begin();
	// Where the slice is in the file, and what it is checked against when read back; false when the card can't take
	// it (the caller keeps this one and the rest in the heap)
	bool put(const void* data, u32 n, u32& off, u32& check);
	// false when the file wasn't written whole
	bool end();
	const char* path() const { return path_; }
	u32 written() const { return written_; }       // bytes written this load (0: the card's copy was the same)
	u32 size() const { return at_; }
	// What a slice read back is checked against (the low half of its hash)
	static u32 check(const void* data, u32 n);
	// A slice read back didn't match: the next start writes the file again
	void damaged();

private:
	enum Mode { NONE, CHECK, WRITE };
	bool startWriting();
	const char* path_;
	u32 version_;
	Mode mode_ = NONE;
	bool failed_ = false;
	FILE* file_ = nullptr;
	u32 at_ = 0, written_ = 0, count_ = 0, oldCount_ = 0, oldEnd_ = 0;
	std::vector<uint64_t> hashes_;    // each slice's, for the table at the end
	char buf_[16 * 1024];             // (not the FILE's own: that would come out of the heap)
};
