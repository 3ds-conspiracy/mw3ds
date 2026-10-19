#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <string>
#include <vector>

#include "uitheme.h"

// Morrowind's palette (Morrowind.ini [Font Color] once a theme is loaded)
namespace col
{
	extern u32 panel, panelLight, border, text, textOver, textPressed, textDim, header, link, select;
	extern u32 health, magicka, fatigue, count, black;
}

struct UiInput
{
	u32 down, held, up;
	bool touching;        // finger on the screen this frame
	bool tapped;          // finger lifted this frame without dragging: tap at (tapX, tapY)
	int touchX, touchY, tapX, tapY;
	int dragDX, dragDY;   // drag since last frame
	float stickY;         // the C-stick up / down, -1 (up) to 1 (down): scrolls a text pane
	float dt;             // the frame's step (seconds): what moves on its own (a long row sliding)
};

// How far the C-stick (-1..1) scrolls a text pane this frame
float uiStickScroll();

void uiInit();
// Morrowind's font, window frames and item icons from the converted data (see uitheme.h)
void uiLoadTheme(const char* dataDir, const UiThemeDef& def);
void uiCheckTheme();          // monitor: the font's and frames' textures still hold what was loaded
bool uiThemeIntact();         // ... and the same as a test check
void uiFreeTheme();
void uiExit();
void uiBeginFrame(const UiInput& in);     // clears the per-frame text buffer
const std::vector<std::string>& uiLastButtons();    // the labels of the buttons the last whole frame drew
const UiInput& uiIn();
void uiConsume(u32 keys);                 // keys handled by gameplay are hidden from widgets this frame

// Text (system font). scale 0.5 ~ 15 px lines.
float uiLineHeight(float scale);
float uiTextWidth(const std::string& s, float scale);
// The game font's own glyph for a character (not the '?' a missing one is drawn as): where it is in the font's picture
// and its advance; false when the font has none
bool uiFontGlyph(unsigned cp, int& x, int& y, float& advance);
std::vector<std::string> uiWrap(const std::string& text, float width, float scale);
void uiText(float x, float y, float scale, u32 color, const std::string& s);
// A picture from data/art (loaded once, kept): its own size (w, h) inside the texture, drawn at x, y scaled
// to dw x dh (0: its own size). False when it can't be read.
bool uiArt(const std::string& file, int w, int h, float x, float y, float dw = 0, float dh = 0);
void uiArtDir(const char* dataDir);   // the folder uiArt reads from before a theme is loaded (the title screen)
void uiArtFree();
void uiTextCentered(float cx, float y, float scale, u32 color, const std::string& s);
void uiTextRight(float rx, float y, float scale, u32 color, const std::string& s);

// Shapes
void uiRect(float x, float y, float w, float h, u32 color);
void uiPanel(float x, float y, float w, float h);
// Window frame from the theme's 9-slice pieces ("menu_thin_border", "menu_button_frame", ...);
// a plain outline when the theme has none
void uiFrame(float x, float y, float w, float h, const char* style = "menu_thin_border", u32 fallback = 0);
bool uiHasPiece(const char* name);
void uiPiece(const char* name, float x, float y, float w, float h);   // stretched to w x h
// Centred on (cx, cy), turned clockwise by angle (radians); false when the theme lacks the piece
bool uiPieceRotated(const char* name, float cx, float cy, float w, float h, float angle);
void uiCaption(const std::string& title, const std::string& note = "");   // window title bar across the top
// Item icon (GameData object iconIx) at size x size; false when there is none
bool uiIcon(int icon, float x, float y, float size = 32.0f);
void uiBar(float x, float y, float w, float h, float frac, u32 color);

// Widgets: return true when activated (tap, or A while `focused`)
bool uiButton(float x, float y, float w, float h, const std::string& label, bool focused = false, bool enabled = true);
bool uiHit(float x, float y, float w, float h);          // tapped inside this rectangle

// Scrollable list of rows. Returns the tapped row, or -1. `selected` is highlighted and
// moved by D-pad up/down when `keys` is set; A activates it.
struct UiList
{
	float scroll = 0.0f;
	int selected = 0;
	int marqueeRow = -1;     // the row whose text slides (too long for its space) and for how long it has
	float marqueeTime = 0.0f;
};
// A list whose rows wrap onto more lines when they don't fit the width (dialogue topics and choices)
int uiWrapList(UiList& list, float x, float y, float w, float h, const std::vector<std::string>& rows,
	float scale, bool keys);
// right: a column of text at each row's right edge, always shown; the row's own text is cut short before it, and the
// selected row's slides left and comes round again when it doesn't fit
int uiList(UiList& list, float x, float y, float w, float h, const std::vector<std::string>& rows,
	float scale = 0.5f, bool keys = true, const std::vector<std::string>* right = nullptr);

// Grid of item icons, Morrowind inventory style: count in the corner, equipped / magic backgrounds.
// D-pad moves the selection, drag scrolls; returns the tapped cell (or the selected one on A), else -1.
enum { UIGRID_EQUIPPED = 1, UIGRID_MAGIC = 2, UIGRID_DIM = 4 };
struct UiGridItem { int icon; int count; u32 flags; std::string label; };   // label: shown when no icon
struct UiGrid
{
	float scroll = 0.0f;
	int selected = 0;
};
int uiItemGrid(UiGrid& grid, float x, float y, float w, float h, const std::vector<UiGridItem>& items, bool keys = true);

// Scrollable wrapped text box; drag or D-pad to scroll.
struct UiScroll { float scroll = 0.0f; };
void uiTextBox(UiScroll& s, float x, float y, float w, float h, const std::string& text, float scale = 0.5f,
	bool keys = true);
// A scrolling page of text whose spans are links (the journal's topics): drawn in the link colour, the id of the one
// tapped is returned (-1: none). The words are laid out again only when `revision` changes
struct UiLink { size_t begin, end; int id; };
struct UiLinkWord { std::string text; float x, y, w; int link; };
struct UiLinkText { UiScroll scroll; long revision = -1; float height = 0.0f; std::vector<UiLinkWord> words; int sel = -1; };   // sel: the word the link chosen with the keys starts at
int uiLinkTextBox(UiLinkText& t, long revision, float x, float y, float w, float h, const std::string& text,
	const std::vector<UiLink>& links, float scale = 0.5f, bool keys = true);
// A scrolling page of text with pictures: lines "[[img:<key>]]" draw the picture look(key) gives
// (file, w, h; empty file: left out), scaled to fit the width
struct UiPicture { std::string file; int w = 0, h = 0; };
void uiTextBoxPictures(UiScroll& s, float x, float y, float w, float h, const std::string& text,
	UiPicture (*look)(const std::string& key, void* ctx), void* ctx, float scale = 0.5f, u32 color = 0);
// The same text and pictures cut into pages w x h (a book's): a line or picture that doesn't fit starts the next
// page, blank lines at the top of a page are dropped, a picture taller than a page is shrunk to it
struct UiPageBlock { std::string text; UiPicture pic; float y = 0.0f, h = 0.0f, pw = 0.0f; bool center = false; };
typedef std::vector<UiPageBlock> UiPage;
std::vector<UiPage> uiPaginate(const std::string& text, float w, float h, float scale,
	UiPicture (*look)(const std::string& key, void* ctx), void* ctx);
void uiDrawPage(const UiPage& page, float x, float y, float w, float scale, u32 color);
