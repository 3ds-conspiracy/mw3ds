// What the interpreter asks of the game (ScriptHost): message boxes, menus, greetings, notices
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwrender/objectpaging.hpp"
#include "mwrender/animation.hpp"
#include "mwbase/session.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "platform/gamma.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "platform/memstat.hpp"
#include <malloc.h>
#include "mwbase/sessionhelpers.hpp"

// The tutorial's console wording (Xbox returns 1), said with the 3DS's controls
static std::string for3ds(std::string t)
{
	// (GoldenEye keeps the Xbox layout's buttons)
	bool xbox = g_controlLayout == CONTROLS_XBOX || g_controlLayout == CONTROLS_GOLDENEYE;
	const char* swap[][2] = {
		{ "The LEFT THUMBSTICK moves you, and the RIGHT THUMBSTICK looks around.",
		  g_controlLayout == CONTROLS_GOLDENEYE
			? "The Circle Pad walks and turns; hold X (or R, winding up a blow) and it aims instead, up and down too."
			: "The Circle Pad moves you, and the C-Stick looks around (the D-Pad looks up and down)." },
		{ "Press B to use your menus. When you are done with them, press B again to close them.",
		  "Tap the buttons on the bottom screen to use your menus (START for saves). Press B to close them." },
		{ "Press the WHITE button to use your journal", "Tap Journal on the bottom screen to use your journal" },
		{ "Press the X button to pull out your weapon. Once your weapon is readied, hold and release the RIGHT TRIGGER to swing it.",
		  g_controlLayout == CONTROLS_GOLDENEYE ? "Equip your weapon, then tap X to ready it, and hold and release R to swing it."
			: xbox ? "Equip your weapon, then press X to ready it, and hold and release R to swing it."
			: "Equip your weapon, then hold and release X to swing it." },
		{ "Equip the dagger by dropping it on your picture in your inventory menu.",
		  "Equip the dagger: tap Items on the bottom screen, then tap the dagger twice." },
		{ "Like all menus, press B to use your new menu. The triggers cycle between menus. Press B again to exit your menus.",
		  "Like all menus, tap it on the bottom screen; B closes it." },
		{ "Press Y to ready your Active Magic, then the RIGHT TRIGGER to cast.",
		  xbox ? "Choose your Active Magic in the Magic menu, then press L to cast it."
			: "Choose your Active Magic in the Magic menu, then press Y to cast it." },
		{ "Press BLACK to rest", "Press START and choose Rest / Wait to rest" },
		{ "Then select 'Take' to pick them up.", "Then tap Take to pick them up." },
	};
	for (auto& s : swap)
	{
		size_t at = t.find(s[0]);
		if (at != std::string::npos)
			t.replace(at, strlen(s[0]), s[1]);
	}
	return t;
}

void Session::messageBox(const std::string& text0, const std::vector<std::string>& buttons)
{
	std::string text = for3ds(text0);
	if (buttons.empty())
	{
		notify(text);
		return;
	}
	buttonPressed = -1;          // a new box: no answer yet
	messages.push_back({ text, buttons, true });
	logf("messagebox: %s", text.c_str());
}

int Session::takeButtonPressed()
{
	int b = buttonPressed;
	buttonPressed = -1;
	return b;
}

void Session::openMenu(ScriptMenu menu)
{
	pendingMenus.push_back(menu);
}

// ForceGreeting: the speaker opens the conversation, hostile or not (Dagoth Gares greets Nerevar before the
// fight) and without their OnActivate (that's the player's activation)
void Session::forceGreeting(int ref)
{
	if (ref < 0 || dlg.open || !w.active(ref))
		return;
	Ref& r = w.refs[ref];
	if (r.dead || !r.visible() || r.actor < 0)
		return;
	float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
	float dz = r.pos[2] + 115.0f - (w.player.feet[2] + PLAYER_EYE_HEIGHT);
	w.player.yaw = atan2f(dx, dy);
	w.player.pitch = atan2f(dz, fmaxf(1.0f, sqrtf(dx * dx + dy * dy)));
	logf("dialogue: %s greets the player (ForceGreeting)", r.id.c_str());
	if (dialogueStart(dlg, w, *this, ref))
	{
		wordsRevision = -1;
		openScreen(SCR_DIALOGUE);
	}
}

void Session::dialogueChoice(const std::vector<std::pair<std::string, int>>& choices)
{
	// each Choice adds to the ones already offered
	dlg.choices.insert(dlg.choices.end(), choices.begin(), choices.end());
	dlg.revision++;
}

void Session::dialogueGoodbye()
{
	dlg.goodbye = true;
	dlg.choices.clear();         // (OpenMW) Goodbye ends any choice
	dlg.revision++;
}

void Session::notify(const std::string& text)
{
	notes.push_back({ text, w.time + 5.0f });
	if (notes.size() > 4)
		notes.erase(notes.begin());
	logf("notify: %s", text.c_str());
}
