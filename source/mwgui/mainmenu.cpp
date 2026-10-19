// The game menu (save, load, options, quit), death and the end
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include <cstdlib>
#include <malloc.h>
#include <cctype>
#include "mwgui/statshelpers.hpp"

// START: the game menu (Morrowind's Esc menu)
void Session::toggleGameMenu()
{
	if (screen == SCR_GAMEMENU)
		closeScreen();
	else if (screen == SCR_NONE && messages.empty() && !dlg.open && !wantName)
	{
		openScreen(SCR_GAMEMENU);
		playSound(-1, "Menu Click");
	}
}

void Session::drawGameMenu()
{
	header("Menu", w.game.cellLabel(w.cellName()));
	// Save and quit: "Saving..." on screen first, then the save (a long campaign's takes seconds: it looked frozen),
	// then "Closing..." for the quit itself: a screen that stays says which of the two it is stuck in. A save that
	// failed doesn't quit (its notice shows, the menu stays)
	if (quitSaving < 0)
	{
		uiTextCentered(160, 110, 0.6f, col::text, "Game saved. Closing...");
		wantQuit = true;
		return;
	}
	if (quitSaving > 0)
	{
		uiTextCentered(160, 110, 0.6f, col::text, w.game.gmstLabel("snotifymessage4", "Saving..."));
		if (++quitSaving > 2)
			quitSaving = save() ? -1 : 0;
		return;
	}
	const UiInput& in = uiIn();
	// OpenMW's getRestEnabled: EnableRest has run, or character creation has finished (CharGenState -1)
	auto cg = w.globals.find("chargenstate");
	bool rest = (w.menusEnabled & MENU_REST) || !w.game.chargen || (cg != w.globals.end() && cg->second == -1.0f);
	std::vector<std::string> labels = { "Return to game", "Save / Load", w.game.gmst("srestmenuxbox", "Rest / Wait"),
		w.game.gmst("soptions", "Options"), "Save and quit" };
	bool enabled[5] = { true, !wantName, rest, true, pendingMenus.empty() };
	int n = labels.size();
	if (in.down & KEY_DOWN) focus = (focus + 1) % n;
	if (in.down & KEY_UP) focus = (focus + n - 1) % n;
	int hit = -1;
	for (int i = 0; i < n; i++)
		if (uiButton(50, 32 + i * 40, 220, 34, labels[i], i == focus, enabled[i]))
			hit = i;
	if (hit == 0 || (in.down & KEY_B))
		closeScreen();
	else if (hit == 1)
	{
		saveList = savesList(dataDir);
		closeScreen();
		openScreen(SCR_SAVES);
	}
	else if (hit == 2)
	{
		closeScreen();
		openRest(false);
	}
	else if (hit == 3)
	{
		closeScreen();
		focus = 0;
		openScreen(SCR_OPTIONS);
	}
	else if (hit == 4)
		quitSaving = 1;
}

void Session::drawDeath()
{
	// Morrowind and OpenMW: death leads to loading a game, never a getting up again where one fell
	header("You have died");
	uiPanel(4, 24, 312, 150);
	bool haveSave = !deathSave.empty();
	uiTextBox(scroll, 10, 30, 300, 140, haveSave ? "Load your last save, or choose another."
		: deathAnySave ? "Choose a save to load." : "There is no save yet. Start over.", 0.5f, false);
	std::vector<std::string> labels;
	if (haveSave)
		labels.push_back("Load last save");
	if (deathAnySave)
		labels.push_back("Choose a save");
	else
		labels.push_back("Start over");
	int b = buttonRow(labels, focus);
	if (b >= 0 && labels[b] == "Load last save")
	{
		reloadPath = deathSave;
		wantReload = true;
	}
	else if (b >= 0 && labels[b] == "Choose a save")
	{
		saveList = savesList(dataDir);
		list.selected = saveList.empty() ? -1 : 0;
		openScreen(SCR_SAVES);
	}
	else if (b >= 0)
	{
		reloadPath = kAutosavePath;     // (none there: a new game)
		wantReload = true;
	}
}

void Session::drawEnd()
{
	header("Released");
	std::string text = "You step out of the Census and Excise Office, a free " +
		std::string(w.stats.female ? "woman" : "man") + " in Vvardenfell.\n\n" +
		statsText(*this, false);
	uiPanel(4, 24, 312, 178);
	uiTextBox(scroll, 6, 26, 308, 174, text, 0.45f, true);
	if (buttonRow({ "Keep exploring" }, focus) == 0)
		closeScreen();
}
