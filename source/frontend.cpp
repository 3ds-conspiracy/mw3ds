// Loading screens, the logos and the title screen (frontend.h)
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "audio.h"
#include "distant.h"
#include "linear.h"
#include "memstat.h"
#include "log.h"
#include "player.h"
#include "gamma.h"
#include "renderer.h"
#include "screenshot.h"
#include "testdrive.h"
#include "zfile.h"
#include "saves.h"
#include "session.h"
#include "ui.h"
#include "platform.h"
#include "controls.h"
#include "frontend.h"

// Clears the 3D screen's target to an 0xRRGGBBAA colour. The GPU's fill writes the value's low bits as they
// are, so the Old 3DS's 16-bit target (console.h rgb565) takes the colour packed as RGB565: an RGBA8 black
// fills it blue.
void clearTop(C3D_RenderTarget* target, u32 rgba)
{
	if (g_profile.rgb565)
		rgba = ((rgba >> 27) & 0x1F) << 11 | ((rgba >> 18) & 0x3F) << 5 | ((rgba >> 11) & 0x1F);
	C3D_RenderTargetClear(target, C3D_CLEAR_ALL, rgba, 0);
}

// Loading screens show one of Morrowind's splash pictures (a new one for each load)
std::vector<ArtRef> g_splashes;
static std::string s_splashFor;
static int s_splash = -1;

void drawLoading(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* text,
	const char* detail, float seconds)
{
	// (marked: the Old 3DS froze on hardware right after New game, before anything the load logs)
	const char* savedAt = g_mainAt;
	UiInput none = {};
	MARK("loading screen: ui");
	uiBeginFrame(none);
	MARK("loading screen: frame begin");
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	MARK("loading screen: draw");
	linearRetire();
	C2D_Prepare();
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	clearTop(top, 0x000000FF);
	C2D_SceneBegin(top);
	if (!g_splashes.empty())
	{
		if (s_splashFor != text)
		{
			s_splashFor = text;
			s_splash = rand() % g_splashes.size();
		}
		const ArtRef& a = g_splashes[s_splash];
		if (uiArt(a.file, a.w, a.h, 0, 0, 400, 240))
		{
			uiRect(0, 204, 400, 36, 0xA0000000);
			uiTextCentered(200, 212, 0.5f, col::text, text);
		}
		else
		{
			uiTextCentered(200, 100, 0.8f, col::header, "Morrowind");
			uiTextCentered(200, 132, 0.5f, col::textDim, text);
		}
	}
	else
	{
		uiTextCentered(200, 100, 0.8f, col::header, "Morrowind");
		uiTextCentered(200, 132, 0.5f, col::textDim, text);
	}
	C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
	C2D_SceneBegin(bottom);
	uiTextCentered(160, 40, 0.5f, col::textDim, "MW3DS");
	// What the loader is doing (its last log line), so a hang shows where it stopped
	if (seconds >= 0.0f)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.0f s", seconds);
		uiTextCentered(160, 80, 0.45f, col::textDim, buf);
	}
	if (detail)
	{
		float y = 110;
		for (auto& line : uiWrap(detail, 300, 0.42f))
		{
			uiText(10, y, 0.42f, col::text, line);
			y += uiLineHeight(0.42f);
			if (y > 220)
				break;
		}
	}
	MARK("loading screen: frame end");
	C3D_FrameEnd(0);
	g_mainAt = savedAt;
}

// While a load runs, each log line the main thread writes redraws the loading screen with it
// (a few times a second at most)
static struct
{
	C3D_RenderTarget *top, *bottom;
	std::string title;
	u64 start, drawn;
	u32 mainThread;
	std::string last;
} s_progress;

static void loadingProgress(const char* line)
{
	u32 id = 0;
	svcGetThreadId(&id, CUR_THREAD_HANDLE);
	if (id != s_progress.mainThread)
		return;
	s_progress.last = line;
	u64 now = osGetTime();
	if (now - s_progress.drawn < 150)
		return;
	s_progress.drawn = now;
	drawLoading(s_progress.top, s_progress.bottom, s_progress.title.c_str(), line, (now - s_progress.start) / 1000.0f);
}

// The game's own font and frames for the logos and the title screen: game.json's "ui" alone, before the game loads
// (it is loaded again with the rest). Without it they write in the 3DS's system font
void loadStartupTheme(const char* dataDir)
{
	UiThemeDef t;
	if (gameLoadUiTheme((std::string(dataDir) + "/game.json").c_str(), t))
		uiLoadTheme(dataDir, t);
}

// The title music (OpenMW's titleMusic), from the converter's fixed name: started once, for the Morrowind logo and the
// title screen after it (game.json isn't read yet)
static bool s_titleMusicOn = false;
void startTitleMusic(const char* dataDir)
{
	struct stat st;
	if (s_titleMusicOn)
		return;
	if (stat((std::string(dataDir) + "/music/music_special_morrowind_title.snd").c_str(), &st) != 0)
	{
		logf("title music: not in %s/music", dataDir);
		return;
	}
	logf("title music: playing");
	audioMusicPlay({ "music_special_morrowind_title.snd" }, 0.3f, false);   // (once: OpenMW streams it once, then the menu is quiet)
	s_titleMusicOn = true;
}

// The Morrowind logo's last frame, kept for the title screen to dissolve from: the two pictures are drawn differently
// (the movie's emblem is larger), so a cut between them looked like a zoom out even with the lettering matched
struct StartupFrame { C3D_Tex tex; u32 w = 0, h = 0; float zoom = 1.0f; bool ok = false; };
static StartupFrame s_lastFrame;
static const int kTitleDissolveFrames = 40;     // (2/3 s at the screen's 60 Hz)

// A movie at startup, before any game is loaded (Session::playMovie needs one): the converter's .mwv (u32 frames,
// fps x 100, width, height; then each frame's size and its .t3x) on the top screen, its sound (music/movie_<name>.snd)
// on the music channel when it has one, zoomed `zoom` times about the middle. A, B, START or a tap skips it. OpenMW's startup: the company logo
// (Movies_Company_Logo), then the main menu's title music with the Morrowind logo over it (Movies_Morrowind_Logo)
void startupMovie(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* dataDir, const char* name,
	float zoom, bool keepLast)
{
	FILE* f = fopen((std::string(dataDir) + "/movies/" + name + ".mwv").c_str(), "rb");
	if (!f)
		return;
	char magic[4];
	u32 hdr[4];
	if (fread(magic, 4, 1, f) != 1 || memcmp(magic, "MWV1", 4) != 0 || fread(hdr, 4, 4, f) != 4 || !hdr[2] || !hdr[3])
	{
		fclose(f);
		return;
	}
	u32 frames = hdr[0], fps100 = hdr[1] ? hdr[1] : 1000, mw = hdr[2], mh = hdr[3];
	struct stat st;
	std::string snd = std::string("movie_") + name + ".snd";
	if (stat((std::string(dataDir) + "/music/" + snd).c_str(), &st) == 0)
	{
		audioMusicPlay({ snd }, 0.8f);
		s_titleMusicOn = false;              // (the music channel is the movie's now)
	}
	logf("startup movie: %s, %u frames", name, (unsigned)frames);
	C3D_Tex tex;
	bool texOk = false;
	int shown = -1;
	u64 start = osGetTime();
	u32 loops = 0;
	(void)start;
	(void)loops;
	while (aptMainLoop())
	{
		hidScanInput();
		if (hidKeysDown() & (KEY_A | KEY_B | KEY_START | KEY_TOUCH))
			break;
#ifdef __3DS__
		int want = (int)((osGetTime() - start) * fps100 / 100000);
#else
		int want = (int)(loops++ * fps100 / 3000);       // (the PC build: a fixed 1/30 s a frame, as its recordings step)
#endif
		if (want >= (int)frames)
			break;
		bool ended = false;
		while (shown < want)
		{
			u32 size = 0;
			if (fread(&size, 4, 1, f) != 1 || size > (1 << 20))
			{
				ended = true;
				break;
			}
			shown++;
			if (shown < want)
			{
				fseek(f, size, SEEK_CUR);
				continue;
			}
			std::vector<u8> buf(size);
			if (fread(buf.data(), 1, size, f) != size)
			{
				ended = true;
				break;
			}
			LinearGuard guard;
			if (texOk)
				deferredTexDelete(&tex);
			Tex3DS_Texture t = Tex3DS_TextureImport(buf.data(), size, &tex, nullptr, false);
			texOk = t != nullptr;
			if (t)
			{
				Tex3DS_TextureFree(t);
				C3D_TexSetFilter(&tex, GPU_LINEAR, GPU_LINEAR);
			}
		}
		if (ended)
			break;
		audioUpdate();
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		linearRetire();
		C2D_Prepare();
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		clearTop(top, 0x000000FF);
		C2D_SceneBegin(top);
		if (texOk)
		{
			Tex3DS_SubTexture sub = { (u16)mw, (u16)mh, 0.0f, 1.0f, (float)mw / tex.width, 1.0f - (float)mh / tex.height };
			C2D_Image img = { &tex, &sub };
			// The whole width, cut at the top and bottom (the logos are 4:3): fitted to the height they stood 320 wide in
			// black bars, and the title screen's picture after the Morrowind logo filled all 400 of a sudden
			float k = fmaxf(400.0f / mw, 240.0f / mh) * zoom;
			C2D_DrawImageAt(img, (400.0f - mw * k) / 2.0f, (240.0f - mh * k) / 2.0f, 0.5f, nullptr, k, k);
		}
		C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 255));
		C2D_SceneBegin(bottom);
		uiTextCentered(160, 110, 0.45f, col::textDim, "A: skip");
		C3D_FrameEnd(0);
	}
	fclose(f);
	if (texOk && keepLast)
	{
		s_lastFrame.tex = tex;
		s_lastFrame.w = mw;
		s_lastFrame.h = mh;
		s_lastFrame.zoom = zoom;
		s_lastFrame.ok = true;
	}
	else if (texOk)
	{
		LinearGuard guard;
		deferredTexDelete(&tex);
	}
	// (a skipped movie's sound gives way to the title music, which takes the music channel next)
}

// Title screen: new game or any save. Returns the save path, "" for a new game, "quit" on START. notice: shown under
// the title (a save that wouldn't read). showFrames > 0: shown that many frames whatever the saves, then "" (MW3DS_INTRO)
std::string titleScreen(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const char* dataDir,
	const char* notice, int showFrames)
{
	std::vector<SaveInfo> saves = savesList(dataDir);
	if (saves.empty() && showFrames <= 0)
		return "";
	UiList list;
	int focus = 1;
	bool wasTouching = false, dragged = false;
	int lastX = 0, lastY = 0, pressX = 0, pressY = 0;
	// Morrowind's own main menu (OpenMW's MainMenu): its picture on the top screen and the title music, from the
	// converter's fixed names (game.json isn't read yet). Without them the title is plain text, as before
	uiArtDir(dataDir);
	startTitleMusic(dataDir);
	struct stat st;
	// Test hook (run-emu.ps1 without -Cams/-Inputs): screenshot the title screen, then load the first save
	bool shotTest = stat("sdmc:/3ds/mw3ds/autoshot", &st) == 0;
	int frame = 0, dissolve = 0;
	u64 lastTick = 0;
	while (aptMainLoop())
	{
		audioUpdate();                       // (the title music streams: nothing else feeds it before the game runs)
		if (showFrames > 0 && ++frame >= showFrames)
			return "";
		if (shotTest && ++frame == 60)
		{
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			linearRetire();
			logf("title shot: %s", screenshotSave("sdmc:/3ds/mw3ds/shot_title.bmp") ? "saved" : "FAILED");
			C3D_FrameEnd(0);
			return saves[0].path;
		}
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START)
			return "quit";
		UiInput ui = {};
		touchPosition tp;
		hidTouchRead(&tp);
		if (held & KEY_TOUCH)
		{
			ui.touching = true;
			ui.touchX = tp.px;
			ui.touchY = tp.py;
			if (wasTouching)
			{
				ui.dragDX = tp.px - lastX;
				ui.dragDY = tp.py - lastY;
				if (abs(tp.px - pressX) + abs(tp.py - pressY) > kTapSlop)
					dragged = true;
			}
			else
			{
				dragged = false;
				pressX = tp.px;
				pressY = tp.py;
			}
			lastX = tp.px;
			lastY = tp.py;
		}
		else if (wasTouching && !dragged)
		{
			ui.tapped = true;
			ui.tapX = lastX;
			ui.tapY = lastY;
		}
		wasTouching = held & KEY_TOUCH;
		ui.down = down;
		ui.held = held;
		ui.stickY = readStickScroll();
		ui.stickX = readStickTurn();
		u64 tick = svcGetSystemTick();
		ui.dt = lastTick ? fminf((float)(tick - lastTick) / SYSCLOCK_ARM11, 0.1f) : 0.0f;
		lastTick = tick;
		uiBeginFrame(ui);

		std::string result = "\x01";
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		linearRetire();
		C2D_Prepare();
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		clearTop(top, 0x000000FF);
		C2D_SceneBegin(top);
		bool art = uiArt("art/menu_morrowind.t3x", 400, 240, 0, 0, 400, 240);
		// the Morrowind logo's last frame dissolves into the picture
		if (s_lastFrame.ok)
		{
			int d = ++dissolve;
			if (d < kTitleDissolveFrames && art)
			{
				const StartupFrame& lf = s_lastFrame;
				Tex3DS_SubTexture sub = { (u16)lf.w, (u16)lf.h, 0.0f, 1.0f, (float)lf.w / lf.tex.width,
					1.0f - (float)lf.h / lf.tex.height };
				C2D_Image img = { const_cast<C3D_Tex*>(&lf.tex), &sub };
				C2D_ImageTint tint;
				C2D_AlphaImageTint(&tint, 1.0f - (float)d / kTitleDissolveFrames);
				float k = fmaxf(400.0f / lf.w, 240.0f / lf.h) * lf.zoom;
				C2D_DrawImageAt(img, (400.0f - lf.w * k) / 2.0f, (240.0f - lf.h * k) / 2.0f, 0.5f, &tint, k, k);
			}
			else
			{
				LinearGuard guard;
				deferredTexDelete(&s_lastFrame.tex);
				s_lastFrame.ok = false;
			}
		}
		if (art)
		{
			uiRect(0, notice ? 186 : 208, 400, notice ? 54 : 32, 0xA0000000);
			if (notice)
				uiTextCentered(200, 192, 0.5f, col::health, notice);
			// (in Morrowind's font the line is wider than the screen at 0.5: as large as fits)
			static const char* kHint = "Choose a save on the touch screen, or start a new game";
			float hs = 0.5f;
			while (hs > 0.3f && uiTextWidth(kHint, hs) > 388.0f)
				hs -= 0.02f;
			uiTextCentered(200, 216, hs, col::text, kHint);
		}
		else
		{
			uiTextCentered(200, 80, 0.9f, col::header, "Morrowind");
			uiTextCentered(200, 120, 0.5f, col::textDim, "Choose a save on the touch screen, or start a new game");
			if (notice)
				uiTextCentered(200, 150, 0.5f, col::health, notice);
		}
		C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
		C2D_SceneBegin(bottom);
		uiRect(0, 0, 320, 20, col::panelLight);
		uiRect(0, 20, 320, 1, col::border);
		uiText(6, 2, 0.55f, col::header, "Load game");
		int pick = uiSaveList(list, saves, 4, 24, 312, 176);
		if (down & KEY_RIGHT) focus = (focus + 1) % 2;
		if (down & KEY_LEFT) focus = (focus + 1) % 2;
		if (uiButton(4, 208, 154, 28, "New game", focus == 0))
			result = "";
		if (uiButton(162, 208, 154, 28, "Load", focus == 1) || (pick >= 0 && (down & KEY_A) && focus == 1))
			result = saves[list.selected].path;
		C3D_FrameEnd(0);
		if (result != "\x01")
			return result;
	}
	return "quit";
}

// The largest texture edge the data was converted with (level.py --max-tex, game.json "max_tex"); 0 for
// data converted before it was recorded. game.json is small: its big sections are in game_<name>.json. (It is
// compressed, MWZ1: read as raw bytes the key was never found, and this said 0 whatever the data)
int dataMaxTex(const char* dataDir)
{
	std::string path = std::string(dataDir) + "/game.json";
	char* text = zreadAll(path.c_str());
	if (!text)
		return 0;
	const char* at = strstr(text, "\"max_tex\":");
	int v = at ? atoi(at + 10) : 0;
	free(text);
	return v;
}

// An Old 3DS given the New 3DS's data (build\mw3ds.cia rather than build\mw3ds-o3ds.cia): its textures do
// not fit the Old 3DS's memory and the first load stops on a black screen. Says which copy to install and
// waits for START (scripted runs save a picture and go on to quit).
void wrongConsoleScreen(C3D_RenderTarget* top, C3D_RenderTarget* bottom, int maxTex)
{
	static const char* kLines[] = {
		"This copy of MW3DS was made for the New 3DS.",
		"",
		"Its textures need more memory than an Old 3DS",
		"or 2DS has, so the game would stop on a black",
		"screen while loading.",
		"",
		"Install the Old 3DS copy instead. On the",
		"old-3ds branch, build it with",
		"tools\\build\\build-cia-mods.ps1 -Old3ds",
		"and install build\\mw3ds-o3ds.cia.",
	};
	char detail[64];
	snprintf(detail, sizeof(detail), "textures up to %d texels (an Old 3DS takes %d)", maxTex, kOld3dsMaxTex);
	struct stat st;
	bool scripted = stat("sdmc:/3ds/mw3ds/autoinput.txt", &st) == 0 || stat("sdmc:/3ds/mw3ds/autocam.txt", &st) == 0
		|| stat("sdmc:/3ds/mw3ds/autoshot", &st) == 0;
	for (int frame = 1; aptMainLoop(); frame++)
	{
		hidScanInput();
		if (hidKeysDown() & KEY_START)
			return;
		UiInput none = {};
		uiBeginFrame(none);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		linearRetire();
		bool shot = scripted && frame == 60;
		if (shot)
			logf("wrong console shot: %s", screenshotSave("sdmc:/3ds/mw3ds/shot_console.bmp") ? "saved" : "FAILED");
		C2D_Prepare();
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		clearTop(top, 0x000000FF);
		C2D_SceneBegin(top);
		uiTextCentered(200, 24, 0.7f, col::header, "Wrong copy for this console");
		for (size_t i = 0; i < sizeof(kLines) / sizeof(kLines[0]); i++)
			uiTextCentered(200, 64 + 15 * i, 0.5f, i >= 8 ? col::header : col::text, kLines[i]);
		C2D_TargetClear(bottom, C2D_Color32(10, 8, 6, 255));
		C2D_SceneBegin(bottom);
		uiTextCentered(160, 40, 0.5f, col::textDim, "MW3DS");
		uiTextCentered(160, 110, 0.5f, col::text, "Press START to quit");
		uiTextCentered(160, 200, 0.4f, col::textDim, detail);
		C3D_FrameEnd(0);
		if (shot)
			return;
	}
}

LoadingProgress::LoadingProgress(C3D_RenderTarget* top, C3D_RenderTarget* bottom, const std::string& title)
{
	s_progress.top = top;
	s_progress.bottom = bottom;
	s_progress.title = title;
	s_progress.start = s_progress.drawn = osGetTime();
	svcGetThreadId(&s_progress.mainThread, CUR_THREAD_HANDLE);
	logSetHook(loadingProgress);
	gammaLoadStart();                // (the camera reading, where it is the light sensor: hidden by the load)
}

// The last step, whatever the throttle skipped, and that the first frame comes next: if the
// screen stays like this, the first frame hung (the GPU)
LoadingProgress::~LoadingProgress()
{
	gammaLoadEnd();
	logSetHook(nullptr);
	std::string text = s_progress.last + "\n\nLoaded. Starting the first frame...";
	drawLoading(s_progress.top, s_progress.bottom, s_progress.title.c_str(), text.c_str(),
		(osGetTime() - s_progress.start) / 1000.0f);
}
