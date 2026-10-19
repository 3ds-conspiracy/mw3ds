// The world map and the interiors' local map
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwsound/soundmanagerimp.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwrender/screenshotmanager.hpp"
#include "mwbase/session.hpp"

bool Session::ensureMapTexture()
{
	const MapDef& m = w.game.map;
	if (mapLoaded || mapFailed || m.file.empty())
		return mapLoaded;
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", w.dataDir, m.file.c_str());
	if (texImportFile(&mapTex, path))
	{
		C3D_TexSetFilter(&mapTex, GPU_LINEAR, GPU_NEAREST);
		mapLoaded = true;
	}
	if (!mapLoaded)
	{
		mapFailed = true;
		logf("map: cannot load %s", path);
	}
	return mapLoaded;
}

// One tile of the 2x-detail map, read when the zoomed-in view needs it
bool Session::ensureMapTile(int i)
{
	MapTileTex& t = mapTiles[i];
	if (t.loaded || t.failed)
		return t.loaded;
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", w.dataDir, w.game.map.tiles[i].file.c_str());
	if (texImportFile(&t.tex, path))
		t.loaded = true;
	if (!t.loaded)
	{
		t.failed = true;
		logf("map: cannot load %s", path);
	}
	return t.loaded;
}

void Session::freeMapTiles()
{
	for (MapTileTex& t : mapTiles)
	{
		if (t.loaded)
		{
			LinearGuard guard;
			C3D_TexDelete(&t.tex);
		}
		t.loaded = false;
	}
}

// An interior's local map: the square round everything in it, a little over (its west and south edges, its side)
static bool interiorMapArea(const Cell& cell, float* minX, float* minY, float* size)
{
	float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
	for (auto& b : cell.batches)
		for (int k = 0; k < 2; k++)
		{
			lo[k] = fminf(lo[k], b.bmin[k]);
			hi[k] = fmaxf(hi[k], b.bmax[k]);
		}
	if (lo[0] > hi[0])
		return false;
	*size = fmaxf(hi[0] - lo[0], hi[1] - lo[1]) * 1.04f + 64.0f;
	*minX = (lo[0] + hi[0] - *size) / 2.0f;
	*minY = (lo[1] + hi[1] - *size) / 2.0f;
	return true;
}

void Session::updateLocalMap()
{
	if (w.current < 0 || !w.cells[w.current].live)
		return;
	if (mapCellJob.on && --mapCellJob.frames <= 0)
	{
		// One outdoor cell straight down, as the local map draws (the world map's picture is baked from these):
		// everything loaded inside its square, from above its highest batch
		mapCellJob.on = false;
		const float x0 = mapCellJob.gx * 8192.0f, y0 = mapCellJob.gy * 8192.0f;
		float lo = 1e9f, hi = -1e9f;
		for (LoadedCell* l : w.loaded)
			for (auto& b : l->cell.batches)
				if (b.bmax[0] >= x0 && b.bmin[0] <= x0 + 8192.0f && b.bmax[1] >= y0 && b.bmin[1] <= y0 + 8192.0f)
				{
					lo = fminf(lo, b.bmin[2]);
					hi = fmaxf(hi, b.bmax[2]);
				}
		bool ok = lo <= hi;
		screenshotDrawThisFrame();
		bool drawn = ok && rendererDrawLocalMap(w, x0, y0, 8192.0f, hi + 10.0f, hi - lo + 100.0f, true);
		bool saved = drawn && screenshotSaveTexture(rendererLocalMap(), mapCellJob.path.c_str());
		ok = saved;
		localMapCell = -1;                       // (the map screen's own local map renders again when it shows)
		logf("mapcell: %d %d -> %s %s", mapCellJob.gx, mapCellJob.gy, mapCellJob.path.c_str(), ok ? "saved" : "FAILED");
		return;
	}
	if (!w.cells[w.current].interior)
	{
		// Outdoors, only while the map screen shows it (a render costs a frame): a cell and a half round the
		// player from above everything loaded, again once they walk a quarter cell from its middle
		if (screen != SCR_MAP || !mapLocal)
			return;
		const float size = 12288.0f;
		const float* p = w.player.feet;
		if (localMapCell == -2 && fabsf(p[0] - (localMin[0] + localSize / 2)) < 2048.0f
			&& fabsf(p[1] - (localMin[1] + localSize / 2)) < 2048.0f)
			return;
		float lo = 1e9f, hi = -1e9f;
		for (LoadedCell* l : w.loaded)
			for (auto& b : l->cell.batches)
			{
				lo = fminf(lo, b.bmin[2]);
				hi = fmaxf(hi, b.bmax[2]);
			}
		if (lo > hi)
			return;
		if (rendererDrawLocalMap(w, p[0] - size / 2, p[1] - size / 2, size, hi + 10.0f, hi - lo + 100.0f))
		{
			localMapCell = -2;
			localMin[0] = p[0] - size / 2;
			localMin[1] = p[1] - size / 2;
			localSize = size;
			localTopZ = hi + 10.0f;
			logf("map: local map outdoors at %.0f %.0f", p[0], p[1]);
		}
		return;
	}
	// Again on another floor: what's above the player's head is left out
	if (localMapCell == w.current && fabsf(w.player.feet[2] - localTopZ + 220.0f) < 300.0f)
		return;
	float minX, minY, size;
	if (!interiorMapArea(w.here(), &minX, &minY, &size))
		return;
	float topZ = w.player.feet[2] + 220.0f;
	if (rendererDrawLocalMap(w, minX, minY, size, topZ, 4000.0f))
	{
		localMapCell = w.current;
		localMin[0] = minX;
		localMin[1] = minY;
		localSize = size;
		localTopZ = topZ;
	}
}

// Interior fog of war: the squares within reach of the player come out of it, the nearer the clearer (OpenMW's local
// map: an explore radius of 0.17 of a fog texture's width, 32 squares to an 8192-unit segment, so about 1350 units).
// Every frame, over the interior's local map area whether or not its picture has been drawn yet
void Session::revealLocalMap()
{
	if (w.current < 0 || !w.cells[w.current].live || !w.cells[w.current].interior)
		return;
	float localMin[2], localSize;
	if (!interiorMapArea(w.here(), &localMin[0], &localMin[1], &localSize))
		return;
	const int n = World::kMapFog;
	std::vector<u8>& fog = w.mapFog[lower(w.cells[w.current].name)];
	if (fog.size() != (size_t)(n * n))
		fog.assign(n * n, 15);
	const float radius = 0.17f * 31.0f * 8192.0f / 32.0f, square = localSize / n;
	float gx = (w.player.feet[0] - localMin[0]) / square, gy = (localMin[1] + localSize - w.player.feet[1]) / square;
	int reach = (int)ceilf(radius / square);
	for (int j = std::max(0, (int)gy - reach); j <= std::min(n - 1, (int)gy + reach); j++)
		for (int i = std::max(0, (int)gx - reach); i <= std::min(n - 1, (int)gx + reach); i++)
		{
			float dx = (i + 0.5f - gx) * square, dy = (j + 0.5f - gy) * square;
			float d2 = (dx * dx + dy * dy) / (radius * radius);
			if (d2 >= 1.0f)
				continue;
			u8 level = (u8)(d2 * 15.0f);
			u8& f = fog[j * n + i];
			if (level < f)
				f = level;
		}
}

// The player: Morrowind's compass arrow when the theme has it, else a red triangle
static void drawPlayerArrow(float sx, float sy, float yaw)
{
	if (uiPieceRotated("compass", sx, sy, 16, 16, yaw))
		return;
	float fx = sinf(yaw), fy = -cosf(yaw);
	u32 c = C2D_Color32(255, 60, 40, 255);
	C2D_DrawTriangle(sx + fx * 8, sy + fy * 8, c, sx - fx * 5 - fy * 5, sy - fy * 5 + fx * 5, c,
		sx - fx * 5 + fy * 5, sy - fy * 5 - fx * 5, c, 0.6f);
}

// A frame t pixels thick round x, y, w, h
static void ringRect(float x, float y, float w, float h, float t, u32 c)
{
	uiRect(x, y, w, t, c);
	uiRect(x, y + h - t, w, t, c);
	uiRect(x, y + t, t, h - 2 * t, c);
	uiRect(x + w - t, y + t, t, h - 2 * t, c);
}

// A map marker as Morrowind draws its door_icon texture: a hollow square, a bright ring between a dark rim outside
// and in, the middle clear, tinted with the font colour (OpenMW's MarkerWidget: normal, normal_over under the mouse)
static void drawMapMarker(float cx, float cy, float size, bool lit)
{
	float x = roundf(cx - size / 2), y = roundf(cy - size / 2), t = size >= 10.0f ? 2.0f : 1.0f;
	u32 c = lit ? col::textOver : col::text;
	u32 dim = C2D_Color32((u8)(((c >> 0) & 0xFF) / 3), (u8)(((c >> 8) & 0xFF) / 3), (u8)(((c >> 16) & 0xFF) / 3), 160);
	ringRect(x, y, size, size, 1.0f, dim);
	ringRect(x + 1, y + 1, size - 2, size - 2, t, c);
	ringRect(x + 1 + t, y + 1 + t, size - 2 - 2 * t, size - 2 - 2 * t, 1.0f, dim);
}

// A marker's name in a framed box above it (below when there's no room), kept inside the view
static void drawMapTip(const std::string& name, float mx, float my, float x, float y, float vw)
{
	float tw = uiTextWidth(name, 0.45f), lh = uiLineHeight(0.45f);
	float lx = fminf(fmaxf(mx - tw / 2 - 3, x + 2), x + vw - tw - 8);
	float ly = my - 8 - lh - 4 < y + 2 ? my + 8 : my - 8 - lh - 4;
	uiRect(lx, ly, tw + 6, lh + 4, C2D_Color32(0, 0, 0, 200));
	uiFrame(lx, ly, tw + 6, lh + 4);
	uiText(lx + 3, ly + 2, 0.45f, col::header, name);
}

// Detect Animal / Enchantment / Key: what's within the spell's range (magnitude in feet) shows as dots
void Session::drawDetected(const std::function<bool(float, float, float*, float*)>& toScreen)
{
	float animal = w.effectTotal(64) * 22.0f, magic = w.effectTotal(65) * 22.0f, keys = w.effectTotal(66) * 22.0f;
	if (animal <= 0.0f && magic <= 0.0f && keys <= 0.0f)
		return;
	auto isKey = [](const std::string& id) { return id.compare(0, 4, "key_") == 0 || id.find("_key") != std::string::npos; };
	auto enchanted = [&](const std::string& id) { const Object* o = w.game.object(id); return o && !o->ench.empty(); };
	w.forLoadedRefs([&](int i) {
		const Ref& r = w.refs[i];
		if (!r.visible() || r.pickedUp)
			return;
		float dx = r.pos[0] - w.player.feet[0], dy = r.pos[1] - w.player.feet[1];
		float d = sqrtf(dx * dx + dy * dy);
		u32 colour = 0;
		if (animal > 0.0f && d < animal && r.actor >= 0 && !r.dead && w.game.actors[r.actor].creature)
			colour = C2D_Color32(230, 120, 60, 255);
		bool holdsMagic = r.obj && !r.obj->ench.empty(), holdsKey = isKey(r.idLower);
		for (auto& c : r.contents)
		{
			holdsMagic |= enchanted(c.second);
			holdsKey |= isKey(c.second);
		}
		if (!colour && magic > 0.0f && d < magic && holdsMagic)
			colour = C2D_Color32(120, 150, 255, 255);
		if (!colour && keys > 0.0f && d < keys && holdsKey)
			colour = C2D_Color32(250, 230, 90, 255);
		float sx, sy;
		if (colour && toScreen(r.pos[0], r.pos[1], &sx, &sy))
		{
			uiRect(sx - 2, sy - 2, 4, 4, C2D_Color32(0, 0, 0, 200));
			uiRect(sx - 1, sy - 1, 2, 2, colour);
		}
	});
}

// A part of a map picture (srcW x srcH pixels from srcX, srcY of a texW x texH texture) at (sx, sy), scaled.
// Whole-number scales stay crisp (nearest); the others are smoothed.
static void drawMapPart(C3D_Tex* tex, int texW, int texH, int srcX, int srcY, int srcW, int srcH, float sx, float sy, float scale)
{
	if (srcW <= 0 || srcH <= 0)
		return;
	GPU_TEXTURE_FILTER_PARAM filter = fabsf(scale - roundf(scale)) < 0.001f ? GPU_NEAREST : GPU_LINEAR;
	C3D_TexSetFilter(tex, filter, filter);
	Tex3DS_SubTexture sub = { (u16)srcW, (u16)srcH, (float)srcX / texW, 1.0f - (float)srcY / texH,
		(float)(srcX + srcW) / texW, 1.0f - (float)(srcY + srcH) / texH };
	C2D_Image img = { tex, &sub };
	C2D_DrawImageAt(img, sx, sy, 0.5f, nullptr, scale, scale);
}

// The fog of war over an interior's local map as a 32 x 32 texture (black, its alpha the fog), drawn over the picture
// smoothed so the edge of what was seen fades. False when there's none for the cell.
bool Session::drawLocalFog(float ox, float oy, float picture)
{
	if (w.current < 0 || !w.cells[w.current].interior)
		return false;
	auto it = w.mapFog.find(lower(w.cells[w.current].name));
	const int n = World::kMapFog;
	if (it == w.mapFog.end() || it->second.size() != (size_t)(n * n))
		return false;
	if (!fogTexReady)
	{
		if (!C3D_TexInit(&fogTex, n, n, GPU_RGBA8))
			return false;
		C3D_TexSetWrap(&fogTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		fogTexReady = true;
	}
	// The PICA's layout: 8 x 8 tiles in rows from the bottom of the picture, Morton order inside a tile; a texel is
	// ABGR (alpha in the low byte)
	u32* data = (u32*)fogTex.data;
	for (int row = 0; row < n; row++)
		for (int tx = 0; tx < n; tx++)
		{
			int ty = n - 1 - row;
			int m = (tx & 1) | ((ty & 1) << 1) | ((tx & 2) << 1) | ((ty & 2) << 2) | ((tx & 4) << 2) | ((ty & 4) << 3);
			int at = ((ty >> 3) * (n >> 3) + (tx >> 3)) * 64 + m;
			u8 a = (u8)(it->second[row * n + tx] * 17);
			data[at] = ((u32)8 << 24) | ((u32)7 << 16) | ((u32)6 << 8) | a;
		}
	C3D_TexFlush(&fogTex);
	C3D_TexSetFilter(&fogTex, GPU_LINEAR, GPU_LINEAR);
	Tex3DS_SubTexture sub = { (u16)n, (u16)n, 0.0f, 1.0f, 1.0f, 0.0f };
	C2D_Image img = { &fogTex, &sub };
	C2D_DrawImageAt(img, ox, oy, 0.5f, nullptr, picture / n, picture / n);
	return true;
}

// full: the map screen (pannable, the unvisited cells dark, detail tiles when zoomed in, markers whose names show
// under the finger); not full: the HUD's minimap, always round the player. local: the local map, else the world map
void Session::drawMapView(float x, float y, float vw, float vh, float zoom, bool full, bool local)
{
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (y + vh)), (u32)(320 - (x + vw)), (u32)(240 - y), (u32)(320 - x));
	uiRect(x, y, vw, vh, C2D_Color32(12, 9, 6, 255));
	bool outside = w.current >= 0 && !w.cells[w.current].interior;
	const UiInput& in = uiIn();
	float ax = 0, ay = 0;
	bool arrow = false;
	// The markers in view: where, which (a ref / a label), the name it shows, its size, the square it's under
	struct Marker { float sx, sy, size; int id; std::string name; };
	std::vector<Marker> markers;
	if (!local && ensureMapTexture())
	{
		const MapDef& m = w.game.map;
		// Map pixels, north up (the picture's own pixels, 1 = unitsPerPixel world units)
		auto mapX = [&](float wx) { return (wx - m.originX) / m.unitsPerPixel; };
		auto mapY = [&](float wy) { return m.height - (wy - m.originY) / m.unitsPerPixel; };
		// (indoors the world map opens on its middle: the player isn't on it)
		float cx = outside ? mapX(w.player.feet[0]) : m.width / 2.0f, cy = outside ? mapY(w.player.feet[1]) : m.height / 2.0f;
		if (full)
		{
			if (!mapViewSet)
			{
				mapCX = cx;
				mapCY = cy;
				mapViewSet = true;
			}
			cx = mapCX;
			cy = mapCY;
		}
		// The view stays on the map (centred when the map is smaller than the view)
		float halfW = vw / zoom / 2, halfH = vh / zoom / 2;
		cx = halfW * 2 >= m.width ? m.width / 2.0f : fmaxf(halfW, fminf(cx, m.width - halfW));
		cy = halfH * 2 >= m.height ? m.height / 2.0f : fmaxf(halfH, fminf(cy, m.height - halfH));
		if (full)
		{
			mapCX = cx;
			mapCY = cy;
		}
		// Top left of the view, on a whole screen pixel
		float left = roundf((cx - halfW) * zoom) / zoom, top = roundf((cy - halfH) * zoom) / zoom;
		float right = left + vw / zoom, bottom = top + vh / zoom;
		// The detail tiles that show, when zoomed in past the base picture's own pixels
		bool detail = false;
		if (full && zoom > 1.0f && m.detailUnitsPerPixel > 0.0f && !m.tiles.empty())
		{
			float ds = m.unitsPerPixel / m.detailUnitsPerPixel;      // detail pixels to a map pixel
			float zd = zoom / ds;
			bool seen[4] = {}, all = true;
			int count = (int)std::min<size_t>(m.tiles.size(), 4);
			for (int i = 0; i < count; i++)
			{
				const MapTile& t = m.tiles[i];
				seen[i] = t.x < right * ds && t.x + t.w > left * ds && t.y < bottom * ds && t.y + t.h > top * ds;
				all &= !seen[i] || ensureMapTile(i);
			}
			if (all)
			{
				detail = true;
				for (int i = 0; i < count; i++)
				{
					if (!seen[i])
						continue;
					const MapTile& t = m.tiles[i];
					int ix0 = std::max((int)floorf(left * ds), t.x), ix1 = std::min((int)ceilf(right * ds), t.x + t.w);
					int iy0 = std::max((int)floorf(top * ds), t.y), iy1 = std::min((int)ceilf(bottom * ds), t.y + t.h);
					drawMapPart(&mapTiles[i].tex, t.texWidth, t.texHeight, t.ox + ix0 - t.x, t.oy + iy0 - t.y, ix1 - ix0, iy1 - iy0,
						x + (ix0 - left * ds) * zd, y + (iy0 - top * ds) * zd, zd);
				}
			}
			// Memory is tight: only two tiles stay loaded beyond the ones in view
			int loaded = 0;
			for (int i = 0; i < count; i++)
				loaded += mapTiles[i].loaded;
			for (int i = 0; i < count && loaded > 2; i++)
				if (mapTiles[i].loaded && !seen[i])
				{
					LinearGuard guard;
					C3D_TexDelete(&mapTiles[i].tex);
					mapTiles[i].loaded = false;
					loaded--;
				}
		}
		else if (zoom <= 1.0f)
			freeMapTiles();
		if (!detail)
		{
			int sx0 = (int)floorf(fmaxf(left, 0.0f)), sx1 = (int)ceilf(fminf(right, (float)m.width));
			int sy0 = (int)floorf(fmaxf(top, 0.0f)), sy1 = (int)ceilf(fminf(bottom, (float)m.height));
			drawMapPart(&mapTex, m.texWidth, m.texHeight, sx0, sy0, sx1 - sx0, sy1 - sy0, x + (sx0 - left) * zoom, y + (sy0 - top) * zoom, zoom);
		}
		auto toScreen = [&](float mx, float my, float* sx, float* sy) {
			*sx = x + (mapX(mx) - left) * zoom;
			*sy = y + (mapY(my) - top) * zoom;
			return *sx >= x && *sx < x + vw && *sy >= y && *sy < y + vh;
		};
		if (full)
		{
			// Morrowind's unexplored map: the cells never visited (or revealed by ShowMap) are dark
			const float cellPx = 8192.0f / m.unitsPerPixel;
			int gx0 = (int)floorf(m.originX / 8192.0f), gy0 = (int)floorf(m.originY / 8192.0f);
			int nx = (int)ceilf(m.width / cellPx), ny = (int)ceilf(m.height / cellPx);
			auto edgeX = [&](int gx) { return roundf(x + (mapX(gx * 8192.0f) - left) * zoom); };
			auto edgeY = [&](int gy) { return roundf(y + (mapY(gy * 8192.0f) - top) * zoom); };
			u32 fog = C2D_Color32(8, 7, 6, 225);
			for (int gy = gy0; gy < gy0 + ny; gy++)
			{
				float y1 = edgeY(gy), y0 = edgeY(gy + 1);       // its south and north edges
				if (y1 < y || y0 > y + vh)
					continue;
				for (int gx = gx0; gx < gx0 + nx; gx++)
				{
					if (w.mapCellSeen(gx, gy))
						continue;
					int run = gx;
					while (run + 1 < gx0 + nx && !w.mapCellSeen(run + 1, gy))
						run++;
					float x0 = edgeX(gx), x1 = edgeX(run + 1);
					if (x1 > x && x0 < x + vw)
						uiRect(x0, y0, x1 - x0, y1 - y0, fog);
					gx = run;
				}
			}
			// The places the player has been to or learned about, each a marker (OpenMW's global map: a marker a
			// named place, the bigger the more cells it covers; its name under the mouse)
			if (mapLabelCells.size() != m.labels.size())
			{
				mapLabelCells.assign(m.labels.size(), 0);
				for (size_t k = 0; k < m.labels.size(); k++)
					for (auto& c : w.game.cells)
						mapLabelCells[k] += !c.interior && lower(c.name) == lower(m.labels[k].text);
			}
			for (size_t k = 0; k < m.labels.size(); k++)
			{
				const MapLabel& l = m.labels[k];
				float sx, sy;
				if (!w.mapKnown.count(lower(l.text)) || !toScreen(l.x, l.y, &sx, &sy))
					continue;
				float size = 8.0f + 2.0f * floorf(sqrtf((float)std::max(1, mapLabelCells[k])) - 1.0f) + (zoom >= 3.0f ? 2.0f : 0.0f);
				markers.push_back({ sx, sy, size, (int)k, l.text });
			}
		}
		if (outside)
		{
			drawDetected(toScreen);
			toScreen(w.player.feet[0], w.player.feet[1], &ax, &ay);
			arrow = true;
		}
	}
	else if (local && localMapCell == (outside ? -2 : w.current))
	{
		// The whole picture fits the view at the first zoom step; zoomed in, the map screen opens on the player and
		// pans (the HUD's follows the player)
		const float texSize = 256.0f;
		float s = fminf(vw, vh) / texSize * zoom, picture = texSize * s;
		float px = (w.player.feet[0] - localMin[0]) / localSize * texSize;
		float py = (localMin[1] + localSize - w.player.feet[1]) / localSize * texSize;
		float cx = px, cy = py;
		if (full)
		{
			if (!mapViewSet)
			{
				mapCX = px;
				mapCY = py;
				mapViewSet = true;
			}
			cx = mapCX;
			cy = mapCY;
		}
		float ox = picture <= vw ? x + (vw - picture) / 2.0f : fminf(x, fmaxf(x + vw - picture, x + vw / 2 - cx * s));
		float oy = picture <= vh ? y + (vh - picture) / 2.0f : fminf(y, fmaxf(y + vh - picture, y + vh / 2 - cy * s));
		ox = roundf(ox);
		oy = roundf(oy);
		if (full)
		{
			// (the view's centre as it ended up, kept on the picture)
			mapCX = picture <= vw ? texSize / 2 : (x + vw / 2 - ox) / s;
			mapCY = picture <= vh ? texSize / 2 : (y + vh / 2 - oy) / s;
		}
		// (the markers below don't need the picture)
		if (C3D_Tex* tex = rendererLocalMap())
		{
			Tex3DS_SubTexture sub = { 256, 256, 0.0f, 1.0f, 1.0f, 0.0f };
			C2D_Image img = { tex, &sub };
			C2D_DrawImageAt(img, ox, oy, 0.5f, nullptr, s, s);
		}
		// Doors, as Morrowind marks them; under the fog of war indoors, as OpenMW draws them (and no name shows)
		auto fog = !outside ? w.mapFog.find(lower(w.cells[w.current].name)) : w.mapFog.end();
		const int fn = World::kMapFog;
		w.forLoadedRefs([&](int i) {
			const Ref& r = w.refs[i];
			if (!r.hasDest || !r.visible() || r.destCell.empty())
				return;
			float tx = (r.pos[0] - localMin[0]) / localSize * texSize, ty = (localMin[1] + localSize - r.pos[1]) / localSize * texSize;
			float dx = roundf(ox + tx * s), dy = roundf(oy + ty * s);
			if (dx < x - 6 || dx > x + vw + 6 || dy < y - 6 || dy > y + vh + 6)
				return;
			if (fog != w.mapFog.end() && fog->second.size() == (size_t)(fn * fn))
			{
				int gi = std::max(0, std::min(fn - 1, (int)(tx / texSize * fn))), gj = std::max(0, std::min(fn - 1, (int)(ty / texSize * fn)));
				if (fog->second[gj * fn + gi] >= 12)
					return;
			}
			markers.push_back({ dx, dy, full ? 10.0f : 8.0f, i, w.game.cellLabel(r.destCell) });
		});
		drawDetected([&](float mx, float my, float* sx, float* sy) {
			*sx = ox + (mx - localMin[0]) / localSize * texSize * s;
			*sy = oy + (localMin[1] + localSize - my) / localSize * texSize * s;
			return *sx >= x && *sx < x + vw && *sy >= y && *sy < y + vh;
		});
		ax = ox + px * s;
		ay = oy + py * s;
		arrow = true;
		// the fog over the picture, the markers under it drawn first
		for (auto& mk : markers)
			drawMapMarker(mk.sx, mk.sy, mk.size, false);
		drawLocalFog(ox, oy, picture);
	}
	else
		uiTextCentered(x + vw / 2, y + vh / 2 - 8, 0.45f, col::textDim, outside && !local ? "No map" : "...");
	// A marker's name on the map screen: tap it, and it stays until the next tap (a finger dragged on the map pans it)
	int& pinned = local ? mapDoorTip : mapPlaceTip;
	if (full && in.tapped && in.tapY >= y && in.tapY < y + vh && in.tapX >= x && in.tapX < x + vw)
	{
		int tapped = -1;
		float best = 1e9f;
		for (auto& mk : markers)
		{
			float reach = fmaxf(11.0f, mk.size / 2 + 4);
			float ddx = in.tapX - mk.sx, ddy = in.tapY - mk.sy;
			if (fabsf(ddx) <= reach && fabsf(ddy) <= reach && ddx * ddx + ddy * ddy < best)
			{
				best = ddx * ddx + ddy * ddy;
				tapped = mk.id;
			}
		}
		pinned = tapped == pinned ? -1 : tapped;
		if (tapped >= 0)
			playSound(-1, "Menu Click");
	}
	if (!local)
		for (auto& mk : markers)
			drawMapMarker(mk.sx, mk.sy, mk.size, mk.id == pinned);
	else
		for (auto& mk : markers)
			if (mk.id == pinned)
				drawMapMarker(mk.sx, mk.sy, mk.size, true);
	if (arrow)
		drawPlayerArrow(ax, ay, w.player.yaw);
	for (auto& mk : markers)
		if (mk.id == pinned)
			drawMapTip(mk.name, mk.sx, mk.sy, x, y, vw);
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	uiFrame(x, y, vw, vh);
}

void Session::drawMap()
{
	static const float kMapZooms[kMapZoomSteps] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
	const UiInput& in = uiIn();
	bool outside = w.current >= 0 && !w.cells[w.current].interior;
	header((mapLocal ? w.game.gmstLabel("slocal", "Local") : w.game.gmstLabel("sworld", "World")) + " " +
		w.game.gmstLabel("smap", "Map"), w.game.cellLabel(w.cellName()));
	// L / R step the zoom (about the view's centre); the D-pad and circle pad pan, so does a finger dragged on the map
	if ((in.down & KEY_L) && mapZoomIdx > 0)
		mapZoomIdx--;
	if ((in.down & KEY_R) && mapZoomIdx < kMapZoomSteps - 1)
		mapZoomIdx++;
	float zoom = kMapZooms[mapZoomIdx];
	if (mapViewSet)
	{
		// (the view's centre is in map pixels on the world map, in local map picture pixels on the local map)
		float unit = mapLocal ? 184.0f / 256.0f * zoom : zoom;
		const float panSpeed = 160.0f;                  // screen pixels a second
		float step = panSpeed * frameDt / unit;
		if (in.held & KEY_LEFT) mapCX -= step;
		if (in.held & KEY_RIGHT) mapCX += step;
		if (in.held & KEY_UP) mapCY -= step;
		if (in.held & KEY_DOWN) mapCY += step;
		if (in.touching && in.touchY >= 22 && in.touchY < 206)
		{
			mapCX -= in.dragDX / unit;
			mapCY -= in.dragDY / unit;
		}
	}
	drawMapView(0, 22, 320, 184, zoom, true, mapLocal);
	// The other map (Morrowind's World / Local button), zoom - and +, Close. The D-pad pans, so it doesn't move
	// the buttons' focus
	uiConsume(KEY_LEFT | KEY_RIGHT);
	if (uiButton(4, 208, 148, 28, (mapLocal ? w.game.gmstLabel("sworld", "World") : w.game.gmstLabel("slocal", "Local")) + " " +
		w.game.gmstLabel("smap", "Map")))
	{
		playSound(-1, "Menu Click");
		mapLocal = !mapLocal;
		if (outside)
			mapLocalOutdoors = mapLocal;
		mapDoorTip = -1;
		mapPlaceTip = -1;
		mapViewSet = false;
	}
	// (the - and + as bars: the font's glyphs sit off centre in a button)
	auto zoomButton = [&](float bx, bool plus, bool enabled) {
		bool hit = uiButton(bx, 208, 36, 28, "", false, enabled);
		u32 c = enabled ? col::text : col::textDim;
		uiRect(bx + 12, 221, 12, 2, c);
		if (plus)
			uiRect(bx + 17, 216, 2, 12, c);
		return hit;
	};
	if (zoomButton(156, false, mapZoomIdx > 0))
		mapZoomIdx--;
	if (zoomButton(196, true, mapZoomIdx < kMapZoomSteps - 1))
		mapZoomIdx++;
	if (uiButton(236, 208, 80, 28, w.game.gmst("sclose", "Close")) || (in.down & KEY_B))
		closeScreen();
}
