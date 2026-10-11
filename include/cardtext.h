#pragma once

#include <cstdint>
#include <string>

// A long text of the game data (a dialogue line, a book, a journal entry) kept on the SD card instead of the heap.
// There are 4.8 MB of them, an eighth of an Old 3DS's heap, and only the one on screen is needed: the card file
// sdmc:/3ds/mw3ds/texts.bin holds them all, and a CardText is where its text starts there (4 bytes, as an Atom).
//
// gameLoad brackets the parse with loadBegin / loadEnd. The file starts with the hash of every text in parse order;
// when it matches, the parse writes nothing (the same data as last time), else the texts are written once again
// (rewriteBegin / rewriteEnd around a second walk over them: a new data build). Without a card the texts stay in
// the heap as before. Any thread.
class CardText
{
public:
	CardText() : off_(0) {}
	static CardText store(const std::string& s);

	bool empty() const { return off_ == 0; }
	std::string str() const;
	operator std::string() const { return str(); }
	bool operator==(const CardText& o) const { return off_ == o.off_; }
	bool operator!=(const CardText& o) const { return off_ != o.off_; }

	// gameLoad's bracket: false from loadEnd means the card file is stale (or couldn't be written) and the texts
	// must be handed to store() again, in the same order, between rewriteBegin and rewriteEnd; false from that
	// (the card failed) means once more, and they then go to the heap
	static void loadBegin();
	static bool loadEnd();
	static void rewriteBegin();
	static bool rewriteEnd();
	static size_t heapBytes();         // what is kept in the heap instead (no card file; the log at load)

private:
	uint32_t off_;
};
