// Topic keywords in a response's text, for the links the player can choose
#include "mwdialogue/dialoguemanagerimp.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <cstring>


// What may come before a keyword (OpenMW's word separators)
static const char kWordStarts[] = { '\n', '\r', ' ', '\t', '\'', '"', '(', '[', 0 };

std::vector<KeywordMatch> dialogueFindKeywords(const std::string& text, const std::vector<std::string>& keywords)
{
	std::string l = lower(text);
	std::vector<KeywordMatch> found, out;
	for (size_t i = 0; i < l.size(); i++)
	{
		if (i > 0 && !strchr(kWordStarts, l[i - 1]))
			continue;
		for (size_t k = 0; k < keywords.size(); k++)
		{
			const std::string& kw = keywords[k];
			if (!kw.empty() && kw[0] == l[i] && l.compare(i, kw.size(), kw) == 0)
				found.push_back({ i, i + kw.size(), (int)k });
		}
	}
	// Overlaps: the longest of the first run of overlapping matches goes out, what it overlaps goes, again
	while (!found.empty())
	{
		size_t best = 0, bestSize = 0;
		for (size_t m = 0; m < found.size(); m++)
		{
			size_t size = found[m].end - found[m].begin;
			if (size > bestSize)
			{
				bestSize = size;
				best = m;
			}
			if (m + 1 == found.size() || found[m].end <= found[m + 1].begin)
				break;
		}
		KeywordMatch kw = found[best];
		out.push_back(kw);
		std::vector<KeywordMatch> rest;
		for (auto& m : found)
			if (!(m.begin < kw.end && m.end > kw.begin))
				rest.push_back(m);
		found.swap(rest);
	}
	std::sort(out.begin(), out.end(), [](const KeywordMatch& a, const KeywordMatch& b) { return a.begin < b.begin; });
	return out;
}

// A "@name#" link's topic: pseudo asterisks (\x7f) at its end stand for asterisks in the topic's name
std::string explicitTopic(const GameData& g, const std::string& name, std::string* shown)
{
	std::string s = name;
	size_t stars = 0;
	while (!s.empty() && s.back() == '\x7f')
	{
		s.pop_back();
		stars++;
	}
	if (shown)
		*shown = s;
	return lower(g.topicStandardForm(s + std::string(stars, '*')));
}

HyperText dialogueHyperText(const GameData& g, const std::string& raw, const std::vector<std::string>& keywords,
	const std::vector<std::string>& ids)
{
	HyperText h;
	auto scan = [&](const std::string& part) {
		for (auto& m : dialogueFindKeywords(part, keywords))
			h.links.push_back({ h.text.size() + m.begin, h.text.size() + m.end, m.keyword });
		h.text += part;
	};
	size_t pos = 0;
	for (;;)
	{
		size_t at = raw.find('@', pos);
		size_t end = at == std::string::npos ? at : raw.find('#', at);
		if (end == std::string::npos)
		{
			if (pos < raw.size())
				scan(raw.substr(pos));
			break;
		}
		if (at != pos)
			scan(raw.substr(pos, at - pos));
		std::string shown;
		std::string id = explicitTopic(g, raw.substr(at + 1, end - at - 1), &shown);
		size_t begin = h.text.size();
		h.text += shown;
		for (size_t k = 0; k < ids.size(); k++)
			if (ids[k] == id)
			{
				h.links.push_back({ begin, h.text.size(), (int)k });
				break;
			}
		pos = end + 1;
	}
	return h;
}
