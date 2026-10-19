// Skill increases and levelling up
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "components/debug/debuglog.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

int Session::levelUpTotal()
{
	return (int)w.game.gmstf("ileveluptotal", 10.0f);
}

// Progress one increase takes: (skill + 1) x fMajor / fMinor / fMiscSkillBonus, x fSpecialSkillBonus in
// the class's specialization
float Session::skillNeed(int skill)
{
	const ClassDef* c = w.playerClass();
	if (!c || skill < 0 || skill >= 27 || skill >= (int)w.game.skills.size())
		return 1e9f;
	float factor = w.game.gmstf("fmiscskillbonus", 1.25f);
	for (int i = 0; i < 5; i++)
	{
		if (c->minor[i] == skill)
			factor = w.game.gmstf("fminorskillbonus", 1.0f);
		if (c->major[i] == skill)
			factor = w.game.gmstf("fmajorskillbonus", 0.75f);
	}
	float need = (baseSkill(skill) + 1) * factor;         // OpenMW: 1 + the BASE skill (Fortify / Drain don't count)
	if (w.game.skills[skill].specialization == c->specialization)
		need *= w.game.gmstf("fspecialskillbonus", 0.8f);
	return need;
}

// One point more in a skill (use, training, a skill book): major and minor skills bring the next level
// closer, and every skill counts towards its governing attribute's multiplier at the level up
void Session::raiseSkill(int skill, bool fromBook)
{
	PlayerStats& s = w.stats;
	const ClassDef* c = w.playerClass();
	if (skill < 0 || skill >= 27 || skill >= (int)w.game.skills.size() || baseSkill(skill) >= 100 || !c)
		return;
	s.skillBonus[skill]++;
	s.skills[skill]++;
	bool major = false, minor = false;
	for (int i = 0; i < 5; i++)
	{
		major |= c->major[i] == skill;
		minor |= c->minor[i] == skill;
	}
	if (major)
		s.levelProgress += (int)w.game.gmstf("ilevelupmajormult", 1.0f);
	else if (minor)
		s.levelProgress += (int)w.game.gmstf("ilevelupminormult", 1.0f);
	int attr = w.game.skills[skill].attribute;
	if (attr >= 0 && attr < 8)
		s.attrSkillUps[attr] += (int)(major ? w.game.gmstf("ilevelupmajormultattribute", 1.0f)
			: minor ? w.game.gmstf("ilevelupminormultattribute", 1.0f) : w.game.gmstf("ilevelupmiscmultattriubte", 1.0f));

	playSound(-1, "skillraise");
	if (fromBook)
		notify(w.game.gmst("sbookskillmessage", "You have gained knowledge from this book."));
	// "Your %s skill increased to %d."
	std::string msg = w.game.gmst("snotifymessage39", "Your %s skill increased to %d.");
	size_t at = msg.find("%s");
	if (at != std::string::npos)
		msg.replace(at, 2, skillName(w, skill));
	at = msg.find("%d");
	if (at != std::string::npos)
		msg.replace(at, 2, std::to_string(s.skills[skill]));
	notify(msg);
	if ((major || minor) && s.levelProgress >= levelUpTotal())
		notify(w.game.gmst("slevelupmsg", "You should rest and meditate on what you've learned."));
	logf("skill %d -> %d (level progress %d)", skill, s.skills[skill], s.levelProgress);
}

// A skill book's first read teaches its skill
void Session::readBook(const Object* o)
{
	if (o && o->bookSkill >= 0 && w.stats.booksRead.insert(lower(o->id)).second)
		raiseSkill(o->bookSkill, true);
}

// The level up's bonus for an attribute: x1 with no skill increases under it, else iLevelUp01Mult ..
// iLevelUp10Mult (2, 2, 2, 2, 3, 3, 3, 4, 4, 5)
int Session::attrMultiplier(int attr)
{
	int n = w.stats.attrSkillUps[attr];
	if (n <= 0)
		return 1;
	n = n > 10 ? 10 : n;
	char key[24];
	snprintf(key, sizeof(key), "ilevelup%02dmult", n);
	return (int)w.game.gmstf(key, n >= 10 ? 5 : n >= 8 ? 4 : n >= 5 ? 3 : 2);
}

void Session::applyLevelUp()
{
	PlayerStats& s = w.stats;
	std::string raised;
	for (int a : levelPicks)
	{
		int m = attrMultiplier(a);
		s.attrBonus[a] += m;
		raised += " " + attrName(w, a) + " +" + std::to_string(m);
	}
	s.level++;
	s.levelProgress = std::max(0, s.levelProgress - levelUpTotal());
	for (int i = 0; i < 8; i++)
		s.attrSkillUps[i] = 0;
	float health = s.health, magicka = s.magicka, fatigue = s.fatigue;
	w.recomputeStats();
	// Health grows by a tenth of the endurance (as raised just now), at once and for good
	// (from the base Endurance: level-up raises count, Fortify and Drain do not)
	float gain = w.game.gmstf("fleveluphealthendmult", 0.1f) * std::min(100, s.attrCreation[ATTR_ENDURANCE] + s.attrBonus[ATTR_ENDURANCE]);
	s.healthBonus += gain;
	w.recomputeStats();
	s.health = fminf(s.healthMax, health + gain);
	s.magicka = fminf(s.magickaMax, magicka);
	s.fatigue = fminf(s.fatigueMax, fatigue);
	levelPicks.clear();
	logf("level up: %d,%s, health +%.1f", s.level, raised.c_str(), gain);
}
