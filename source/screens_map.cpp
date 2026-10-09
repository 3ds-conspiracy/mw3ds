// The world map and the interiors' local map
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "audio.h"
#include "linear.h"
#include "log.h"
#include "renderer.h"
#include "screens.h"
#include "session.h"

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

void Session::updateLocalMap()
{
	if (w.current < 0 || !w.cells[w.current].live)
		return;
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
	const Cell& cell = w.here();
	float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
	for (auto& b : cell.batches)
		for (int k = 0; k < 2; k++)
		{
			lo[k] = fminf(lo[k], b.bmin[k]);
			hi[k] = fmaxf(hi[k], b.bmax[k]);
		}
	if (lo[0] > hi[0])
		return;
	float size = fmaxf(hi[0] - lo[0], hi[1] - lo[1]) * 1.04f + 64.0f;
	float minX = (lo[0] + hi[0] - size) / 2.0f, minY = (lo[1] + hi[1] - size) / 2.0f;
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

// full: the map screen (pannable, the unvisited cells dark, detail tiles when zoomed in, labels); not full:
// the HUD's minimap, always the base picture round the player. local: the local map, else the world map
void Session::drawMapView(float x, float y, float vw, float vh, float zoom, bool full, bool local)
{
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)(240 - (y + vh)), (u32)(320 - (x + vw)), (u32)(240 - y), (u32)(320 - x));
	uiRect(x, y, vw, vh, C2D_Color32(12, 9, 6, 255));
	bool outside = w.current >= 0 && !w.cells[w.current].interior;
	float ax = 0, ay = 0;
	bool arrow = false;
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
			for (auto& l : m.labels)
			{
				float sx, sy;
				// Only places the player has been to or learned about
				if (!w.mapKnown.count(lower(l.text)))
					continue;
				if (toScreen(l.x, l.y, &sx, &sy))
				{
					float tw = uiTextWidth(l.text, 0.45f);
					uiRect(sx - tw / 2 - 3, sy - 8, tw + 6, 16, C2D_Color32(0, 0, 0, 140));
					uiTextCentered(sx, sy - 8, 0.45f, col::header, l.text);
				}
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
		// The whole picture fits the view; zoomed in, it follows the player
		const float texSize = 256.0f;
		float s = fminf(vw, vh) / texSize * zoom;
		float px = (w.player.feet[0] - localMin[0]) / localSize * texSize;
		float py = (localMin[1] + localSize - w.player.feet[1]) / localSize * texSize;
		float ox = x + (vw - texSize * s) / 2.0f, oy = y + (vh - texSize * s) / 2.0f;
		if (zoom > 1.0f)
		{
			ox = fminf(x, fmaxf(x + vw - texSize * s, x + vw / 2 - px * s));
			oy = fminf(y, fmaxf(y + vh - texSize * s, y + vh / 2 - py * s));
		}
		// (the markers below don't need the picture)
		if (C3D_Tex* tex = rendererLocalMap())
		{
			Tex3DS_SubTexture sub = { 256, 256, 0.0f, 1.0f, 1.0f, 0.0f };
			C2D_Image img = { tex, &sub };
			C2D_DrawImageAt(img, ox, oy, 0.5f, nullptr, s, s);
		}
		// Doors, as Morrowind marks them. On the map screen a tap on one shows where it leads (Morrowind's
		// tooltip over the marker; OpenMW's local map: the destination cell's name), a tap elsewhere hides it
		int tapped = -1;
		int tip = -1;
		float tipX = 0.0f, tipY = 0.0f;
		w.forLoadedRefs([&](int i) {
			const Ref& r = w.refs[i];
			if (!r.hasDest || !r.visible() || r.destCell.empty())
				return;
			float dx = roundf(ox + (r.pos[0] - localMin[0]) / localSize * texSize * s);
			float dy = roundf(oy + (localMin[1] + localSize - r.pos[1]) / localSize * texSize * s);
			if (dx < x - 6 || dx > x + vw + 6 || dy < y - 6 || dy > y + vh + 6)
				return;
			// a gold box with a dark edge (the converted door_icon comes out a faint grey)
			float half = full ? 5.0f : 3.0f;
			uiRect(dx - half, dy - half, half * 2, half * 2, C2D_Color32(0, 0, 0, 220));
			uiRect(dx - half + 1, dy - half + 1, half * 2 - 2, half * 2 - 2, col::header);
			// a finger-sized place to tap round the small marker
			if (full && uiHit(dx - 11, dy - 11, 22, 22))
				tapped = i;
			if (full && i == mapDoorTip)
			{
				tip = i;
				tipX = dx;
				tipY = dy;
			}
		});
		if (full && uiIn().tapped && uiIn().tapY >= y && uiIn().tapY < y + vh)
		{
			mapDoorTip = tapped == mapDoorTip ? -1 : tapped;
			if (tapped >= 0)
				playSound(-1, "Menu Click");
		}
		drawDetected([&](float mx, float my, float* sx, float* sy) {
			*sx = ox + (mx - localMin[0]) / localSize * texSize * s;
			*sy = oy + (localMin[1] + localSize - my) / localSize * texSize * s;
			return *sx >= x && *sx < x + vw && *sy >= y && *sy < y + vh;
		});
		ax = ox + px * s;
		ay = oy + py * s;
		arrow = true;
		if (tip >= 0 && tip == mapDoorTip)
		{
			drawPlayerArrow(ax, ay, w.player.yaw);
			arrow = false;
			// the name above the marker, kept inside the view
			const std::string& name = w.refs[tip].destCell;
			float tw = uiTextWidth(name, 0.45f), lh = uiLineHeight(0.45f);
			float lx = fminf(fmaxf(tipX - tw / 2 - 3, x + 2), x + vw - tw - 8);
			float ly = tipY - 8 - lh - 4 < y + 2 ? tipY + 8 : tipY - 8 - lh - 4;
			uiRect(lx, ly, tw + 6, lh + 4, C2D_Color32(0, 0, 0, 200));
			uiFrame(lx, ly, tw + 6, lh + 4);
			uiText(lx + 3, ly + 2, 0.45f, col::header, name);
		}
	}
	else
		uiTextCentered(x + vw / 2, y + vh / 2 - 8, 0.45f, col::textDim, outside && !local ? "No map" : "...");
	if (arrow)
		drawPlayerArrow(ax, ay, w.player.yaw);
	C2D_Flush();
	C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
	uiFrame(x, y, vw, vh);
}

void Session::drawMap()
{
	static const float kMapZooms[kMapZoomSteps] = { 1.0f, 1.5f, 2.0f, 3.0f, 4.0f };
	const UiInput& in = uiIn();
	bool outside = w.current >= 0 && !w.cells[w.current].interior;
	header(mapLocal ? "Local Map" : "World Map", w.cellName());
	// L / R step the zoom (about the view's centre); the D-pad and circle pad pan, so does a finger dragged on the map
	if ((in.down & KEY_L) && mapZoomIdx > 0)
		mapZoomIdx--;
	if ((in.down & KEY_R) && mapZoomIdx < kMapZoomSteps - 1)
		mapZoomIdx++;
	float zoom = kMapZooms[mapZoomIdx];
	if (!mapLocal && mapViewSet)
	{
		const float panSpeed = 160.0f;                  // screen pixels a second
		float step = panSpeed * frameDt / zoom;
		if (in.held & KEY_LEFT) mapCX -= step;
		if (in.held & KEY_RIGHT) mapCX += step;
		if (in.held & KEY_UP) mapCY -= step;
		if (in.held & KEY_DOWN) mapCY += step;
		if (in.touching && in.touchY >= 22 && in.touchY < 206)
		{
			mapCX -= in.dragDX / zoom;
			mapCY -= in.dragDY / zoom;
		}
	}
	drawMapView(0, 22, 320, 184, zoom, true, mapLocal);
	// The other map (Morrowind's World / Local button), zoom - and +, Close. The D-pad pans, so it doesn't move
	// the buttons' focus
	uiConsume(KEY_LEFT | KEY_RIGHT);
	if (uiButton(4, 208, 148, 28, mapLocal ? "World Map" : "Local Map"))
	{
		playSound(-1, "Menu Click");
		mapLocal = !mapLocal;
		if (outside)
			mapLocalOutdoors = mapLocal;
		mapDoorTip = -1;
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
