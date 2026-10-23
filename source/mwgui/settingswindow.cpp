// Options: preferences, display and controls
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "platform/gamma.hpp"
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

// Options, in Morrowind's tabs: Prefs, Audio, Video and Controls (L / R, or a tap on a tab). Each tab is a column of
// settings, a bar (D-pad left / right, or a tap on it, sets it) or a value (A, a tap or D-pad right steps to the next,
// left to the one before), over its buttons; D-pad up / down moves between them. Kept in settings.txt. Each tab is its
// own screen (options, audio, display, look), so B and Close leave from any of them. 3D has no setting: the 3D slider
// alone turns it on and off
static const Screen kOptionTabs[4] = { SCR_OPTIONS, SCR_AUDIO, SCR_DISPLAY, SCR_LOOK };
static const int kControlsButtonsFocus = 6;   // Controls' Layout button, where the button list's Close comes back to

void Session::drawOptions()
{
	int tab = 0;
	while (tab < 3 && kOptionTabs[tab] != screen)
		tab++;
	header(w.game.gmst("soptions", "Options"));
	const UiInput& in = uiIn();
	static const char* kTabKeys[4][2] = { { "sprefs", "Prefs" }, { "saudio", "Audio" }, { "svideo", "Video" },
		{ "scontrols", "Controls" } };
	int to = -1;
	for (int i = 0; i < 4; i++)
		if (uiButton(4 + i * 79, 24, 75, 20, w.game.gmstLabel(kTabKeys[i][0], kTabKeys[i][1]), i == tab) && i != tab)
			to = i;
	if (in.down & KEY_L) to = (tab + 3) % 4;
	if (in.down & KEY_R) to = (tab + 1) % 4;
	if (to >= 0)
	{
		openScreen(kOptionTabs[to]);
		return;
	}

	// The focus walks lines: a line holds one setting, or buttons side by side (left / right between them)
	struct Line { int first, count; };
	std::vector<Line> lines;
	auto line = [&](int count) {
		int first = lines.empty() ? 0 : lines.back().first + lines.back().count;
		lines.push_back({ first, count });
		return first;
	};
	bool changed = false;
	// a setting's name at the left, shrunk to fit beside its control (x 118 on)
	auto name = [&](float y, const std::string& text, u32 color) {
		float s = 0.45f;
		while (s > 0.34f && uiTextWidth(text, s) > 104)
			s -= 0.02f;
		uiText(10, y, s, color, text);
	};
	// a bar: its name and value on a line, the bar across the screen under them (a line 34 high)
	auto bar = [&](float y, const std::string& label, const std::string& shown, int& value, int lo, int hi, int step) {
		bool f = focus == line(1);
		uiText(10, y, 0.45f, f ? col::header : col::text, label);
		uiTextRight(310, y, 0.45f, f ? col::header : col::text, shown);
		float bx = 14, bw = 292, by = y + 21;
		uiRect(bx, by, bw, 7, col::panel);
		uiFrame(bx, by, bw, 7);
		uiRect(bx + (float)(value - lo) / (hi - lo) * bw - 4, by - 5, 8, 17, col::header);
		int v = value;
		if (f && (in.down & KEY_LEFT)) v -= step;
		if (f && (in.down & KEY_RIGHT)) v += step;
		if (in.touching && in.touchY >= y + 10 && in.touchY < y + 34 && in.touchX >= bx - 8 && in.touchX <= bx + bw + 8)
		{
			focus = lines.back().first;
			v = lo + (int)roundf(fmaxf(0.0f, fminf(1.0f, (in.touchX - bx) / bw)) * (hi - lo) / step) * step;
		}
		v = v < lo ? lo : v > hi ? hi : v;
		if (v == value)
			return;
		value = v;
		changed = true;
	};
	// a value: its name at the left, the value on a button at the right. Returns the step asked for (-1, 0, 1)
	auto choice = [&](float y, const std::string& label, const std::string& shown, bool enabled = true) {
		bool f = focus == line(1);
		name(y + 4, label, !enabled ? col::textDim : f ? col::header : col::text);
		int d = uiButton(118, y, 198, 22, shown, f, enabled) ? 1 : 0;
		if (enabled && f && (in.down & KEY_RIGHT)) d = 1;
		if (enabled && f && (in.down & KEY_LEFT)) d = -1;
		if (d)
			changed = true;
		return d;
	};
	// buttons side by side across the screen: the one pressed, or -1
	auto buttons = [&](float y, float h, const std::vector<std::string>& labels) {
		int n = labels.size(), first = line(n), hit = -1;
		float bw = (312.0f - (n - 1) * 4.0f) / n;
		for (int i = 0; i < n; i++)
			if (uiButton(4 + i * (bw + 4), y, bw, h, labels[i], focus == first + i))
				hit = i;
		return hit;
	};
	auto onOff = [](bool on) { return std::string(on ? "On" : "Off"); };
	char text[64];
	int b = -1;
	bool closing = false;
	if (screen == SCR_OPTIONS)
	{
		snprintf(text, sizeof(text), "%d", difficulty);
		bar(50, w.game.gmstLabel("sdifficulty", "Difficulty"), text, difficulty, -100, 100, 10);
		// Combat: vanilla, or Glancing Blows and Crits (misses glance, crits; the same average damage)
		if (choice(86, "Combat hits", combatMode == COMBAT_GBAC ? "Glancing blows + crits" : "Vanilla"))
		{
			combatMode = combatMode == COMBAT_GBAC ? COMBAT_VANILLA : COMBAT_GBAC;
			notify(combatMode == COMBAT_GBAC ? "Glancing blows and crits: misses glance for half damage, strong hits can crit."
				: "Vanilla combat.");
		}
		// Hit & Miss Indicators: damage numbers and misses by the crosshair
		if (choice(112, "Hit numbers", onOff(hitIndicators)))
		{
			hitIndicators = !hitIndicators;
			notify(hitIndicators ? "Hit and miss numbers on." : "Hit and miss numbers off.");
		}
		// Subtitles: spoken lines written on the top screen
		if (choice(138, w.game.gmstLabel("ssubtitles", "Subtitles"), onOff(g_subtitles)))
		{
			g_subtitles = !g_subtitles;
			notify(g_subtitles ? "Subtitles on." : "Subtitles off: spoken lines are only heard.");
		}
		// Console: OpenMW's, one script line at a time, on the object the crosshair was on; Credits: Morrowind's movie
		int extra = buttons(170, 26, { "Console", "Credits" });
		b = buttons(208, 28, { "Default", w.game.gmstLabel("sclose", "Close") });
		closing = b == 1;
		if (extra == 0)
		{
			// the object under the crosshair when the menu was opened is the one the lines act on (OpenMW's click on it)
			if (target >= 0)
				console.selected = target;
			closeScreen();
			consoleBack = 0.0f;
			openScreen(SCR_CONSOLE);
			return;
		}
		// Morrowind's credits are its movie (Video\mw_credits.bik): data converted without it keeps Options open and
		// says so, rather than dropping back into the game as if the button did nothing
		if (extra == 1)
		{
			if (playMovie("mw_credits"))
				closeScreen();
			else
				notify("The credits movie is missing: Video\\mw_credits.bik was not converted with the game data.");
			return;
		}
		if (b == 0)
		{
			difficulty = 0;
			combatMode = COMBAT_VANILLA;
			hitIndicators = false;
			g_subtitles = true;
			changed = true;
		}
	}
	else if (screen == SCR_AUDIO)
	{
		snprintf(text, sizeof(text), "%d", effectsVolume);
		bar(50, w.game.gmstLabel("seffects", "Effects"), text, effectsVolume, 0, 100, 10);
		snprintf(text, sizeof(text), "%d", musicVolume);
		bar(86, w.game.gmstLabel("smusic", "Music"), text, musicVolume, 0, 100, 10);
		b = buttons(208, 28, { "Default", w.game.gmstLabel("sclose", "Close") });
		closing = b == 1;
		if (b == 0)
		{
			effectsVolume = musicVolume = 100;
			changed = true;
		}
	}
	else if (screen == SCR_DISPLAY)
	{
		// Gamma (0.80 to 1.80), and a ramp dark to light through the curve in use (what the bar does to the dim end);
		// the world shows on the top screen behind it, so the change is seen as it is made
		snprintf(text, sizeof(text), "%.2f", g_gammaPct / 100.0f);
		bar(50, "Gamma", text, g_gammaPct, kGammaMinPct, kGammaMaxPct, kGammaStepPct);
		float g = gammaValue();
		for (int i = 0; i < 10; i++)
		{
			int c = (int)(powf(i / 9.0f, 1.0f / g) * 255.0f + 0.5f);
			uiRect(14 + i * 29.2f, 88, 29.2f, 12, C2D_Color32(c, c, c, 255));
		}
		uiFrame(14, 88, 292, 12);
		// Auto gamma: what this console reads the light with
		GammaSensor sensor = gammaSensor();
		if (choice(106, "Auto gamma", sensor == SENSOR_NONE ? "No light sensor" : onOff(gammaAutoOn()), sensor != SENSOR_NONE))
		{
			g_autoGamma = gammaAutoOn() ? 0 : 1;
			notify(!gammaAutoOn() ? "Auto gamma off." : sensor == SENSOR_QTM
				? "Auto gamma on: the picture lifts in bright light."
				: "Auto gamma on: the inner camera checks the light at each loading screen.");
		}
		const char* how = sensor == SENSOR_QTM ? "Auto gamma checks the light every few seconds."
			: sensor == SENSOR_CAMERA ? "Auto gamma uses the inner camera at loads."
			: "No light sensor: Auto gamma stays off.";
		// View distance (outdoors; OpenMW's 7168 at the most): Max, the console's longest whatever the speed; Min, its
		// shortest; Mid halfway; Auto between them with the frame rate (a New 3DS from the longest, closing in when slow;
		// an Old 3DS from the shortest, opening out while there is room). A tap goes Auto, Max, Mid, Min
		static const char* kViewNames[4] = { "Min", "Auto", "Max", "Mid" };
		if (choice(130, "View distance", kViewNames[viewMode]))
		{
			viewMode = viewMode == VIEW_AUTO ? VIEW_MAX : viewMode == VIEW_MAX ? VIEW_MID : viewMode == VIEW_MID ? VIEW_MIN : VIEW_AUTO;
			changed = true;
			notify(viewMode == VIEW_MAX ? "View distance Max: always as far as this console sees, however slow."
				: viewMode == VIEW_MID ? "View distance Mid: halfway, whatever the speed."
				: viewMode == VIEW_MIN ? "View distance Min: the nearest fog, the smoothest frames."
				: "View distance Auto: the fog moves with the frame rate.");
		}
		// Depth buffer: 24-bit, 16-bit while 3D is on (half the depth traffic of two eyes), or 16-bit always. A tap cycles
		static const char* kDepthNames[3] = { "24-bit", "16-bit in 3D", "16-bit" };
		if (choice(154, "Depth buffer", kDepthNames[g_depthMode]))
		{
			g_depthMode = (g_depthMode + 1) % 3;
			changed = true;
			notify(g_depthMode == DEPTH_24 ? "Depth buffer 24-bit." : g_depthMode == DEPTH_16_IN_3D
				? "Depth buffer 16-bit while 3D is on: lighter frames in 3D." : "Depth buffer 16-bit: far surfaces may flicker.");
		}
		// (the rows close up so the two hint lines clear the buttons)
		uiText(10, 180, 0.4f, col::textDim, how);
		if (gammaAutoOn())
			snprintf(text, sizeof(text), "Light now: x%.2f, gamma in use %.2f", gammaLift(), g);
		else
			snprintf(text, sizeof(text), "Gamma 1.00 is Morrowind's own; higher helps in sun.");
		uiText(10, 192, 0.4f, col::textDim, text);
		b = buttons(208, 28, { "Default", w.game.gmstLabel("sclose", "Close") });
		closing = b == 1;
		if (b == 0)
		{
			g_gammaPct = 100;
			g_autoGamma = -1;
			viewMode = VIEW_AUTO;
			g_depthMode = DEPTH_16_IN_3D;
			changed = true;
		}
	}
	else
	{
		// Look speed (the C-stick, the D-pad and the GoldenEye pad while aiming: one bar, they were two) and the GoldenEye
		// pad's turn while walking (0.50x to 2.00x), the sticks' dead zone (10% to 35% of a full push: a stick that rests
		// a little off centre kept walking or turning the view), Invert and Gyro aim
		snprintf(text, sizeof(text), "%.2fx", g_lookSpeed / 100.0f);
		bar(50, "Look speed (sticks, D-pad, aiming)", text, g_lookSpeed, kLookMinPct, kLookMaxPct, kLookStepPct);
		snprintf(text, sizeof(text), "%.2fx", g_geTurnSpeed / 100.0f);
		bar(84, "GoldenEye turn (walking)", text, g_geTurnSpeed, kGeTurnMinPct, kGeTurnMaxPct, kLookStepPct);
		snprintf(text, sizeof(text), "%d%%", g_stickZonePct);
		bar(118, "Stick dead zone", text, g_stickZonePct, kStickZoneMinPct, kStickZoneMaxPct, kStickZoneStepPct);
		if (choice(154, "Invert look", onOff(g_invertY)))
		{
			g_invertY = !g_invertY;
			notify(g_invertY ? "Up on the stick now looks down." : "Up on the stick looks up.");
		}
		// Gyro aim: turning the console turns the view (off, half, as far, twice as far as it turned)
		static const int kGyroSteps[4] = { 0, 50, 100, 200 };
		snprintf(text, sizeof(text), g_gyroAim > 0 ? "%.1fx" : "Off", g_gyroAim / 100.0f);
		if (int d = choice(180, "Gyro aim", text))
		{
			int k = 0;
			while (k < 3 && kGyroSteps[k] != g_gyroAim)
				k++;
			g_gyroAim = kGyroSteps[(k + d + 4) % 4];
			// (set to a speed, the gyroscope's offsets are measured first, the console laid down: GyroRest::calibrate.
			// Choosing a speed again calibrates again)
			extern bool g_gyroCalibrate;
			g_gyroCalibrate = g_gyroAim > 0;
			notify(g_gyroAim > 0 ? "Gyro aim on: lay the 3DS down for 2 seconds to calibrate, then turn it to aim." : "Gyro aim off.");
		}
		// Layout: the button list, what each button does in the layout in use (Y there switches Xbox, classic, GoldenEye)
		b = buttons(208, 28, { "Default", "Layout", w.game.gmstLabel("sclose", "Close") });
		closing = b == 2;
		if (b == 0)
		{
			g_lookSpeed = kLookDefaultPct;
			g_geTurnSpeed = 100;
			g_stickZonePct = kStickZoneDefaultPct;
			g_invertY = false;
			g_gyroAim = 0;
			changed = true;
		}
		else if (b == 1)
		{
			openScreen(SCR_CONTROLS);
			return;
		}
	}
	if (changed)
	{
		audioSetScales(effectsVolume / 100.0f, musicVolume / 100.0f);
		saveSettings();
	}
	if (closing || (in.down & KEY_B))
	{
		closeScreen();
		return;
	}
	// D-pad up / down: the line above / below (keeping the column, as near as the line has); left / right between
	// buttons side by side (a setting's own line takes them itself)
	int li = 0;
	while (li + 1 < (int)lines.size() && focus >= lines[li + 1].first)
		li++;
	int column = focus - lines[li].first, n = lines.size();
	if (in.down & (KEY_DOWN | KEY_UP))
	{
		li = (li + ((in.down & KEY_DOWN) ? 1 : n - 1)) % n;
		focus = lines[li].first + std::min(column, lines[li].count - 1);
	}
	else if (lines[li].count > 1 && (in.down & (KEY_LEFT | KEY_RIGHT)))
		focus = lines[li].first + (column + ((in.down & KEY_RIGHT) ? 1 : lines[li].count - 1)) % lines[li].count;
}

// What each button does
void Session::drawControls()
{
	bool xbox = g_controlLayout == CONTROLS_XBOX, goldenEye = g_controlLayout == CONTROLS_GOLDENEYE;
	header(xbox ? "Controls (Xbox layout)" : goldenEye ? "Controls (GoldenEye layout)" : "Controls (classic layout)");
	static const char* kXbox[][2] = {
		{ "Circle Pad / C-Stick", "Move (pushed far: run) / look" }, { "A", "Activate: talk, open, take" },
		{ "R (hold, release)", "Wind up, strike" }, { "X", "Ready / put away the weapon" }, { "L", "Cast the selected spell" },
		{ "Y", "Jump (swim up in water)" }, { "ZL (hold)", "Sneak" },
		{ "D-Pad", "Quick keys" }, { "SELECT", "First / third person" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Inventory, magic, map, stats" } };
	static const char* kClassic[][2] = {
		{ "Circle Pad", "Move (pushed far: run)" }, { "C-Stick / D-Pad", "Look" }, { "A", "Activate: talk, open, take" },
		{ "X (hold, release)", "Ready weapon, wind up, strike" }, { "ZL + X", "Put the weapon away" }, { "Y", "Cast the selected spell" },
		{ "B", "Jump (swim up in water)" }, { "L (hold)", "Sneak" },
		{ "ZL + D-Pad", "Quick keys" }, { "SELECT", "First / third person" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Inventory, magic, map, stats" } };
	// One stick (an Old 3DS without the C-stick) on the Xbox buttons: the pad walks and turns; an aim button held
	// stands still and the pad aims
	static const char* kGoldenEye[][2] = {
		{ "Circle Pad", "Walk (far: run) up, down; turn" }, { "A (hold) / ZL / ZR", "Aim: stand still, the pad aims" },
		{ "A (tap)", "Activate: talk, open, take" }, { "R (hold, release)", "Wind up, strike" },
		{ "L", "Cast the selected spell" }, { "X", "Ready / put away the weapon" },
		{ "Y", "Jump (swim up in water)" }, { "B", "Sneak on / off" }, { "SELECT", "First / third person" },
		{ "D-Pad", "Left / right: strafe; up / down: look" }, { "START", "Menu: save, rest, options" },
		{ "Touch screen", "Menus, quick keys" } };
	float y = 30;
	int n = xbox ? sizeof(kXbox) / sizeof(kXbox[0]) : goldenEye ? sizeof(kGoldenEye) / sizeof(kGoldenEye[0])
		: sizeof(kClassic) / sizeof(kClassic[0]);
	for (int i = 0; i < n; i++)
	{
		const char* const* l = xbox ? kXbox[i] : goldenEye ? kGoldenEye[i] : kClassic[i];
		uiText(14, y, 0.42f, col::header, l[0]);
		uiText(140, y, 0.42f, col::text, l[1]);
		y += 13.5f;
	}
	// Y: the next layout (Xbox, classic, GoldenEye); A / B: back to the Controls tab, on its Layout button
	if (uiButton(4, 208, 154, 28, xbox ? "Y: Use classic" : goldenEye ? "Y: Use Xbox" : "Y: Use GoldenEye") || (uiIn().down & KEY_Y))
	{
		g_controlLayout = xbox ? CONTROLS_CLASSIC : goldenEye ? CONTROLS_XBOX : CONTROLS_GOLDENEYE;
		saveSettings();
	}
	else if (uiButton(162, 208, 154, 28, w.game.gmst("sclose", "Close"), true) || (uiIn().down & (KEY_A | KEY_B)))
	{
		openScreen(SCR_LOOK);
		focus = kControlsButtonsFocus;
	}
}
