// The player's settings (settings.txt): controls, gyro, subtitles, read at start and written on change
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

static const char* kSettingsPath = "sdmc:/3ds/mw3ds/settings.txt";
int g_controlLayout = CONTROLS_XBOX;
int g_gyroAim = 0;                // gyro aim starts off (until it is tried on the device): Options turns it on
int g_lookSpeed = kLookDefaultPct, g_geTurnSpeed = 100;
int g_stickZonePct = kStickZoneDefaultPct;
bool g_invertY = false;
bool g_subtitles = true;
static int s_gyroSens = 100;      // gyro aim's sensitivity, kept while it is off

void Session::loadSettings()
{
	// An Old 3DS starts with the GoldenEye layout: one with no C-stick or ZL / ZR (those need a Circle Pad Pro there)
	if (g_profile.goldenEyeAtStart)
		g_controlLayout = CONTROLS_GOLDENEYE;
	// A scripted run starts at gamma 1.00 with Auto gamma off, whatever settings.txt a case before left (Azahar's SD card
	// keeps it), and has no light reading of its own (Azahar's and the PC's would be noise): GAMMA:, AUTOGAMMA:, LUX:,
	// CAMLUMA: change them. (A test reload keeps what the case set: main)
	if (autotest)
	{
		g_gammaPct = 100;
		g_autoGamma = 0;
		g_lookSpeed = g_geTurnSpeed = 100;
		g_stickZonePct = kStickZoneDefaultPct;
		g_invertY = false;
		g_subtitles = true;
	}
	FILE* f = fopen(kSettingsPath, "r");
	if (!f)
		return;
	bool gyroOff = false, gyroOn = false;
	char line[64];
	while (fgets(line, sizeof(line), f))
	{
		// (an older file's stereo= line is left unread: the 3D slider alone decides)
		if (strncmp(line, "difficulty=", 11) == 0) difficulty = atoi(line + 11);
		else if (strncmp(line, "effects=", 8) == 0) effectsVolume = atoi(line + 8);
		else if (strncmp(line, "music=", 6) == 0) musicVolume = atoi(line + 6);
		else if (strncmp(line, "controls=classic", 16) == 0) g_controlLayout = CONTROLS_CLASSIC;
		else if (strncmp(line, "combat=gbac", 11) == 0) combatMode = COMBAT_GBAC;
		else if (strncmp(line, "numbers=on", 10) == 0) hitIndicators = true;
		else if (strncmp(line, "numbers=off", 11) == 0) hitIndicators = false;
		else if (strncmp(line, "combat=vanilla", 14) == 0) combatMode = COMBAT_VANILLA;
		else if (strncmp(line, "controls=xbox", 13) == 0) g_controlLayout = CONTROLS_XBOX;
		else if (strncmp(line, "controls=goldeneye", 18) == 0) g_controlLayout = CONTROLS_GOLDENEYE;
		else if (strncmp(line, "gyroaim=off", 11) == 0) gyroOff = true;
		else if (strncmp(line, "gyroaim=on", 10) == 0) gyroOn = true;
		else if (strncmp(line, "gyrosens=", 9) == 0 && atoi(line + 9) > 0) s_gyroSens = atoi(line + 9);
		else if (autotest) {}
		else if (strncmp(line, "gamma=", 6) == 0)
			g_gammaPct = std::max(kGammaMinPct, std::min(kGammaMaxPct, atoi(line + 6)));
		else if (strncmp(line, "autogamma=on", 12) == 0) g_autoGamma = 1;
		else if (strncmp(line, "autogamma=off", 13) == 0) g_autoGamma = 0;
		else if (strncmp(line, "stickzone=", 10) == 0)
			g_stickZonePct = std::max(kStickZoneMinPct, std::min(kStickZoneMaxPct, atoi(line + 10)));
		else if (strncmp(line, "lookspeed=", 10) == 0) g_lookSpeed = std::max(kLookMinPct, std::min(kLookMaxPct, atoi(line + 10)));
		else if (strncmp(line, "geturn=", 7) == 0) g_geTurnSpeed = std::max(kLookMinPct, std::min(kLookMaxPct, atoi(line + 7)));
		// (an older file's geaim= is left unread: the GoldenEye aim looks at the look speed)
		else if (strncmp(line, "inverty=", 8) == 0) g_invertY = strncmp(line + 8, "on", 2) == 0;
		else if (strncmp(line, "subtitles=", 10) == 0) g_subtitles = strncmp(line + 10, "off", 3) != 0;
		else if (strncmp(line, "dynview=", 8) == 0) dynamicView = strncmp(line + 8, "off", 3) != 0;
	}
	fclose(f);
	// Gyro aim as it was left, at its sensitivity (it was turned off at every start while untried on the device, and
	// the setting seemed not to be kept). A scripted run starts with it off
	g_gyroAim = gyroOn && !gyroOff && !autotest ? s_gyroSens : 0;
}

void Session::saveSettings()
{
	FILE* f = fopen(kSettingsPath, "w");
	if (!f)
		return;
	fprintf(f, "difficulty=%d\neffects=%d\nmusic=%d\n", difficulty, effectsVolume, musicVolume);
	fprintf(f, "controls=%s\n", g_controlLayout == CONTROLS_CLASSIC ? "classic" : g_controlLayout == CONTROLS_GOLDENEYE ? "goldeneye" : "xbox");
	if (g_gyroAim > 0)
		s_gyroSens = g_gyroAim;
	fprintf(f, "gyroaim=%s\ngyrosens=%d\n", g_gyroAim > 0 ? "on" : "off", s_gyroSens);
	fprintf(f, "combat=%s\n", combatMode == COMBAT_GBAC ? "gbac" : "vanilla");
	fprintf(f, "numbers=%s\n", hitIndicators ? "on" : "off");
	fprintf(f, "gamma=%d\n", g_gammaPct);
	if (g_autoGamma >= 0)                // (not chosen: the console's default, on with QTM)
		fprintf(f, "autogamma=%s\n", g_autoGamma ? "on" : "off");
	fprintf(f, "lookspeed=%d\ngeturn=%d\ninverty=%s\n", g_lookSpeed, g_geTurnSpeed, g_invertY ? "on" : "off");
	fprintf(f, "stickzone=%d\n", g_stickZonePct);
	fprintf(f, "subtitles=%s\n", g_subtitles ? "on" : "off");
	fprintf(f, "dynview=%s\n", dynamicView ? "on" : "off");
	fclose(f);
}
