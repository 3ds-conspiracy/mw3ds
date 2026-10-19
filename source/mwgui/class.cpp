// Character creation: the class menus (pick, the quiz, make one's own)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include "mwgui/statshelpers.hpp"

void Session::drawClassMethod()
{
	header(w.game.gmstLabel("sclass", "Class"));
	uiPanel(4, 28, 312, 60);
	uiTextBox(scroll, 6, 34, 308, 50, "Socucius Ergalla needs to record your class. How would you like to do this?", 0.5f, false);
	const UiInput& in = uiIn();
	if (in.down & KEY_DOWN) focus = (focus + 1) % 3;
	if (in.down & KEY_UP) focus = (focus + 2) % 3;
	if (uiButton(20, 98, 280, 38, w.game.gmst("sclasschoicemenu1", "Answer his questions"), focus == 0))
	{
		openScreen(SCR_CLASS_QUIZ);
		quizIndex = 0;
		quizScroll = UiScroll();
		quizCounts[0] = quizCounts[1] = quizCounts[2] = 0;
		if (!w.game.quiz.empty())
			say(-1, w.game.quiz[0].sound, "");
	}
	else if (uiButton(20, 144, 280, 38, w.game.gmst("sclasschoicemenu2", "Pick from the class list"), focus == 1))
		openScreen(SCR_CLASS_LIST);
	else if (uiButton(20, 190, 280, 38, w.game.gmst("sclasschoicemenu3", "Create a custom class"), focus == 2))
	{
		openScreen(SCR_CLASS_CREATE);
		classPicking = -1;
	}
}

static std::string classText(World& w, const ClassDef& c)
{
	std::string t = w.game.gmstLabel("sspecialization", "Specialization") + ": " + specName(w, c.specialization) + "\nFavored: " +
		attrName(w, c.attributes[0]) + ", " + attrName(w, c.attributes[1]) + "\n\nMajor skills:\n";
	for (int i = 0; i < 5; i++) t += "  " + skillName(w, c.major[i]) + "\n";
	t += "Minor skills:\n";
	for (int i = 0; i < 5; i++) t += "  " + skillName(w, c.minor[i]) + "\n";
	return t + "\n" + c.desc;
}

// The class's picture (Morrowind's level-up art for it) in a box on the top screen; title: over it, the class's name under it
static void classModal(Session& s, const ClassDef& c, const std::string& title = "")
{
	auto art = s.w.game.art.levelup.find(lower(c.id));
	if (art == s.w.game.art.levelup.end() || art->second.h <= 0)
		return;
	s.topModal.kind = Session::TopModal::ART;
	s.topModal.art = art->second;
	s.topModal.title = title.empty() ? c.name : title;
	s.topModal.caption = title.empty() ? "" : c.name;
}

void Session::drawClassList()
{
	header("Choose your class");
	std::vector<const ClassDef*> classes;
	std::vector<std::string> rows;
	for (auto& c : w.game.classes)
		if (c.playable)
		{
			classes.push_back(&c);
			rows.push_back(c.name);
		}
	int pick = uiList(list, 4, 24, 116, 212, rows);
	const ClassDef* c = classes[list.selected < (int)classes.size() ? list.selected : 0];
	classModal(*this, *c);
	uiPanel(124, 24, 192, 168);
	uiTextBox(scroll, 126, 26, 188, 164, classText(w, *c), 0.45f, false);
	if (uiButton(124, 196, 192, 36, w.game.gmst("sok", "OK"), true) || (pick >= 0 && (uiIn().down & KEY_A)))
	{
		w.stats.cls = c->id;
		w.recomputeStats();
		logf("chargen: class %s", c->id.c_str());
		closeScreen();
	}
}

// Class from the question answers (combat / magic / stealth counts), as the original engine picks it
static std::string generatedClass(const int* n)
{
	int combat = n[0], magic = n[1], stealth = n[2];
	if (combat > 7) return "Warrior";
	if (magic > 7) return "Mage";
	if (stealth > 7) return "Thief";
	switch (combat)
	{
	case 4: return "Rogue";
	case 5: return stealth == 3 ? "Scout" : "Archer";
	case 6: return stealth == 1 ? "Barbarian" : stealth == 3 ? "Crusader" : "Knight";
	case 7: return "Warrior";
	}
	switch (magic)
	{
	case 4: return "Spellsword";
	case 5: return "Witchhunter";
	case 6: return combat == 2 ? "Sorcerer" : combat == 3 ? "Healer" : "Battlemage";
	case 7: return "Mage";
	}
	switch (stealth)
	{
	case 3: return magic == 3 ? "Bard" : "Warrior";
	case 5: return magic == 3 ? "Monk" : "Pilgrim";
	case 6: return magic == 1 ? "Agent" : magic == 3 ? "Assassin" : "Acrobat";
	case 7: return "Thief";
	}
	return "Warrior";
}

void Session::drawClassQuiz()
{
	if (quizIndex >= (int)w.game.quiz.size())
	{
		pickedClass = generatedClass(quizCounts);
		openScreen(SCR_CLASS_RESULT);
		focus = 1;    // OK
		return;
	}
	const QuizQuestion& q = w.game.quiz[quizIndex];
	header("Question " + std::to_string(quizIndex + 1) + " of " + std::to_string(w.game.quiz.size()));
	// The question and its three answers; most are taller than the screen, so the page scrolls (drag it, or the
	// circle pad), and the D-pad's choice is scrolled into view
	const float top = 22.0f, bottom = 240.0f;
	std::vector<std::string> ql = uiWrap(q.question, 300, 0.45f);
	float lh = uiLineHeight(0.45f), lha = uiLineHeight(0.42f);
	std::vector<std::string> al[3];
	float ay[3], ah[3];
	float content = 4 + ql.size() * lh + 4;
	for (int a = 0; a < 3; a++)
	{
		al[a] = uiWrap(q.answers[a], 288, 0.42f);
		ay[a] = content;
		ah[a] = al[a].size() * lha + 6;
		content += ah[a] + 4;
	}
	const UiInput& in = uiIn();
	bool moved = in.down & (KEY_DOWN | KEY_UP);
	if (in.down & KEY_DOWN) focus = (focus + 1) % 3;
	if (in.down & KEY_UP) focus = (focus + 2) % 3;
	float& sc = quizScroll.scroll;
	if (in.touching && in.touchY >= top)
		sc -= in.dragDY;
	sc += uiStickScroll();
	if (moved)
		sc = fminf(fmaxf(sc, ay[focus] + ah[focus] - (bottom - top) + 2), ay[focus] - 2);
	sc = fmaxf(0.0f, fminf(sc, fmaxf(0.0f, content - (bottom - top))));
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - bottom), 0, (u32)(240 - top), 320);
	float y0 = top - sc;
	for (size_t i = 0; i < ql.size(); i++)
		uiText(10, y0 + 4 + i * lh, 0.45f, col::header, ql[i]);
	int answer = -1;
	for (int a = 0; a < 3; a++)
	{
		float y = y0 + ay[a], h = ah[a];
		uiRect(4, y, 312, h, a == focus ? col::border : col::textDim);
		uiRect(5, y + 1, 310, h - 2, a == focus ? col::select : col::panelLight);
		for (size_t i = 0; i < al[a].size(); i++)
			uiText(12, y + 3 + i * lha, 0.42f, col::text, al[a][i]);
		if (uiHit(4, fmaxf(y, top), 312, fminf(y + h, bottom) - fmaxf(y, top)))
			answer = a;
		if (a == focus)
			quizBottom = y + h;
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	if (in.down & KEY_A)
		answer = focus;
	if (answer >= 0)
	{
		quizCounts[answer]++;
		quizIndex++;
		focus = 0;
		quizScroll = UiScroll();
		playSound(-1, "Menu Click");
		if (quizIndex < (int)w.game.quiz.size())
			say(-1, w.game.quiz[quizIndex].sound, "");
	}
}

void Session::drawClassResult()
{
	const ClassDef* c = nullptr;
	for (auto& k : w.game.classes)
		if (lower(k.id) == lower(pickedClass))
			c = &k;
	header("Your class");
	if (!c)
	{
		openScreen(SCR_CLASS_LIST);
		return;
	}
	classModal(*this, *c, "Your personality and past reflect a:");
	uiPanel(4, 24, 312, 178);
	uiTextBox(scroll, 6, 26, 308, 174, "Based on your answers, you are a " + c->name + ".\n\n" + classText(w, *c), 0.45f, false);
	int b = buttonRow({ w.game.gmst("sback", "Back"), w.game.gmst("sok", "OK") }, focus);
	if (b == 1)
	{
		w.stats.cls = c->id;
		w.recomputeStats();
		logf("chargen: class %s (questions)", c->id.c_str());
		closeScreen();
	}
	else if (b == 0)
		openScreen(SCR_CLASS_METHOD);
}

// Create Custom Class (OpenMW's CreateClassDialog): a name, a specialization, two favored attributes, five major and
// five minor skills, and a description. Each field opens a list of its choices over the form; choosing a skill or an
// attribute that another field holds swaps the two, so no skill or attribute is ever taken twice
enum { CF_NAME, CF_SPEC, CF_ATTR0, CF_ATTR1, CF_MAJOR0, CF_MINOR0 = CF_MAJOR0 + 5, CF_DESC = CF_MINOR0 + 5, CF_BACK, CF_OK };

// The skill list: three columns of nine rows, scrolling between the column titles and the Cancel / OK row
static const float kSkillPickTop = 56.0f, kSkillPickBottom = 202.0f, kSkillPickRowH = 26.0f, kSkillPickStep = 29.0f;

// The form's fields as the D-pad walks them: rows top to bottom, -1 past a row's end
static const int kClassFormRows[9][3] = {
	{ CF_NAME, -1, -1 }, { CF_SPEC, -1, -1 }, { CF_ATTR0, CF_ATTR1, -1 },
	{ CF_MAJOR0, CF_MINOR0, -1 }, { CF_MAJOR0 + 1, CF_MINOR0 + 1, -1 }, { CF_MAJOR0 + 2, CF_MINOR0 + 2, -1 },
	{ CF_MAJOR0 + 3, CF_MINOR0 + 3, -1 }, { CF_MAJOR0 + 4, CF_MINOR0 + 4, -1 }, { CF_DESC, CF_BACK, CF_OK },
};

// What the form starts from (OpenMW's): Combat; Strength and Agility; Block, Armorer, Medium Armor, Heavy Armor and
// Blunt Weapon major; Long Blade, Axe, Spear, Athletics and Enchant minor; named sCustomClassName ("Adventurer")
static ClassDef defaultCustomClass(World& w)
{
	ClassDef c = ClassDef();
	c.id = kCustomClassId;
	c.name = w.game.gmstLabel("scustomclassname", "Adventurer");
	c.specialization = 0;
	c.attributes[0] = ATTR_STRENGTH;
	c.attributes[1] = ATTR_AGILITY;
	for (int i = 0; i < 5; i++)
	{
		c.major[i] = i;
		c.minor[i] = 5 + i;
	}
	c.playable = true;
	return c;
}

// The skill a skill field holds (CF_MAJOR0 .. CF_MINOR0 + 4)
static int& classSkill(ClassDef& c, int field)
{
	return field < CF_MINOR0 ? c.major[field - CF_MAJOR0] : c.minor[field - CF_MINOR0];
}

void Session::drawClassCreate()
{
	if (!classDraftReady)
	{
		classDraft = !w.stats.customClass.id.empty() ? w.stats.customClass : defaultCustomClass(w);
		classDraftReady = true;
	}
	ClassDef& c = classDraft;
	const UiInput& in = uiIn();
	header("Create your class");

	// ---- a field's choices, over the form: tap one (or move to it with the D-pad), then OK or A; B or Cancel leaves
	// the field as it was. The skills are taller than their area and scroll: drag, the circle pad, or the D-pad's
	// choice kept in view
	if (classPicking >= 0)
	{
		// the choices in columns (the skills by specialization, as OpenMW's skill list), and the one the field holds
		std::vector<std::vector<int>> cols;
		std::vector<std::string> colTitle;
		std::string title, help;
		int now = -1;
		if (classPicking == CF_SPEC)
		{
			title = w.game.gmstLabel("sspecializationmenu1", "Choose a Specialization");
			help = w.game.gmst("screateclassmenuhelp1", "");
			cols = { { 0, 1, 2 } };
			now = c.specialization;
		}
		else if (classPicking == CF_ATTR0 || classPicking == CF_ATTR1)
		{
			title = w.game.gmstLabel("sattributesmenu1", "Choose an Attribute");
			help = w.game.gmst("screateclassmenuhelp2", "");
			cols = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 } };
			now = c.attributes[classPicking - CF_ATTR0];
		}
		else
		{
			title = w.game.gmstLabel("sskillsmenu1", "Choose a Skill");
			cols.resize(3);
			for (int k = 0; k < 27 && k < (int)w.game.skills.size(); k++)
				if (w.game.skills[k].specialization >= 0 && w.game.skills[k].specialization < 3)
					cols[w.game.skills[k].specialization].push_back(k);
			for (int s = 0; s < 3; s++)
				colTitle.push_back(specName(w, s));
			now = classSkill(c, classPicking);
		}
		auto label = [&](int v) {
			return classPicking == CF_SPEC ? specName(w, v) : classPicking <= CF_ATTR1 ? attrName(w, v) : skillName(w, v);
		};
		// the D-pad: up / down in a column, left / right across them (kept to the row the column has)
		int nc = (int)cols.size(), cc = std::min(classPickCursor / 16, nc - 1), cr = classPickCursor % 16;
		bool moved = in.down & (KEY_DOWN | KEY_UP | KEY_LEFT | KEY_RIGHT);
		if (in.down & KEY_DOWN) cr++;
		if (in.down & KEY_UP) cr--;
		if (in.down & KEY_RIGHT) cc = (cc + 1) % nc;
		if (in.down & KEY_LEFT) cc = (cc + nc - 1) % nc;
		cr = std::max(0, std::min(cr, (int)cols[cc].size() - 1));

		uiTextCentered(160, 24, 0.5f, col::header, title);
		bool skills = !colTitle.empty();
		float colW = (312.0f - (nc - 1) * 4.0f) / nc;
		float rowH = skills ? kSkillPickRowH : 24.0f, step = skills ? kSkillPickStep : 28.0f;
		float top = skills ? kSkillPickTop : 44.0f, bottom = skills ? kSkillPickBottom : 204.0f;
		size_t rows = 0;
		for (auto& cv : cols)
			rows = std::max(rows, cv.size());
		// the skills' scroll: dragged, the circle pad, the D-pad's choice brought into view; held to the list
		float off = 0.0f;
		if (skills)
		{
			float& sc = scroll.scroll;
			float view = bottom - top, content = rows * step - (step - rowH);
			if (in.touching && in.touchY >= top && in.touchY < bottom)
				sc -= in.dragDY;
			sc += uiStickScroll();
			if (moved)
				sc = fminf(fmaxf(sc, cr * step + rowH - view), cr * step);
			sc = fmaxf(0.0f, fminf(sc, fmaxf(0.0f, content - view)));
			off = sc;
			// where in the list the area is: a thin bar at the right edge
			if (content > view)
			{
				uiRect(317, top, 2, view, col::panelLight);
				uiRect(317, top + (view - view * view / content) * sc / (content - view), 2, view * view / content, col::header);
			}
		}
		// one size of text for every choice, the largest the longest fits (uiButton would shrink each on its own)
		float ts = 0.5f;
		for (auto& cv : cols)
			for (int v : cv)
				while (ts > 0.33f && (uiTextWidth(label(v) + " *", ts) > colW - 6 || uiLineHeight(ts) > rowH + 2))
					ts -= 0.02f;
		if (skills)
		{
			for (int ci = 0; ci < nc; ci++)
				uiTextCentered(4 + ci * (colW + 4) + colW / 2, 40, 0.42f, col::header, colTitle[ci]);
			C2D_Flush();
			C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - bottom), 0, (u32)(240 - top), 320);
		}
		// a tap only picks out a choice (lit); OK takes it. (A tap on a row scrolled out of the area picks nothing)
		bool tapIn = in.tapped && in.tapY >= top && in.tapY < bottom;
		for (int ci = 0; ci < nc; ci++)
		{
			float x = 4 + ci * (colW + 4);
			for (int r = 0; r < (int)cols[ci].size(); r++)
			{
				int v = cols[ci][r];
				float y = top + r * step - off;
				if (y + rowH < top || y > bottom)
					continue;
				bool lit = ci == cc && r == cr;
				if (uiButton(x, y, colW, rowH, "", lit) && tapIn)
				{
					playSound(-1, "Menu Click");
					cc = ci, cr = r;
				}
				// (the one the field holds now is marked)
				uiTextCentered(x + colW / 2, y + (rowH - uiLineHeight(ts)) / 2, ts, lit ? col::textOver : col::text,
					label(v) + (v == now ? " *" : ""));
			}
		}
		if (skills)
		{
			C2D_Flush();
			C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
		}
		classPickCursor = cc * 16 + cr;
		if (!help.empty())
		{
			float hy = top + rows * step + 4;
			uiTextBox(scroll, 6, hy, 308, 204 - hy, help, 0.42f, false);
		}
		bool cancel = uiButton(4, 208, 154, 28, w.game.gmst("scancel", "Cancel"), false) || (in.down & KEY_B);
		bool ok = uiButton(162, 208, 154, 28, w.game.gmst("sok", "OK"), false) || (in.down & KEY_A);
		int chosen = ok ? cols[cc][cr] : -1;
		if (chosen >= 0)
		{
			playSound(-1, "Menu Click");
			if (classPicking == CF_SPEC)
				c.specialization = chosen;
			else if (classPicking == CF_ATTR0 || classPicking == CF_ATTR1)
			{
				int& mine = c.attributes[classPicking - CF_ATTR0];
				int& other = c.attributes[CF_ATTR1 - classPicking];
				if (other == chosen)
					other = mine;
				mine = chosen;
			}
			else
			{
				int& mine = classSkill(c, classPicking);
				for (int f = CF_MAJOR0; f < CF_DESC; f++)
					if (f != classPicking && classSkill(c, f) == chosen)
					{
						classSkill(c, f) = mine;
						break;
					}
				mine = chosen;
			}
			logf("chargen: class field %d = %s", classPicking, label(chosen).c_str());
			classPicking = -1;
		}
		else if (cancel)
		{
			playSound(-1, "Menu Click");
			classPicking = -1;
		}
		return;
	}

	// ---- the form
	int fr = 0, fc = 0;
	for (int r = 0; r < 9; r++)
		for (int k = 0; k < 3; k++)
			if (kClassFormRows[r][k] == focus)
				fr = r, fc = k;
	auto rowLen = [](int r) { int n = 0; while (n < 3 && kClassFormRows[r][n] >= 0) n++; return n; };
	if (in.down & KEY_DOWN) fr = (fr + 1) % 9;
	if (in.down & KEY_UP) fr = (fr + 8) % 9;
	int n = rowLen(fr);
	fc = std::min(fc, n - 1);
	if (in.down & KEY_RIGHT) fc = (fc + 1) % n;
	if (in.down & KEY_LEFT) fc = (fc + n - 1) % n;
	focus = kClassFormRows[fr][fc];

	auto label = [&](float y, const std::string& text) {
		float s = 0.45f;
		while (s > 0.33f && uiTextWidth(text, s) > 112)
			s -= 0.02f;
		uiText(6, y + (20 - uiLineHeight(s)) / 2, s, col::header, text);
	};
	int hit = -1;
	auto field = [&](int id, float x, float y, float fw, float fh, const std::string& text) {
		if (uiButton(x, y, fw, fh, text, focus == id))
			hit = id;
	};
	label(24, w.game.gmstLabel("sname", "Name") + ":");
	field(CF_NAME, 120, 24, 196, 20, c.name);
	label(46, w.game.gmstLabel("schooseclassmenu1", "Specialization:"));
	field(CF_SPEC, 120, 46, 196, 20, specName(w, c.specialization));
	label(68, w.game.gmstLabel("schooseclassmenu2", "Favorite Attributes:"));
	field(CF_ATTR0, 120, 68, 96, 20, attrName(w, c.attributes[0]));
	field(CF_ATTR1, 220, 68, 96, 20, attrName(w, c.attributes[1]));
	uiText(6, 92, 0.45f, col::header, w.game.gmstLabel("schooseclassmenu3", "Major Skills:"));
	uiText(164, 92, 0.45f, col::header, w.game.gmstLabel("schooseclassmenu4", "Minor Skills:"));
	for (int i = 0; i < 5; i++)
	{
		field(CF_MAJOR0 + i, 4, 107 + i * 19, 154, 18, skillName(w, c.major[i]));
		field(CF_MINOR0 + i, 162, 107 + i * 19, 154, 18, skillName(w, c.minor[i]));
	}
	field(CF_DESC, 4, 208, 128, 28, w.game.gmstLabel("screateclassmenu1", "Class Description"));
	field(CF_BACK, 136, 208, 88, 28, w.game.gmst("sback", "Back"));
	field(CF_OK, 228, 208, 88, 28, w.game.gmst("sok", "OK"));

	if (hit == CF_NAME || hit == CF_DESC)
	{
		playSound(-1, "Menu Click");
		wantText = hit == CF_NAME ? TEXT_CLASSNAME : TEXT_CLASSDESC;
	}
	else if (hit >= CF_SPEC && hit < CF_DESC)
	{
		playSound(-1, "Menu Click");
		classPicking = hit;
		scroll = UiScroll();
		// the list opens on what the field holds
		int now = hit == CF_SPEC ? c.specialization : hit <= CF_ATTR1 ? c.attributes[hit - CF_ATTR0] : classSkill(c, hit);
		classPickCursor = 0;
		if (hit == CF_SPEC)
			classPickCursor = now;
		else if (hit <= CF_ATTR1)
			classPickCursor = (now / 4) * 16 + now % 4;
		else if (now >= 0 && now < (int)w.game.skills.size())
		{
			int spec = w.game.skills[now].specialization, row = 0;
			for (int k = 0; k < now; k++)
				row += w.game.skills[k].specialization == spec;
			classPickCursor = spec * 16 + row;
			// (scrolled to show it)
			scroll.scroll = std::max(0.0f, row * kSkillPickStep + kSkillPickRowH - (kSkillPickBottom - kSkillPickTop));
		}
	}
	else if (hit == CF_BACK || (in.down & KEY_B))
		openScreen(SCR_CLASS_METHOD);
	else if (hit == CF_OK)
	{
		c.id = kCustomClassId;
		c.playable = true;
		w.stats.customClass = c;
		w.stats.cls = c.id;
		w.recomputeStats();
		logf("chargen: class %s (created: %s; %s, %s; major %s %s %s %s %s; minor %s %s %s %s %s)", c.name.c_str(),
			specName(w, c.specialization).c_str(), attrName(w, c.attributes[0]).c_str(), attrName(w, c.attributes[1]).c_str(),
			skillName(w, c.major[0]).c_str(), skillName(w, c.major[1]).c_str(), skillName(w, c.major[2]).c_str(),
			skillName(w, c.major[3]).c_str(), skillName(w, c.major[4]).c_str(), skillName(w, c.minor[0]).c_str(),
			skillName(w, c.minor[1]).c_str(), skillName(w, c.minor[2]).c_str(), skillName(w, c.minor[3]).c_str(),
			skillName(w, c.minor[4]).c_str());
		closeScreen();
	}
}
