// Tokens: placing, visuals, enable / disable, race, sneak, items, settings
#include <optional>
#include <3ds.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <queue>
#include <tuple>
#include "testdrive/testdrive.hpp"
#include "platform/gamma.hpp"
#include "mwbase/session.hpp"
#include "components/debug/debuglog.hpp"

extern const char* g_playerBlock;
bool g_testDepth16 = false;
#include <malloc.h>
#include "testdrive/testhelpers.hpp"

std::optional<bool> opsWorld(Session& s, const std::vector<std::string>& a)
{
	World& w = s.w;
	const std::string& verb = a[0];
	auto fail = [&](const std::string& why) { logf("drive: FAIL %s: %s", verb.c_str(), why.c_str()); return true; };
	// PLACEAT:<id>:x:y:z: that actor put there (on the floor under it), as PLACE puts it in front of the player but at a
	// point that doesn't hang on where the player ended up (side-by-side stand-ins, tools/test/sidebyside.py)
	if (verb == "PLACEAT" && a.size() >= 5)
	{
		float at[3] = { (float)atof(a[2].c_str()), (float)atof(a[3].c_str()), (float)atof(a[4].c_str()) };
		s.testPlaceAt(a[1], at, atan2f(w.player.feet[0] - at[0], w.player.feet[1] - at[1]));
		return true;
	}
	// VFX:<static>:x:y:z: that spell visual played there on its own (side-by-side shots against OpenMW's world.vfx)
	if (verb == "VFX" && a.size() >= 5)
	{
		float at[3] = { (float)atof(a[2].c_str()), (float)atof(a[3].c_str()), (float)atof(a[4].c_str()) };
		// (a token can't hold a space: an id written with underscores is tried with spaces too, "blight_cloud")
		std::string id = lower(a[1]);
		if (!w.game.firstPerson.vfx.count(id) && !w.game.object(id))
			id = lower(spaced(a[1]));
		// (VFX:<id>:x:y:z:<effect>: drawn for that magic effect, its particle texture on the shapes that take one)
		int effect = a.size() >= 6 ? atoi(a[5].c_str()) : -1;
		s.spawnVfx(id, at, 1.2f, -2, 0.0f, 1.0f, false, effect);
		logf("test: vfx %s at %.0f %.0f %.0f (%d showing)", a[1].c_str(), at[0], at[1], at[2], (int)s.vfx.size());
		return true;
	}
	if (verb == "HIDEACTORS" && a.size() >= 2)
	{
		float r = (float)atof(a[1].c_str());
		int n = 0;
		for (LoadedCell* l : w.loaded)
			for (const Actor& ac : l->actors.actors)
			{
				if (ac.ref < 0 || !w.refs[ac.ref].visible())
					continue;
				float dx = ac.place[3] - w.player.feet[0], dy = ac.place[7] - w.player.feet[1];
				if (dx * dx + dy * dy < r * r)
				{
					w.setEnabled(ac.ref, false);
					n++;
				}
			}
		logf("test: %d actors hidden", n);
		return true;
	}
	if ((verb == "ENABLE" || verb == "DISABLE") && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		w.setEnabled(ri, verb == "ENABLE");
		return true;
	}
	// PCRACE:<race id>: the player's race (Same Race, %PCRace)   PCSEX:<0|1>: male / female (Same Sex, PC Gender)
	// PCNAME:<name>: the player's name (%PCName)   (underscores for spaces)
	if (verb == "PCRACE" && a.size() >= 2)
	{
		w.stats.race = lower(spaced(a[1]));
		return true;
	}
	// SETRACE:<race id>: as PCRACE (a Khajiit tries the helmet)
	if (verb == "SETRACE" && a.size() >= 2)
	{
		w.stats.race = lower(spaced(a[1]));
		return true;
	}
	// SNEAK:<0|1>: the player sneaks (or not)   WEREWOLF:<0|1>: the player is a werewolf (what it refuses)
	if (verb == "SNEAK" && a.size() >= 2)
	{
		s.testSneak = atoi(a[1].c_str()) != 0;
		return true;
	}
	if (verb == "WEREWOLF" && a.size() >= 2)
	{
		s.werewolf = atoi(a[1].c_str()) != 0;
		return true;
	}
	// KNOCKDOWN:<npc>: they lie knocked down for a long while (their pockets open to anyone)
	if (verb == "KNOCKDOWN" && a.size() >= 2)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		w.refs[ri].knockTimer = 3600.0f;
		return true;
	}
	// SETITEMHEALTH:<id>:<n>: the carried piece's condition (0: broken)
	if (verb == "SETITEMHEALTH" && a.size() >= 3)
	{
		int i = carried(w, a[1]);
		if (i < 0)
			return fail(a[1] + " not carried");
		w.inventory[i].condition = atoi(a[2].c_str());
		return true;
	}
	// SETITEMCHARGE:<id>:<n>: the carried enchanted item's charge left
	if (verb == "SETITEMCHARGE" && a.size() >= 3)
	{
		int i = carried(w, a[1]);
		if (i < 0)
			return fail(a[1] + " not carried");
		w.inventory[i].charge = (float)atof(a[2].c_str());
		return true;
	}
	// USELOCKPICK:<door or container>:<tool> / USEPROBE: the tool is held up to that object (it faces it), one try;
	// ROLL holds the die
	if ((verb == "USELOCKPICK" || verb == "USEPROBE") && a.size() >= 3)
	{
		// the one in the crosshair when it has that id (shared door ids), else the nearest in 3D: a tool is held up to what is
		// in front of the player, not to the door the shortest walk leads to
		int ri = findRef(s, a[1], false, verb == "USELOCKPICK"), ti = carried(w, a[2]);
		if (s.target >= 0 && s.target < (int)w.refs.size() && (w.refs[s.target].idLower == lower(a[1]) || w.refs[s.target].idLower == lower(spaced(a[1]))) &&
			(verb != "USELOCKPICK" || ri < 0 || w.refs[s.target].lockLevel > 0 || w.refs[ri].lockLevel <= 0))
			ri = s.target;
		if (ri < 0)
			return fail(a[1] + " not loaded");
		if (ti < 0)
			return fail(a[2] + " not carried");
		if (!w.inventory[ti].equipped)
			s.equipItem(ti, true);
		{
			const Ref& t = w.refs[ri];
			logf("drive: %s on %s at %.0f %.0f %.0f (%s%d)", a[0].c_str(), t.id.c_str(), t.pos[0], t.pos[1], t.pos[2],
				(t.recType == REC_DOOR || t.recType == REC_CONT) ? "lock " : "not a door or container, lock ", t.lockLevel);
		}
		int was = s.target;
		s.target = ri;
		s.useTool(w.inventory[ti]);
		s.target = was;
		return true;
	}
	if (verb == "PCSEX" && a.size() >= 2)
	{
		w.stats.female = atoi(a[1].c_str()) != 0;
		return true;
	}
	if (verb == "PCNAME" && a.size() >= 2)
	{
		w.stats.name = spaced(a[1]);
		return true;
	}
	// TYPE:<text>: what the system keyboard returns when a name is asked for (the made spell's / item's); % for spaces
	if (verb == "TYPE" && a.size() >= 2)
	{
		s.typedText = spaced(a[1]);
		return true;
	}
	// NOTIFY:<text>: a notice on the top screen, as a script's or the game's (underscores for spaces)
	if (verb == "NOTIFY" && a.size() >= 2)
	{
		s.notify(spaced(a[1]));
		return true;
	}
	// MSGBOX:<text>:<button>...: a message box with those buttons, as a script's MessageBox
	if (verb == "MSGBOX" && a.size() >= 3)
	{
		MessageState m;
		m.text = spaced(a[1]);
		for (size_t k = 2; k < a.size(); k++)
			m.buttons.push_back(spaced(a[k]));
		m.fromScript = true;
		s.messages.push_back(m);
		return true;
	}
	// TOPICLOG:<topic>:<line>: the journal's Topics index has that line about the topic (underscores for spaces)
	if (verb == "TOPICLOG" && a.size() >= 3)
	{
		w.topicLog[lower(spaced(a[1]))].push_back(spaced(a[2]));
		return true;
	}
	// SETITEM:<id>:<n>: the player holds exactly n of the item (Item conditions)
	if (verb == "SETITEM" && a.size() >= 3)
	{
		std::string id = w.game.object(a[1]) ? a[1] : spaced(a[1]);
		int have = w.itemCount(id), want = atoi(a[2].c_str());
		if (want > have)
			w.addItem(id, want - have);
		else if (want < have)
			w.removeItem(id, have - want);
		return true;
	}
	// SETDEAD:<id>:<n>: how many of that id have died (Dead conditions)
	if (verb == "SETDEAD" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		w.deadCounts[ri >= 0 ? w.refs[ri].idLower.str() : lower(a[1])] = atoi(a[2].c_str());
		return true;
	}
	// SETTALKED:<npc>:<0|1>: the NPC's real talked-to-player flag (a talk in progress keeps the one it began with)
	if (verb == "SETTALKED" && a.size() >= 3)
	{
		int ri = findRef(s, a[1]);
		if (ri < 0)
			return fail(a[1] + " not loaded");
		w.refs[ri].talkedToPC = atoi(a[2].c_str()) != 0;
		return true;
	}
	// CLOTHVALUE:<n>: the Clothing Modifier reads n whatever is worn (-1: what is worn again)
	if (verb == "CLOTHVALUE" && a.size() >= 2)
	{
		g_dialogueClothValue = atoi(a[1].c_str());
		return true;
	}
	// GAMMA:<percent> / AUTOGAMMA:on|off: the Display settings, saved as the screen saves them. LUX:<lux>: a light reading
	// as if from QTM (the New 3DS's), polled as it is; CAMLUMA:<0..255>: as if from the camera at the next loading screen
	// (CAMLUMA:-1: the real inner camera there, as an Old 3DS reads it)
	// LOOKSPEED / GETURN / GEAIM:<percent>, INVERTY:on|off (Controls > Look), SUBTITLES:on|off (Display): saved as the
	// screens save them
	if ((verb == "LOOKSPEED" || verb == "GETURN" || verb == "GEAIM" || verb == "INVERTY" || verb == "SUBTITLES") && a.size() >= 2)
	{
		int pct = std::max(kLookMinPct, std::min(kLookMaxPct, atoi(a[1].c_str())));
		if (verb == "LOOKSPEED") g_lookSpeed = pct;
		else if (verb == "GETURN") g_geTurnSpeed = std::max(kGeTurnMinPct, std::min(kGeTurnMaxPct, atoi(a[1].c_str())));
		else if (verb == "GEAIM") g_lookSpeed = pct;      // (the GoldenEye aim looks at the look speed)
		else if (verb == "INVERTY") g_invertY = a[1] == "on";
		else g_subtitles = a[1] != "off";
		s.saveSettings();
		logf("test: %s %s", verb.c_str(), a[1].c_str());
		return true;
	}
	// DEPTH16:on|off: the top screen's 16-bit depth buffer in 2D too (Engine::createTopTargets takes it in 3D only): shots
	// of far surfaces fighting, in the emulator
	if (verb == "DEPTH16" && a.size() >= 2)
	{
		g_testDepth16 = a[1] == "on";
		logf("test: depth16 %s", g_testDepth16 ? "on" : "off");
		return true;
	}
	// SAYLINE:<npc>[:novoice]: the NPC speaks a line, voiced (the first voice file in the data) or text only
	if (verb == "SAYLINE" && a.size() >= 2)
	{
		int ri = s.testFindRef(a[1]);
		if (ri < 0)
			return fail("no " + a[1]);
		bool voiced = !(a.size() >= 3 && a[2] == "novoice");
		if (voiced && w.game.voices.empty() && w.game.voiceFiles.empty())
			return fail("no voice files in the data");
		std::string line = !voiced ? "" : !w.game.voices.empty() ? *w.game.voices.begin() : w.game.voiceFiles.begin()->first;
		s.say(ri, line, voiced ? "A line spoken for the test." : "A line written for the test.");
		logf("test: %s says a %s line", a[1].c_str(), voiced ? "voiced" : "text-only");
		return true;
	}
	if ((verb == "GAMMA" || verb == "AUTOGAMMA" || verb == "LUX" || verb == "CAMLUMA") && a.size() >= 2)
	{
		if (verb == "GAMMA")
			g_gammaPct = std::max(kGammaMinPct, std::min(kGammaMaxPct, atoi(a[1].c_str())));
		else if (verb == "AUTOGAMMA")
			g_autoGamma = a[1] == "on" ? 1 : 0;
		else if (verb == "LUX")
			gammaTestLux((float)atof(a[1].c_str()));
		else
			gammaTestLuma(atoi(a[1].c_str()));
		if (verb == "GAMMA" || verb == "AUTOGAMMA")
			s.saveSettings();
		logf("test: %s %s", verb.c_str(), a[1].c_str());
		return true;
	}
	return std::nullopt;
}
