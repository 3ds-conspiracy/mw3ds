#pragma once

#include <3ds.h>
#include <citro3d.h>
#include <string>
#include <vector>

#include "mwworld/player.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/widgets.hpp"

// The Morrowind logo's last frame frames the logo looser than the title screen's picture after it: 1.2 times closer
// they match (fitted on the two, frame against picture), and the logo doesn't jump in size as the title comes
static const float kMwLogoZoom = 1.2f;
// The largest texture edge an Old 3DS takes (build-cia.ps1 -Old3ds converts with --max-tex 128)
static const int kOld3dsMaxTex = 128;

// The loading screens' splash pictures
extern std::vector<ArtRef> g_splashes;

// Clears the 3D screen's target to an 0xRRGGBBAA colour (packed RGB565 on an Old 3DS's 16-bit target)
void clearTop(C3D_RenderTarget* target, u32 rgba);
// A loading screen: a splash picture, the text, what is being read and for how long
void drawLoading(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* text, const char* detail = nullptr,
	float seconds = -1.0f);

// While it lives, each log line the main thread writes redraws the loading screen with it (a few times a second at
// most); its end draws the last step and that the first frame comes next
struct LoadingProgress
{
	LoadingProgress(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const std::string& title);
	~LoadingProgress();
};

// The game's own font and frames for the logos and the title screen, before the game loads
void loadStartupTheme(const char* dataDir);
// The title music, started once for the logos and the title screen
void startTitleMusic(const char* dataDir);
// One of the start-up movies (the company logo, the Morrowind logo), skippable
void startupMovie(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* dataDir, const char* name, float zoom = 1.0f,
	bool keepLast = false);
// Title screen: new game or any save. Returns the save path, "" for a new game, "quit" on START
std::string titleScreen(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* dataDir, const char* notice = nullptr,
	int showFrames = 0);
// The largest texture edge the data was converted with (0: not recorded)
int dataMaxTex(const char* dataDir);
// Data converted for the other console: what to do
void wrongConsoleScreen(C3D_RenderTarget* top, C3D_RenderTarget* bottom, int maxTex);
