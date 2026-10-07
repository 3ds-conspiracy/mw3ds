#include "ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "linear.h"
#include "log.h"

// 2D draws since the counter was last read (main.cpp compares the two eyes' HUD passes)
int g_uiDraws = 0;

// Plain palette until a theme brings Morrowind.ini's
namespace col
{
	u32 panel = C2D_Color32(0, 0, 0, 225);
	u32 panelLight = C2D_Color32(22, 17, 11, 235);
	u32 border = C2D_Color32(136, 110, 64, 255);
	u32 text = C2D_Color32(202, 165, 96, 255);
	u32 textOver = C2D_Color32(223, 201, 159, 255);
	u32 textPressed = C2D_Color32(243, 237, 221, 255);
	u32 textDim = C2D_Color32(140, 118, 76, 255);
	u32 header = C2D_Color32(223, 201, 159, 255);
	u32 link = C2D_Color32(112, 126, 207, 255);
	u32 select = C2D_Color32(58, 45, 26, 255);
	u32 health = C2D_Color32(200, 60, 30, 255);
	u32 magicka = C2D_Color32(53, 69, 159, 255);
	u32 fatigue = C2D_Color32(0, 150, 60, 255);
	u32 count = C2D_Color32(223, 201, 159, 255);
	u32 black = C2D_Color32(0, 0, 0, 255);
}

static std::string s_artDir;              // data folder of the pictures (uiArt)
static C2D_TextBuf s_buf;
static UiInput s_in;
static float s_lineFeed = 30.0f;
static u32 s_frame = 0;

// Theme textures: the font and frame atlas stay loaded, icon pages load on demand
static UiThemeDef s_def;
static bool s_fontOn = false, s_atlasOn = false;
static C3D_Tex s_fontTex, s_atlasTex;
static UiGlyph s_ascii[256];
static bool s_asciiHas[256];
struct IconPage { C3D_Tex tex; bool loaded = false, failed = false; u32 used = 0; };
static std::vector<IconPage> s_iconPages;
static std::string s_dataDir;
static const int kMaxIconPages = 8;     // loaded at once (128 KB each); unused ones go first

static std::unordered_map<std::string, std::vector<std::string>> s_wrapCache;

void uiInit()
{
	fontEnsureMapped();
	s_buf = C2D_TextBufNew(8192);
	s_lineFeed = fontGetInfo(nullptr)->lineFeed;
}

void uiExit()
{
	uiFreeTheme();
	C2D_TextBufDelete(s_buf);
}

void uiBeginFrame(const UiInput& in)
{
	s_in = in;
	s_frame++;
	C2D_TextBufClear(s_buf);
}

static bool loadTex(const std::string& path, C3D_Tex* tex, GPU_TEXTURE_FILTER_PARAM filter)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	LinearGuard guard;
	Tex3DS_Texture t = Tex3DS_TextureImportStdio(f, tex, nullptr, false);
	fclose(f);
	if (!t)
		return false;
	Tex3DS_TextureFree(t);
	C3D_TexSetFilter(tex, filter, filter);
	C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
	return true;
}

struct ArtTex { C3D_Tex tex; bool ok; };
static std::unordered_map<std::string, ArtTex*> s_art;

bool uiArt(const std::string& file, int w, int h, float x, float y, float dw, float dh)
{
	auto it = s_art.find(file);
	ArtTex* a;
	if (it == s_art.end())
	{
		a = new ArtTex();
		a->ok = loadTex(s_artDir + "/" + file, &a->tex, GPU_LINEAR);
		s_art[file] = a;
	}
	else
		a = it->second;
	if (!a->ok || w <= 0 || h <= 0)
		return false;
	Tex3DS_SubTexture sub = { (u16)w, (u16)h, 0.0f, 1.0f, (float)w / a->tex.width, 1.0f - (float)h / a->tex.height };
	C2D_Image img = { &a->tex, &sub };
	g_uiDraws++;
	C2D_DrawImageAt(img, x, y, 0.5f, nullptr, dw > 0 ? dw / w : 1.0f, dh > 0 ? dh / h : 1.0f);
	return true;
}

void uiArtFree()
{
	LinearGuard guard;
	for (auto& a : s_art)
	{
		if (a.second->ok)
			C3D_TexDelete(&a.second->tex);
		delete a.second;
	}
	s_art.clear();
}

static u32 themeColor(const char* key, u32 fallback)
{
	auto it = s_def.colors.find(key);
	return it != s_def.colors.end() ? it->second : fallback;
}

void uiFreeTheme()
{
	LinearGuard guard;
	if (s_fontOn)
		C3D_TexDelete(&s_fontTex);
	if (s_atlasOn)
		C3D_TexDelete(&s_atlasTex);
	for (auto& p : s_iconPages)
		if (p.loaded)
			C3D_TexDelete(&p.tex);
	s_iconPages.clear();
	s_fontOn = s_atlasOn = false;
	s_wrapCache.clear();
}

void uiLoadTheme(const char* dataDir, const UiThemeDef& def)
{
	uiFreeTheme();
	s_def = def;
	s_artDir = dataDir;
	s_dataDir = dataDir;
	std::string dir = std::string(dataDir) + "/";
	if (!def.fontFile.empty() && def.fontLine > 0.0f && loadTex(dir + def.fontFile, &s_fontTex, GPU_LINEAR))
	{
		s_fontOn = true;
		memset(s_asciiHas, 0, sizeof(s_asciiHas));
		for (auto& g : def.glyphs)
			if (g.first < 256)
			{
				s_ascii[g.first] = g.second;
				s_asciiHas[g.first] = true;
			}
	}
	if (!def.atlasFile.empty())
		s_atlasOn = loadTex(dir + def.atlasFile, &s_atlasTex, GPU_NEAREST);
	s_iconPages.resize(def.iconPages.size());
	// Morrowind.ini colours
	col::text = themeColor("normal", col::text);
	col::textOver = themeColor("normal_over", col::textOver);
	col::textPressed = themeColor("normal_pressed", col::textPressed);
	col::header = themeColor("header", col::header);
	col::link = themeColor("link", col::link);
	col::health = themeColor("health", col::health);
	col::magicka = themeColor("magic", col::magicka);
	col::fatigue = themeColor("fatigue", col::fatigue);
	col::count = themeColor("count", col::count);
	logf("ui: font %s, frames %s (%d pieces), %d icon pages", s_fontOn ? "Magic Cards" : "system",
		s_atlasOn ? "Morrowind" : "plain", (int)def.pieces.size(), (int)def.iconPages.size());
}

const UiInput& uiIn()
{
	return s_in;
}

void uiConsume(u32 keys)
{
	s_in.down &= ~keys;
}

// The Morrowind font is drawn at its own pixel size when that is close to the size asked for
// (crisp), else scaled
static float fontScale(float scale)
{
	float k = scale * s_lineFeed / s_def.fontLine;
	return fabsf(k - 1.0f) < 0.2f ? 1.0f : k;
}

static const UiGlyph* glyphOf(u32 cp)
{
	if (cp < 256)
		return s_asciiHas[cp] ? &s_ascii[cp] : (s_asciiHas['?'] ? &s_ascii['?'] : nullptr);
	auto it = s_def.glyphs.find(cp);
	if (it != s_def.glyphs.end())
		return &it->second;
	return s_asciiHas['?'] ? &s_ascii['?'] : nullptr;
}

float uiLineHeight(float scale)
{
	if (s_fontOn)
		return s_def.fontLine * fontScale(scale);
	return s_lineFeed * scale;
}

static float glyphWidth(u32 cp, float scale)
{
	int g = fontGlyphIndexFromCodePoint(nullptr, cp);
	return fontGetCharWidthInfo(nullptr, g)->charWidth * scale;
}

float uiTextWidth(const std::string& s, float scale)
{
	if (s_fontOn)
	{
		float k = fontScale(scale), w = 0.0f;
		const u8* p = (const u8*)s.c_str();
		while (*p)
		{
			u32 cp;
			ssize_t n = decode_utf8(&cp, p);
			if (n <= 0)
				break;
			p += n;
			if (const UiGlyph* g = glyphOf(cp))
				w += (g->advance + g->bearX) * k;
		}
		return w;
	}
	float w = 0.0f;
	const u8* p = (const u8*)s.c_str();
	while (*p)
	{
		u32 cp;
		ssize_t n = decode_utf8(&cp, p);
		if (n <= 0)
			break;
		p += n;
		w += glyphWidth(cp, scale);
	}
	return w;
}

static std::vector<std::string> wrapUncached(const std::string& text, float width, float scale);

// Wrapping measures every glyph, so results are cached (text boxes re-wrap the same text each frame)
std::vector<std::string> uiWrap(const std::string& text, float width, float scale)
{
	auto& cache = s_wrapCache;
	char key[32];
	snprintf(key, sizeof(key), "%.1f|%.3f|", width, scale);
	std::string k = key + text;
	auto it = cache.find(k);
	if (it != cache.end())
		return it->second;
	if (cache.size() > 64)
		cache.clear();
	return cache[k] = wrapUncached(text, width, scale);
}

static std::vector<std::string> wrapUncached(const std::string& text, float width, float scale)
{
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= text.size())
	{
		size_t nl = text.find('\n', start);
		std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		std::string line;
		size_t i = 0;
		while (i < para.size())
		{
			size_t sp = para.find(' ', i);
			std::string word = para.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
			std::string cand = line.empty() ? word : line + " " + word;
			if (!line.empty() && uiTextWidth(cand, scale) > width)
			{
				lines.push_back(line);
				line = word;
			}
			else
				line = cand;
			if (sp == std::string::npos)
				break;
			i = sp + 1;
		}
		lines.push_back(line);
		if (nl == std::string::npos)
			break;
		start = nl + 1;
	}
	return lines;
}

void uiText(float x, float y, float scale, u32 color, const std::string& s)
{
	if (s.empty())
		return;
	if (s_fontOn)
	{
		float k = fontScale(scale);
		bool crisp = k == 1.0f;
		C2D_ImageTint tint;
		C2D_PlainImageTint(&tint, color, 1.0f);
		float pen = crisp ? floorf(x + 0.5f) : x;
		y = crisp ? floorf(y + 0.5f) : y;
		const u8* p = (const u8*)s.c_str();
		while (*p)
		{
			u32 cp;
			ssize_t n = decode_utf8(&cp, p);
			if (n <= 0)
				break;
			p += n;
			const UiGlyph* g = glyphOf(cp);
			if (!g)
				continue;
			if (g->w > 0 && g->h > 0)
			{
				Tex3DS_SubTexture sub = { g->w, g->h, (float)g->x / s_def.fontW, 1.0f - (float)g->y / s_def.fontH,
					(float)(g->x + g->w) / s_def.fontW, 1.0f - (float)(g->y + g->h) / s_def.fontH };
				C2D_Image img = { &s_fontTex, &sub };
				g_uiDraws++;
				C2D_DrawImageAt(img, pen + g->bearX * k, y + g->bearY * k, 0.5f, &tint, k, k);
			}
			pen += (g->advance + g->bearX) * k;
		}
		return;
	}
	C2D_Text t;
	if (!C2D_TextParse(&t, s_buf, s.c_str()))
		return;
	C2D_TextOptimize(&t);
	g_uiDraws++;
	C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

void uiTextCentered(float cx, float y, float scale, u32 color, const std::string& s)
{
	uiText(cx - uiTextWidth(s, scale) / 2.0f, y, scale, color, s);
}

void uiTextRight(float rx, float y, float scale, u32 color, const std::string& s)
{
	uiText(rx - uiTextWidth(s, scale), y, scale, color, s);
}

void uiRect(float x, float y, float w, float h, u32 color)
{
	g_uiDraws++;
	C2D_DrawRectSolid(x, y, 0.5f, w, h, color);
}

static const UiPiece* findPiece(const std::string& name)
{
	if (!s_atlasOn)
		return nullptr;
	auto it = s_def.pieces.find(name);
	return it != s_def.pieces.end() ? &it->second : nullptr;
}

// part: the top left fraction of the piece that is drawn (1: all of it)
static void drawPiece(const UiPiece& p, float x, float y, float w, float h, float part = 1.0f)
{
	if (w <= 0.0f || h <= 0.0f)
		return;
	float pw = p.w * part, ph = p.h * part;
	Tex3DS_SubTexture sub = { (u16)pw, (u16)ph, (float)p.x / s_def.atlasW, 1.0f - (float)p.y / s_def.atlasH,
		(p.x + pw) / s_def.atlasW, 1.0f - (p.y + ph) / s_def.atlasH };
	C2D_Image img = { &s_atlasTex, &sub };
	g_uiDraws++;
	C2D_DrawImageAt(img, x, y, 0.5f, nullptr, w / pw, h / ph);
}

bool uiHasPiece(const char* name)
{
	return findPiece(name) != nullptr;
}

void uiPiece(const char* name, float x, float y, float w, float h)
{
	if (const UiPiece* p = findPiece(name))
		drawPiece(*p, x, y, w, h);
}

bool uiPieceRotated(const char* name, float cx, float cy, float w, float h, float angle)
{
	const UiPiece* p = findPiece(name);
	if (!p)
		return false;
	Tex3DS_SubTexture sub = { p->w, p->h, (float)p->x / s_def.atlasW, 1.0f - (float)p->y / s_def.atlasH,
		(float)(p->x + p->w) / s_def.atlasW, 1.0f - (float)(p->y + p->h) / s_def.atlasH };
	C2D_Image img = { &s_atlasTex, &sub };
	g_uiDraws++;
	C2D_DrawImageAtRotated(img, cx, cy, 0.5f, angle, nullptr, w / p->w, h / p->h);
	return true;
}

void uiFrame(float x, float y, float w, float h, const char* style, u32 fallback)
{
	static const char* parts[8] = { "_top_left_corner", "_top", "_top_right_corner", "_left", "_right",
		"_bottom_left_corner", "_bottom", "_bottom_right_corner" };
	const UiPiece* p[8];
	bool all = s_atlasOn;
	for (int i = 0; i < 8 && all; i++)
		all = (p[i] = findPiece(std::string(style) + parts[i])) != nullptr;
	if (!all)
	{
		u32 c = fallback ? fallback : col::border;
		uiRect(x, y, w, 1, c);
		uiRect(x, y + h - 1, w, 1, c);
		uiRect(x, y + 1, 1, h - 2, c);
		uiRect(x + w - 1, y + 1, 1, h - 2, c);
		return;
	}
	float l = p[0]->w, t = p[0]->h, r = p[2]->w, b = p[5]->h;
	drawPiece(*p[0], x, y, l, t);
	drawPiece(*p[2], x + w - r, y, r, t);
	drawPiece(*p[5], x, y + h - b, l, b);
	drawPiece(*p[7], x + w - r, y + h - b, r, b);
	drawPiece(*p[1], x + l, y, w - l - r, p[1]->h);
	drawPiece(*p[6], x + l, y + h - p[6]->h, w - l - r, p[6]->h);
	drawPiece(*p[3], x, y + t, p[3]->w, h - t - b);
	drawPiece(*p[4], x + w - p[4]->w, y + t, p[4]->w, h - t - b);
}

void uiPanel(float x, float y, float w, float h)
{
	uiRect(x, y, w, h, col::panel);
	uiFrame(x, y, w, h);
}

// Morrowind's window caption: the title between two ornamental bars
void uiCaption(const std::string& title, const std::string& note)
{
	uiRect(0, 0, 320, 21, col::panelLight);
	// A note (gold, magicka, ...) takes the right end; the title centres in what is left
	float right = 314.0f;
	float tw = uiTextWidth(title, 0.55f);
	if (!note.empty())
	{
		// a note too long to sit beside the title ("Seyda Neen, Arrille's Tradehouse") is cut short with ".."
		std::string text = note;
		float room = 314.0f - 6.0f - tw - 12.0f;
		if (uiTextWidth(text, 0.45f) > room)
		{
			while (!text.empty() && uiTextWidth(text + "..", 0.45f) > room)
			{
				while (!text.empty() && ((u8)text.back() & 0xC0) == 0x80)
					text.pop_back();
				if (!text.empty())
					text.pop_back();
			}
			text += "..";
		}
		float nw = uiTextWidth(text, 0.45f);
		uiTextRight(314, (21.0f - uiLineHeight(0.45f)) / 2.0f, 0.45f, col::textDim, text);
		right = 314.0f - nw - 8.0f;
	}
	float lh = uiLineHeight(0.55f);
	float tx = fmaxf(6.0f, (6.0f + right) / 2.0f - tw / 2.0f);
	uiText(tx, (21.0f - lh) / 2.0f, 0.55f, col::header, title);
	float barH = 8.0f, by = 7.0f;
	float spans[2][2] = { { 6.0f, tx - 8.0f }, { tx + tw + 8.0f, right } };
	for (auto& sp : spans)
	{
		float bw = sp[1] - sp[0];
		if (bw < 8.0f)
			continue;
		if (uiHasPiece("menu_head_block_middle") && uiHasPiece("menu_head_block_top_left_corner"))
		{
			uiPiece("menu_head_block_middle", sp[0], by, bw, barH);
			uiFrame(sp[0], by, bw, barH, "menu_head_block");
		}
		else
		{
			uiRect(sp[0], by + 2, bw, 1, col::border);
			uiRect(sp[0], by + 5, bw, 1, col::border);
		}
	}
	uiRect(0, 21, 320, 1, col::border);
}

void uiBar(float x, float y, float w, float h, float frac, u32 color)
{
	uiRect(x, y, w, h, col::black);
	if (frac > 0.0f)
		uiRect(x + 1, y + 1, (w - 2) * fminf(frac, 1.0f), h - 2, color);
	uiFrame(x, y, w, h);
}

bool uiIcon(int icon, float x, float y, float size)
{
	int perRow = s_def.iconPage / s_def.iconSize;
	int perPage = perRow * perRow;
	if (icon < 0 || perPage <= 0)
		return false;
	int page = icon / perPage;
	if (page >= (int)s_iconPages.size())
		return false;
	IconPage& pg = s_iconPages[page];
	if (!pg.loaded)
	{
		if (pg.failed)
			return false;
		// Too many pages in memory: drop ones this frame hasn't drawn from (the GPU is done with
		// earlier frames by now)
		int loaded = 0;
		for (auto& p : s_iconPages)
			loaded += p.loaded;
		for (size_t i = 0; i < s_iconPages.size() && loaded >= kMaxIconPages; i++)
			if (s_iconPages[i].loaded && s_iconPages[i].used != s_frame)
			{
				LinearGuard guard;
				C3D_TexDelete(&s_iconPages[i].tex);
				s_iconPages[i].loaded = false;
				loaded--;
			}
		pg.loaded = loadTex(s_dataDir + "/" + s_def.iconPages[page].first, &pg.tex, GPU_LINEAR);
		if (!pg.loaded)
		{
			pg.failed = true;
			logf("ui: cannot load %s", s_def.iconPages[page].first.c_str());
			return false;
		}
	}
	pg.used = s_frame;
	int k = icon % perPage;
	float px = (k % perRow) * s_def.iconSize, py = (k / perRow) * s_def.iconSize;
	float tw = pg.tex.width, th = pg.tex.height;
	Tex3DS_SubTexture sub = { (u16)s_def.iconSize, (u16)s_def.iconSize, px / tw, 1.0f - py / th,
		(px + s_def.iconSize) / tw, 1.0f - (py + s_def.iconSize) / th };
	C2D_Image img = { &pg.tex, &sub };
	float k2 = size / s_def.iconSize;
	g_uiDraws++;
	C2D_DrawImageAt(img, x, y, 0.5f, nullptr, k2, k2);
	return true;
}

bool uiHit(float x, float y, float w, float h)
{
	return s_in.tapped && s_in.tapX >= x && s_in.tapX < x + w && s_in.tapY >= y && s_in.tapY < y + h;
}

bool uiButton(float x, float y, float w, float h, const std::string& label, bool focused, bool enabled)
{
	bool pressed = enabled && s_in.touching && s_in.touchX >= x && s_in.touchX < x + w && s_in.touchY >= y && s_in.touchY < y + h;
	// Morrowind's buttons: framed text that brightens under the pointer and when pressed
	uiRect(x, y, w, h, pressed || focused ? col::select : col::panelLight);
	uiFrame(x, y, w, h, "menu_button_frame", enabled ? col::border : col::textDim);
	// A disabled button greys out: the frame is a picture that ignores the fallback colour, so it is darkened
	if (!enabled)
		uiRect(x, y, w, h, C2D_Color32(0, 0, 0, 150));
	u32 c = !enabled ? col::textDim : pressed ? col::textPressed : focused ? col::textOver : col::text;
	// The label shrinks to fit the button (its width and its height: the spell effect rows are 12 high), down to
	// a size still readable; past that it is cut short with ".."
	float scale = uiTextWidth(label, 0.5f) > w - 6 ? 0.42f : 0.5f;
	while (scale > 0.33f && (uiTextWidth(label, scale) > w - 6 || uiLineHeight(scale) > h + 2))
		scale -= 0.02f;
	std::string text = label;
	if (uiTextWidth(text, scale) > w - 6)
	{
		while (!text.empty() && uiTextWidth(text + "..", scale) > w - 6)
		{
			while (!text.empty() && ((u8)text.back() & 0xC0) == 0x80)   // (a whole UTF-8 character at a time)
				text.pop_back();
			if (!text.empty())
				text.pop_back();
		}
		text += "..";
	}
	uiTextCentered(x + w / 2, y + (h - uiLineHeight(scale)) / 2, scale, c, text);
	if (!enabled)
		return false;
	return uiHit(x, y, w, h) || (focused && (s_in.down & KEY_A));
}

int uiList(UiList& list, float x, float y, float w, float h, const std::vector<std::string>& rows, float scale, bool keys)
{
	float rowH = uiLineHeight(scale) + 4.0f;
	float content = rows.size() * rowH;
	int result = -1;
	if (keys && !rows.empty())
	{
		if (s_in.down & KEY_DOWN) list.selected = (list.selected + 1) % rows.size();
		if (s_in.down & KEY_UP) list.selected = (list.selected + rows.size() - 1) % rows.size();
		if (list.selected >= (int)rows.size()) list.selected = rows.size() - 1;
		// keep the selection visible
		float sy = list.selected * rowH;
		if (sy < list.scroll) list.scroll = sy;
		if (sy + rowH > list.scroll + h) list.scroll = sy + rowH - h;
		if (s_in.down & KEY_A) result = list.selected;
	}
	if (s_in.touching && s_in.touchX >= x && s_in.touchX < x + w && s_in.touchY >= y && s_in.touchY < y + h)
		list.scroll -= s_in.dragDY;
	list.scroll = fmaxf(0.0f, fminf(list.scroll, fmaxf(0.0f, content - h)));

	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (y + h)), (u32)(320 - (x + w)), (u32)(240 - y), (u32)(320 - x));
	for (size_t i = 0; i < rows.size(); i++)
	{
		float ry = y + i * rowH - list.scroll;
		if (ry + rowH < y || ry > y + h)
			continue;
		bool sel = (int)i == list.selected && keys;
		if (sel)
			uiRect(x, ry, w, rowH, col::select);
		uiText(x + 4, ry + 2, scale, sel ? col::textOver : col::text, rows[i]);
		if (uiHit(x, fmaxf(ry, y), w, fminf(rowH, y + h - ry)))
		{
			list.selected = i;
			result = i;
		}
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	return result;
}

int uiWrapList(UiList& list, float x, float y, float w, float h, const std::vector<std::string>& rows,
	float scale, bool keys)
{
	float lh = uiLineHeight(scale);
	std::vector<std::vector<std::string>> lines(rows.size());
	std::vector<float> top(rows.size() + 1, 0.0f);
	for (size_t i = 0; i < rows.size(); i++)
	{
		lines[i] = uiWrap(rows[i], w - 8, scale);
		if (lines[i].empty())
			lines[i].push_back("");
		top[i + 1] = top[i] + lines[i].size() * lh + 4.0f;
	}
	float content = top[rows.size()];
	int result = -1;
	if (keys && !rows.empty())
	{
		if (s_in.down & KEY_DOWN) list.selected = (list.selected + 1) % rows.size();
		if (s_in.down & KEY_UP) list.selected = (list.selected + rows.size() - 1) % rows.size();
		if (list.selected >= (int)rows.size()) list.selected = rows.size() - 1;
		if (list.selected >= 0)
		{
			float sy = top[list.selected], sh = top[list.selected + 1] - sy;
			if (sy < list.scroll) list.scroll = sy;
			if (sy + sh > list.scroll + h) list.scroll = sy + sh - h;
		}
		if (s_in.down & KEY_A) result = list.selected;
	}
	if (s_in.touching && s_in.touchX >= x && s_in.touchX < x + w && s_in.touchY >= y && s_in.touchY < y + h)
		list.scroll -= s_in.dragDY;
	list.scroll = fmaxf(0.0f, fminf(list.scroll, fmaxf(0.0f, content - h)));

	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (y + h)), (u32)(320 - (x + w)), (u32)(240 - y), (u32)(320 - x));
	for (size_t i = 0; i < rows.size(); i++)
	{
		float ry = y + top[i] - list.scroll, rh = top[i + 1] - top[i];
		if (ry + rh < y || ry > y + h)
			continue;
		bool sel = (int)i == list.selected && keys;
		if (sel)
			uiRect(x, ry, w, rh, col::select);
		for (size_t k = 0; k < lines[i].size(); k++)
			uiText(x + 4, ry + 2 + k * lh, scale, sel ? col::textOver : col::text, lines[i][k]);
		if (uiHit(x, fmaxf(ry, y), w, fminf(rh, y + h - ry)))
		{
			list.selected = i;
			result = i;
		}
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	return result;
}

// The C-stick scrolls a text pane smoothly: up to 6 pixels a frame
float uiStickScroll()
{
	return s_in.stickY * 6.0f;
}

void uiTextBox(UiScroll& s, float x, float y, float w, float h, const std::string& text, float scale, bool keys)
{
	std::vector<std::string> lines = uiWrap(text, w - 8, scale);
	float lh = uiLineHeight(scale);
	float content = lines.size() * lh;
	if (keys)
	{
		if (s_in.held & KEY_DOWN) s.scroll += 4.0f;
		if (s_in.held & KEY_UP) s.scroll -= 4.0f;
	}
	s.scroll += uiStickScroll();
	if (s_in.touching && s_in.touchX >= x && s_in.touchX < x + w && s_in.touchY >= y && s_in.touchY < y + h)
		s.scroll -= s_in.dragDY;
	s.scroll = fmaxf(0.0f, fminf(s.scroll, fmaxf(0.0f, content - h)));
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (y + h)), (u32)(320 - (x + w)), (u32)(240 - y), (u32)(320 - x));
	for (size_t i = 0; i < lines.size(); i++)
	{
		float ly = y + i * lh - s.scroll;
		if (ly + lh < y || ly > y + h)
			continue;
		uiText(x + 4, ly, scale, col::text, lines[i]);
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

int uiLinkTextBox(UiLinkText& t, long revision, float x, float y, float w, float h, const std::string& text,
	const std::vector<UiLink>& links, float scale, bool keys)
{
	float lh = uiLineHeight(scale);
	if (t.revision != revision)
	{
		// word by word, as uiWrap: a word that would pass the right edge starts the next line
		t.revision = revision;
		t.words.clear();
		t.scroll = UiScroll();
		std::vector<int> linkAt(text.size(), -1);
		for (auto& l : links)
			for (size_t k = l.begin; k < l.end && k < text.size(); k++)
				linkAt[k] = l.id;
		float space = uiTextWidth(" ", scale), cx = 0.0f, cy = 0.0f;
		size_t i = 0;
		while (i < text.size())
		{
			if (text[i] == '\n')
			{
				cx = 0.0f;
				cy += lh;
				i++;
				continue;
			}
			if (text[i] == ' ')
			{
				i++;
				continue;
			}
			// a word ends at a space, a line break, or where a link starts or ends ("Balmora," links "Balmora")
			size_t e = i + 1;
			while (e < text.size() && text[e] != ' ' && text[e] != '\n' && linkAt[e] == linkAt[i])
				e++;
			std::string word = text.substr(i, e - i);
			float ww = uiTextWidth(word, scale);
			bool joined = i > 0 && text[i - 1] != ' ' && text[i - 1] != '\n';    // the rest of a word a link split
			if (!joined && cx > 0.0f && cx + ww > w - 8)
			{
				cx = 0.0f;
				cy += lh;
			}
			if (joined && !t.words.empty())
				cx = t.words.back().x + t.words.back().w;
			t.words.push_back({ word, cx, cy, ww, linkAt[i] });
			cx += ww + space;
			i = e;
		}
		t.height = cy + lh;
	}
	UiScroll& s = t.scroll;
	if (keys)
	{
		if (s_in.held & KEY_DOWN) s.scroll += 4.0f;
		if (s_in.held & KEY_UP) s.scroll -= 4.0f;
	}
	s.scroll += uiStickScroll();
	if (s_in.touching && s_in.touchX >= x && s_in.touchX < x + w && s_in.touchY >= y && s_in.touchY < y + h)
		s.scroll -= s_in.dragDY;
	s.scroll = fmaxf(0.0f, fminf(s.scroll, fmaxf(0.0f, t.height - h)));
	int hit = -1;
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (y + h)), (u32)(320 - (x + w)), (u32)(240 - y), (u32)(320 - x));
	for (auto& wd : t.words)
	{
		float wy = y + wd.y - s.scroll;
		if (wy + lh < y || wy > y + h)
			continue;
		uiText(x + 4 + wd.x, wy, scale, wd.link >= 0 ? col::link : col::text, wd.text);
		if (wd.link >= 0 && wy >= y && uiHit(x + 4 + wd.x, wy, wd.w, lh))
			hit = wd.link;
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	return hit;
}

void uiTextBoxPictures(UiScroll& s, float x, float y, float w, float h, const std::string& text,
	UiPicture (*look)(const std::string& key, void* ctx), void* ctx, float scale)
{
	// blocks: wrapped text lines, and pictures
	struct Block { std::string text; UiPicture pic; float height; bool center = false; };
	std::vector<Block> blocks;
	float lh = uiLineHeight(scale), content = 0.0f;
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t at = text.find("[[img:", pos);
		std::string part = text.substr(pos, at == std::string::npos ? std::string::npos : at - pos);
		// paragraph by paragraph: one starting with \x01 is centred (books' <DIV ALIGN="CENTER">)
		for (size_t ps = 0; ps <= part.size();)
		{
			size_t nl = part.find('\n', ps);
			std::string para = part.substr(ps, nl == std::string::npos ? std::string::npos : nl - ps);
			bool center = !para.empty() && para[0] == '\x01';
			if (center)
				para.erase(0, 1);
			for (auto& l : uiWrap(para, w - 8, scale))
			{
				blocks.push_back({ l, {}, lh, center });
				content += lh;
			}
			if (nl == std::string::npos)
				break;
			ps = nl + 1;
		}
		if (at == std::string::npos)
			break;
		size_t end = text.find("]]", at);
		if (end == std::string::npos)
			break;
		UiPicture p = look(text.substr(at + 6, end - at - 6), ctx);
		if (!p.file.empty() && p.w > 0 && p.h > 0)
		{
			float k = fminf(1.0f, (w - 8) / p.w);
			blocks.push_back({ "", p, p.h * k + 4.0f });
			content += p.h * k + 4.0f;
		}
		pos = end + 2;
	}
	if (s_in.held & KEY_DOWN) s.scroll += 4.0f;
	if (s_in.held & KEY_UP) s.scroll -= 4.0f;
	s.scroll += uiStickScroll();
	if (s_in.touching && s_in.touchX >= x && s_in.touchX < x + w && s_in.touchY >= y && s_in.touchY < y + h)
		s.scroll -= s_in.dragDY;
	s.scroll = fmaxf(0.0f, fminf(s.scroll, fmaxf(0.0f, content - h)));
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (y + h)), (u32)(320 - (x + w)), (u32)(240 - y), (u32)(320 - x));
	float by = y - s.scroll;
	for (auto& b : blocks)
	{
		if (by + b.height >= y && by <= y + h)
		{
			if (!b.pic.file.empty())
			{
				float k = fminf(1.0f, (w - 8) / b.pic.w);
				uiArt(b.pic.file, b.pic.w, b.pic.h, x + (w - b.pic.w * k) / 2, by + 2, b.pic.w * k, b.pic.h * k);
			}
			else if (b.center)
				uiText(x + (w - uiTextWidth(b.text, scale)) / 2, by, scale, col::text, b.text);
			else
				uiText(x + 4, by, scale, col::text, b.text);
		}
		by += b.height;
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

int uiItemGrid(UiGrid& grid, float x, float y, float w, float h, const std::vector<UiGridItem>& items, bool keys)
{
	const float cell = 36.0f;
	int cols = w / cell > 1 ? (int)(w / cell) : 1;
	int n = items.size();
	int rows = (n + cols - 1) / cols;
	float content = rows * cell;
	float left = x + (w - cols * cell) / 2.0f;
	int result = -1;
	if (grid.selected >= n)
		grid.selected = n - 1;
	if (grid.selected < 0)
		grid.selected = 0;
	if (keys && n > 0)
	{
		int sel = grid.selected;
		if (s_in.down & KEY_RIGHT) sel = sel + 1 < n ? sel + 1 : sel;
		if (s_in.down & KEY_LEFT) sel = sel > 0 ? sel - 1 : sel;
		if (s_in.down & KEY_DOWN) sel = sel + cols < n ? sel + cols : sel;
		if (s_in.down & KEY_UP) sel = sel - cols >= 0 ? sel - cols : sel;
		if (sel != grid.selected)
		{
			grid.selected = sel;
			float sy = (sel / cols) * cell;
			if (sy < grid.scroll) grid.scroll = sy;
			if (sy + cell > grid.scroll + h) grid.scroll = sy + cell - h;
		}
		if (s_in.down & KEY_A) result = grid.selected;
	}
	if (s_in.touching && s_in.touchX >= x && s_in.touchX < x + w && s_in.touchY >= y && s_in.touchY < y + h)
		grid.scroll -= s_in.dragDY;
	grid.scroll = fmaxf(0.0f, fminf(grid.scroll, fmaxf(0.0f, content - h)));

	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (y + h)), (u32)(320 - (x + w)), (u32)(240 - y), (u32)(320 - x));
	for (int i = 0; i < n; i++)
	{
		float cx = left + (i % cols) * cell, cy = y + (i / cols) * cell - grid.scroll;
		if (cy + cell < y || cy > y + h)
			continue;
		const UiGridItem& it = items[i];
		// Equipped and enchanted items sit on Morrowind's backgrounds
		bool eq = it.flags & UIGRID_EQUIPPED, magic = it.flags & UIGRID_MAGIC;
		const char* bg = magic && eq ? "menu_icon_magic_equip" : magic ? "menu_icon_magic" : eq ? "menu_icon_equip" : nullptr;
		// (Morrowind draws only the top left 44 x 44 of their 64 x 64 pictures, OpenMW's ItemWidget::setItem: the
		// whole picture put the swirl and the square small, up and to the left of the icon)
		if (bg && uiHasPiece(bg))
			drawPiece(*findPiece(bg), cx + 1, cy + 1, cell - 2, cell - 2, 44.0f / 64.0f);
		else if (eq)
			uiRect(cx + 1, cy + 1, cell - 2, cell - 2, C2D_Color32(70, 52, 30, 255));
		// Icons sit inside the inner square (28 px) the selection outlines, so nothing is clipped
		if (!uiIcon(it.icon, cx + 4, cy + 4, 28.0f))
		{
			// No icon: the name's first letters on a plain tile
			uiRect(cx + 4, cy + 4, cell - 8, cell - 8, col::panelLight);
			std::string shortName = it.label.substr(0, 3);
			uiTextCentered(cx + cell / 2, cy + cell / 2 - uiLineHeight(0.42f) / 2, 0.42f, col::textDim, shortName);
		}
		if (it.flags & UIGRID_DIM)
			uiRect(cx + 1, cy + 1, cell - 2, cell - 2, C2D_Color32(0, 0, 0, 150));
		if (it.count > 1)
		{
			std::string c = it.count > 9999 ? std::to_string(it.count / 1000) + "k" : std::to_string(it.count);
			float lh = uiLineHeight(0.38f);
			uiTextRight(cx + cell - 2, cy + cell - lh - 1, 0.38f, col::count, c);
		}
		if (i == grid.selected && keys)
		{
			// A 2 px outline round the inner square, inset from the cell edge
			uiRect(cx + 2, cy + 2, cell - 4, 2, col::textOver);
			uiRect(cx + 2, cy + cell - 4, cell - 4, 2, col::textOver);
			uiRect(cx + 2, cy + 4, 2, cell - 8, col::textOver);
			uiRect(cx + cell - 4, cy + 4, 2, cell - 8, col::textOver);
		}
		if (uiHit(cx, fmaxf(cy, y), cell, fminf(cell, y + h - cy)))
		{
			grid.selected = i;
			result = i;
		}
	}
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	return result;
}
