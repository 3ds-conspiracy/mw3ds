// Talking and the services NPCs offer: dialogue, barter, spells, training, travel, arrest
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"

void Session::layoutDialogue(float width)
{
	words.clear();
	linkTopics = dlg.topics;
	std::vector<std::string> linkNames, linkIds;
	for (auto& t : linkTopics)
	{
		linkNames.push_back(lower(w.game.topicKeyword(t)));
		linkIds.push_back(lower(t));
	}
	float lh = uiLineHeight(0.5f), space = uiTextWidth(" ", 0.5f);
	float y = 0.0f, lastEntryY = 0.0f;

	auto addText = [&](const std::string& raw, u32 color, int forcedAction) {
		HyperText h = forcedAction == -1 ? dialogueHyperText(w.game, raw, linkNames, linkIds) : HyperText{ raw, {} };
		const std::string& text = h.text;
		std::vector<int> linkAt(text.size(), -1);
		if (forcedAction == -1)
			for (auto& m : h.links)
				for (size_t k = m.begin; k < m.end; k++)
					linkAt[k] = m.keyword;
		float x = 0.0f;
		size_t i = 0;
		while (i < text.size())
		{
			if (text[i] == '\n')
			{
				x = 0.0f;
				y += lh;
				i++;
				continue;
			}
			if (text[i] == ' ')
			{
				i++;
				continue;
			}
			size_t e = i;
			while (e < text.size() && text[e] != ' ' && text[e] != '\n')
				e++;
			std::string word = text.substr(i, e - i);
			float ww = uiTextWidth(word, 0.5f);
			if (x > 0.0f && x + ww > width)
			{
				x = 0.0f;
				y += lh;
			}
			int action = forcedAction != -1 ? forcedAction : linkAt[i];
			bool link = action >= 0 || (action <= -2 && action > -1000);
			words.push_back({ word, x, y, ww, link ? col::link : color, action });
			x += ww + space;
			i = e;
		}
		y += lh;
	};

	for (auto& e : dlg.history)
	{
		lastEntryY = y;
		if (!e.header.empty())
			addText(e.header, col::header, -1000);
		addText(e.text, col::text, -1);
		y += lh * 0.5f;
	}
	for (size_t i = 0; i < dlg.choices.size(); i++)
		addText(std::to_string(i + 1) + ". " + dlg.choices[i].first, col::link, -2 - (int)i);
	wordsHeight = y;

	// Show the newest response from its first line
	dlgScroll = lastEntryY;
	wordsRevision = dlg.revision;
}

void Session::drawDialogue()
{
	if (!dlg.open)
	{
		closeScreen();
		return;
	}
	const Ref& speaker = w.refs[dlg.ref];
	header(speaker.actor >= 0 ? w.game.actors[speaker.actor].name : speaker.id.str());

	const float tx = 4, ty = 24, tw = 206, th = 212;
	if (wordsRevision != dlg.revision)
		layoutDialogue(tw - 6);
	const UiInput& in = uiIn();
	if (in.touching && in.touchX < tx + tw && in.touchY > ty)
		dlgScroll -= in.dragDY;
	if (in.held & KEY_R) dlgScroll += 3;
	if (in.held & KEY_L) dlgScroll -= 3;
	dlgScroll += uiStickScroll();
	dlgScroll = fmaxf(0.0f, fminf(dlgScroll, fmaxf(0.0f, wordsHeight - th)));

	uiPanel(tx - 2, ty - 2, tw + 2, th + 2);
	int clicked = -1000;
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (ty + th)), (u32)(320 - (tx + tw)), (u32)(240 - ty), (u32)(320 - tx));
	for (auto& wd : words)
	{
		float y = ty + 2 + wd.y - dlgScroll;
		if (y + 16 < ty || y > ty + th)
			continue;
		uiText(tx + 3 + wd.x, y, 0.5f, wd.color, wd.text);
		if (wd.action != -1 && wd.action != -1000 && uiHit(tx + 3 + wd.x, y, wd.w, 16))
			clicked = wd.action;
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);

	bool choosing = !dlg.choices.empty();
	std::vector<std::string> rows = dlg.goodbye || choosing ? std::vector<std::string>() : dlg.topics;
	// Services sit between the topics and Goodbye
	bool merchant = isMerchant(dlg.ref);
	bool travels = speaker.actor >= 0 && !w.game.actors[speaker.actor].travel.empty();
	bool sells = speaker.actor >= 0 && !w.game.actors[speaker.actor].spells.empty();
	bool trains = speaker.actor >= 0 && (w.game.actors[speaker.actor].services & 0x4000);
	bool persuades = speaker.type == "NPC_";
	bool makesSpells = speaker.actor >= 0 && (w.game.actors[speaker.actor].services & 0x8000);
	bool enchants = speaker.actor >= 0 && (w.game.actors[speaker.actor].services & 0x10000);
	bool repairs = speaker.actor >= 0 && (w.game.actors[speaker.actor].services & 0x20000);
	int services = merchant + travels + sells + trains + persuades + makesSpells + enchants + repairs;
	int pick = uiWrapList(list, 214, 24, 104, 180 - 26 * services, rows, 0.38f, !choosing);
	float sy = 206 - 26 * services;
	if (persuades && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("spersuasion", "Persuasion")))
	{
		list2 = UiList();
		screen = SCR_PERSUADE;
		playSound(-1, "Menu Click");
		return;
	}
	if (persuades)
		sy += 26;
	if (merchant && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("sbarter", "Barter"))
		&& !serviceRefused())
	{
		openBarter(dlg.ref);
		playSound(-1, "Menu Click");
		return;
	}
	if (merchant)
		sy += 26;
	if (travels && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("stravel", "Travel"))
		&& !serviceRefused())
	{
		openTravel();
		return;
	}
	if (travels)
		sy += 26;
	if (sells && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("sspells", "Spells"))
		&& !serviceRefused())
	{
		barterRef = dlg.ref;
		list2 = UiList();
		screen = SCR_SPELLS;
		playSound(-1, "Menu Click");
		return;
	}
	if (sells)
		sy += 26;
	if (trains && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("strain", "Training"))
		&& !serviceRefused())
	{
		barterRef = dlg.ref;
		list2 = UiList();
		screen = SCR_TRAINING;
		playSound(-1, "Menu Click");
		return;
	}
	if (trains)
		sy += 26;
	if (makesSpells && !choosing && !dlg.goodbye
		&& uiButton(216, sy, 100, 24, w.game.gmst("sspellmakingmenutitle", "Spellmaking")) && !serviceRefused())
	{
		list2 = UiList();
		makeEffects.clear();
		makeSel = -1;
		makeName.clear();
		barterRef = dlg.ref;               // (the price is their barter offer)
		screen = SCR_SPELLMAKE;
		playSound(-1, "Menu Click");
		return;
	}
	if (makesSpells)
		sy += 26;
	if (enchants && !choosing && !dlg.goodbye
		&& uiButton(216, sy, 100, 24, w.game.gmst("senchanting", "Enchanting")) && !serviceRefused())
	{
		openEnchanting(dlg.ref);
		playSound(-1, "Menu Click");
		return;
	}
	if (enchants)
		sy += 26;
	if (repairs && !choosing && !dlg.goodbye && uiButton(216, sy, 100, 24, w.game.gmst("srepair", "Repair"))
		&& !serviceRefused())
	{
		openRepair(dlg.ref, -1);
		playSound(-1, "Menu Click");
		return;
	}
	if (repairs)
		sy += 26;
	if (choosing)
	{
		// choices: tap them in the text, or press 1-3 style with D-pad + A
		std::vector<std::string> texts;
		for (auto& ch : dlg.choices)
			texts.push_back(ch.first);
		static UiList choiceList;
		if (choiceList.selected < 0 || choiceList.selected >= (int)texts.size())
			choiceList.selected = 0;
		int c = uiWrapList(choiceList, 214, 24, 104, 180, texts, 0.38f, true);
		focus = choiceList.selected;
		if (c >= 0)
			clicked = -2 - c;
	}
	if (pick >= 0 && pick < (int)rows.size())
		clicked = -3000 - pick;
	bool bye = uiButton(216, 208, 100, 28, w.game.gmst("sgoodbye", "Goodbye"), false, !choosing) ||
		(!choosing && (in.down & KEY_B));

	if (clicked <= -3000)
	{
		playSound(-1, "Menu Click");
		dialogueTopic(dlg, w, *this, rows[-3000 - clicked]);
	}
	else if (clicked >= 0 && clicked < (int)linkTopics.size() && !choosing && !dlg.goodbye)
	{
		playSound(-1, "Menu Click");
		dialogueTopic(dlg, w, *this, linkTopics[clicked]);
	}
	else if (clicked <= -2 && clicked > -1000 && choosing)
	{
		int i = -2 - clicked;
		playSound(-1, "Menu Click");
		focus = 0;
		dialogueChoose(dlg, w, *this, dlg.choices[i].second);
	}
	else if (bye)
		closeScreen();
}

// ---- persuasion

bool Session::serviceRefused()
{
	if (!dialogueReact(dlg, w, *this, "Service Refusal"))
		return false;
	wordsRevision = -1;
	return true;
}

// Admire / intimidate / taunt / bribe, then back to the talk with their reaction
void Session::drawPersuade()
{
	const Ref& npc = w.refs[dlg.ref];
	header(w.game.gmst("spersuasion", "Persuasion"), "Disposition " + std::to_string(w.disposition(dlg.ref)));
	int gold = w.itemCount("gold_001");
	static const char* gm[6] = { "sadmire", "sintimidate", "staunt", "sbribe 10 gold", "sbribe 100 gold", "sbribe 1000 gold" };
	static const char* fallback[6] = { "Admire", "Intimidate", "Taunt", "Bribe 10 Gold", "Bribe 100 Gold", "Bribe 1000 Gold" };
	static const int cost[6] = { 0, 0, 0, 10, 100, 1000 };
	for (int k = 0; k < 6; k++)
		if (uiButton(60, 30 + k * 28, 200, 24, w.game.gmst(gm[k], fallback[k]), false, cost[k] <= gold) && cost[k] <= gold)
		{
			playSound(-1, "Menu Click");
			persuade(k);
			return;
		}
	uiText(8, 202, 0.42f, col::textDim, w.game.gmstLabel("sgold", "Gold") + " " + std::to_string(gold));
	if (uiButton(216, 208, 100, 28, w.game.gmst("sback", "Back")) || (uiIn().down & KEY_B))
	{
		playSound(-1, "Menu Click");
		screen = SCR_DIALOGUE;
	}
}

// Morrowind's persuasion (OpenMW's getPersuasionDispositionChange): ratings from Speechcraft / Mercantile,
// Personality, Luck, reputation and level, the player's against the NPC's, scaled by how far their disposition is
// from 50. The result is a temporary change (gone at Goodbye) and a permanent one; Intimidate and Taunt also
// move Fight and Flee, a bribe costs its gold only when it works, and the NPC keeps it
void Session::persuade(int kind)
{
	Ref& npc = w.refs[dlg.ref];
	if (npc.actor < 0)
		return;
	const ActorDef& def = w.game.actors[npc.actor];
	auto g = [&](const char* name, float fb) { return w.game.gmstf(name, fb); };
	int disp = w.disposition(dlg.ref);     // what they feel now
	float t = persuadeChance(dlg.ref, kind);              // (mwmechanics/formulas.cpp)
	float minChange = g("iperminchange", 5.0f), dieMult = g("fperdierollmult", 0.15f), tempMult = g("fpertempmult", 1.0f);
	int roll = persuadeRoll >= 0 ? persuadeRoll : rand() % 100;
	bool ok = roll <= t;
	float x = 0.0f, y = 0.0f;
	int fight = npc.fight >= 0 ? npc.fight : def.fight;
	int flee = npc.flee >= 0 ? npc.flee : def.flee;
	if (kind == 0)
	{
		float c = floorf(dieMult * (t - roll));
		x = ok ? fmaxf(minChange, c) : c;
	}
	else if (kind == 1)
	{
		float r = roll != (int)t ? floorf(t - roll) : 1.0f;
		if (ok)
		{
			float s = floorf(r * dieMult * tempMult);
			flee = std::max(0, std::min(100, flee + (int)fmaxf(minChange, s)));
			fight = std::max(0, std::min(100, fight + (int)fminf(-minChange, -s)));
		}
		float c = -fabsf(floorf(r * dieMult));
		if (ok)
		{
			if (fabsf(c) < minChange)
				y = x = minChange;
			else
			{
				x = -floorf(c * tempMult);
				y = c;
			}
		}
		else
		{
			x = floorf(c * tempMult);
			y = c;
		}
	}
	else if (kind == 2)
	{
		float c = fabsf(floorf(t - roll));
		if (ok)
		{
			float s = c * dieMult * tempMult;
			flee = std::max(0, std::min(100, flee + std::min(-(int)minChange, (int)-s)));
			fight = std::max(0, std::min(100, fight + std::max((int)minChange, (int)s)));
		}
		x = floorf(-c * dieMult);
		if (ok && fabsf(x) < minChange)
			x = -minChange;
	}
	else
	{
		float c = floorf((t - roll) * dieMult);
		x = ok ? fmaxf(minChange, c) : c;
		static const int bribe[3] = { 10, 100, 1000 };
		if (ok)
		{
			w.removeItem("gold_001", bribe[kind - 3]);
			npc.gold += bribe[kind - 3];
		}
	}
	int temp = (int)(kind == 1 ? x : x * tempMult);
	temp = std::max(-disp, std::min(100 - disp, temp));
	int perm = kind == 1 ? (ok ? -(int)(temp / tempMult) : (int)y) : (int)(temp / tempMult);
	npc.fight = fight;
	npc.flee = flee;
	dialoguePersuaded(dlg, w, temp, perm);
	useSkill(25, ok ? 0 : 1);             // Speechcraft: a success, or (less) a failure
	logf("persuade: %d on %s, roll %d, %s, disposition %d -> %d (temp %d, perm %d), fight %d", kind, npc.id.c_str(), roll,
		ok ? "success" : "fail", disp, w.disposition(dlg.ref), temp, perm, fight);
	static const char* topics[6][2] = { { "Admire Fail", "Admire Success" }, { "Intimidate Fail", "Intimidate Success" },
		{ "Taunt Fail", "Taunt Success" }, { "Bribe Fail", "Bribe Success" }, { "Bribe Fail", "Bribe Success" },
		{ "Bribe Fail", "Bribe Success" } };
	dialogueReact(dlg, w, *this, topics[kind][ok]);
	wordsRevision = -1;
	screen = SCR_DIALOGUE;
}
