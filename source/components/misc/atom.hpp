#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// A string of the game data kept once, for the whole run, in big shared blocks: 4 bytes where a std::string is 24,
// and no allocation of its own. The game data has hundreds of thousands of short texts (who may say a line, the
// condition names, script names, the item and actor fields): as std::strings they were a third of an Old 3DS's
// heap. Atom(text) interns (one copy of each distinct text: ids, names, the same values over and over), and
// Atom::stored(text) only copies (dialogue lines and book texts, each one different).
// Never freed: the data is loaded once (a save loaded again interns the same texts and finds them). Any thread.
class Atom
{
public:
	Atom() : p_("") {}
	Atom(const std::string& s) : p_(intern(s.data(), s.size())) {}
	Atom(const char* s) : p_(intern(s, strlen(s))) {}
	static Atom stored(const std::string& s);
	// The pooled copy of a text, or nullptr when no Atom holds it (a lookup that adds nothing to the pool)
	static const char* existing(const char* s, size_t n);
	Atom& operator=(const std::string& s) { p_ = intern(s.data(), s.size()); return *this; }
	Atom& operator=(const char* s) { p_ = intern(s, strlen(s)); return *this; }

	void clear() { p_ = ""; }
	const char* c_str() const { return p_; }
	bool empty() const { return !*p_; }
	size_t size() const { return strlen(p_); }
	size_t length() const { return strlen(p_); }
	char operator[](size_t i) const { return p_[i]; }
	std::string str() const { return p_; }
	std::string_view view() const { return p_; }
	// (the std::string calls the code uses on these)
	int compare(const char* s) const { return strcmp(p_, s); }
	int compare(size_t pos, size_t n, const char* s) const { return view().compare(pos, n, s); }
	int compare(size_t pos, size_t n, const std::string& s) const { return view().compare(pos, n, s); }
	size_t find(const char* s, size_t pos = 0) const { return view().find(s, pos); }
	size_t find(char c, size_t pos = 0) const { return view().find(c, pos); }
	size_t rfind(const char* s, size_t pos = std::string::npos) const { return view().rfind(s, pos); }
	size_t rfind(char c, size_t pos = std::string::npos) const { return view().rfind(c, pos); }
	std::string substr(size_t pos, size_t n = std::string::npos) const { return std::string(view().substr(pos, n)); }
	operator std::string() const { return p_; }

	bool operator==(const Atom& o) const { return p_ == o.p_ || strcmp(p_, o.p_) == 0; }
	bool operator!=(const Atom& o) const { return !(*this == o); }
	bool operator==(const std::string& s) const { return s.compare(p_) == 0; }
	bool operator!=(const std::string& s) const { return s.compare(p_) != 0; }
	bool operator==(const char* s) const { return strcmp(p_, s) == 0; }
	bool operator!=(const char* s) const { return strcmp(p_, s) != 0; }
	bool operator<(const Atom& o) const { return strcmp(p_, o.p_) < 0; }

	// The interned Atom of a text, without adding it: false when no Atom holds it (a map keyed by Atoms has no such key)
	static bool lookup(const std::string& s, Atom& out)
	{
		const char* p = existing(s.data(), s.size());
		if (p)
			out = Atom(p, 0);
		return p != nullptr;
	}
	// An Atom again from its c_str(), within the same run (the pool is never freed): a script's tree kept on the card
	// (mwscript/scriptstore.cpp) writes its texts as these
	static Atom pooled(const char* p) { return Atom(p, 0); }
	static size_t poolBytes();      // what the blocks hold (the log at load)
	// Takes the next block early, at a quiet moment, so it isn't born mid-parse between transient
	// allocations (each block is pinned for the run). Session's eviction timer calls it
	static void prefill();

private:
	explicit Atom(const char* p, int) : p_(p) {}
	static const char* intern(const char* s, size_t n);
	const char* p_;
};

// A map keyed by interned Atoms (Atom(text), never Atom::stored): one pointer per text, so the pointer is the key. 4 bytes
// where a std::string key is 24, and its own block for an id past 15 letters (most object ids)
struct AtomKeyHash { size_t operator()(const Atom& a) const noexcept { return (size_t)(uintptr_t)a.c_str() >> 2; } };
struct AtomKeyEq { bool operator()(const Atom& x, const Atom& y) const noexcept { return x.c_str() == y.c_str(); } };

inline bool operator==(const std::string& s, const Atom& a) { return a == s; }
inline bool operator!=(const std::string& s, const Atom& a) { return a != s; }
inline bool operator==(const char* s, const Atom& a) { return a == s; }
inline bool operator!=(const char* s, const Atom& a) { return a != s; }
inline std::string operator+(const std::string& s, const Atom& a) { return s + a.c_str(); }
inline std::string operator+(const Atom& a, const std::string& s) { return a.c_str() + s; }
inline std::string operator+(const char* s, const Atom& a) { return std::string(s) + a.c_str(); }
inline std::string operator+(const Atom& a, const char* s) { return std::string(a.c_str()) + s; }
