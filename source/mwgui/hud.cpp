// The HUD: the top screen's overlay (bars, crosshair, notices), the bottom screen's quick strip, hit indicators
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mwbase/session.hpp"
#include <cstdio>
#include "mwgui/windowmanagerimp.hpp"
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include <malloc.h>

void Session::addIndicator(const std::string& text, u32 rgb)
{
	if (!hitIndicators)
		return;
	// a random spot within 10% x 5% of the screen around the middle (the mod's OFFSET_RANGE)
	float ox = ((rand() % 1000) / 1000.0f - 0.5f) * 0.1f * 400.0f;
	float oy = ((rand() % 1000) / 1000.0f - 0.5f) * 0.05f * 240.0f;
	indicators.push_back({ text, rgb, 200.0f + ox, 110.0f + oy, 1.0f });
}

// Drawn with the HUD (the crosshair's depth); 0.1 of the screen's height a second upward, fading out
void Session::drawIndicators(float dt)
{
	for (size_t i = 0; i < indicators.size();)
	{
		Indicator& d = indicators[i];
		float alpha = fmaxf(0.0f, fminf(1.0f, d.timer));
		u32 col = C2D_Color32((d.rgb >> 16) & 255, (d.rgb >> 8) & 255, d.rgb & 255, (u8)(alpha * 255.0f));
		uiTextCentered(d.x + aimShift, d.y, 0.55f, col, d.text);
		d.y -= 0.1f * 240.0f * dt;
		d.timer -= dt;
		if (d.timer <= 0.0f)
			indicators.erase(indicators.begin() + i);
		else
			i++;
	}
}

// ---- top screen overlay

// The red wash when the player is hurt: a screen effect, drawn with the HUD hidden too (a test's HUD:off, OpenMW's hidden HUD
// still shows its own)
void Session::drawHurtFlash()
{
	if (hurtFlash <= 0.0f)
		return;
	// Red at the edges of the screen, fading to nothing toward the middle (OpenMW's hit overlay is a vignette too), not a
	// wash over all of it: four bands, each a quad of two triangles with the colour at its outer side only
	extern int g_uiDraws;
	const u32 edge = C2D_Color32(200, 0, 0, (u8)(hurtFlash * 110)), none = C2D_Color32(200, 0, 0, 0);
	auto quad = [&](float x, float y, float w, float h, u32 tl, u32 tr, u32 bl, u32 br) {
		g_uiDraws++;
		C2D_DrawTriangle(x, y, tl, x + w, y, tr, x, y + h, bl, 0.5f);
		C2D_DrawTriangle(x, y + h, bl, x + w, y, tr, x + w, y + h, br, 0.5f);
	};
	quad(0, 0, 400, 50, edge, edge, none, none);          // top
	quad(0, 190, 400, 50, none, none, edge, edge);        // bottom
	quad(0, 0, 70, 240, edge, none, edge, none);          // left
	quad(330, 0, 70, 240, none, edge, none, edge);        // right
}

// The werewolf's view: the fur at the screen's edges over the whole top screen, in first person (OpenMW's
// setWerewolfOverlay, shown with the first-person view), under every other mark. Part of the view, not the HUD: drawn
// with the HUD hidden too
void Session::drawWerewolfView()
{
	if (!w.stats.werewolf || thirdPerson)
		return;
	auto fur = w.game.art.hud.find("werewolf_overlay");
	if (fur != w.game.art.hud.end())
		uiArt(fur->second.file, fur->second.w, fur->second.h, 0, 0, 400, 240);
}

// With no weapon, a werewolf's weapon box shows the claw (OpenMW's unsetSelectedWeapon: tx_werewolf_hand); in their
// own form the 3DS's "Fist" stays
const char* Session::hudHandArt()
{
	if (!w.stats.werewolf)
		return nullptr;
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it))
				if (o->recType == REC_WEAP)
					return nullptr;
	return w.game.art.hud.count("werewolf_hand") ? "werewolf_hand" : nullptr;
}

void Session::drawTop()
{
	const UiInput& in = uiIn();
	(void)in;
	drawWerewolfView();
	// (aimShift: in 3D, the crosshair and the label sit at the depth of what's aimed at)
	float ax = aimShift;
	if (!indicators.empty())
	{
		// both eyes draw them; the time moves on once a frame (the left eye's pass)
		static u64 last = 0;
		u64 now = osGetTime();
		float dt = last && hudEye == 0 ? fminf(0.1f, (now - last) / 1000.0f) : 0.0f;
		if (hudEye == 0)
			last = now;
		drawIndicators(dt);
	}
	if (!menuOpen())
	{
		uiRect(199 + ax, 117, 3, 7, C2D_Color32(255, 255, 255, 150));
		uiRect(197 + ax, 119, 7, 3, C2D_Color32(255, 255, 255, 150));
	}
	if (target >= 0 && !menuOpen())
	{
		// (a long name, "Hut Door - Ald-ruhn, Tiras Sadus: General Merchandise", wraps over lines inside the screen)
		std::vector<std::string> lines = targetPopupLines(target);
		float lh = uiLineHeight(0.55f), tw = 0.0f;
		for (auto& l : lines)
			tw = fmaxf(tw, uiTextWidth(l, 0.55f));
		float h = lines.size() * lh + 2;
		uiRect(200 + ax - tw / 2 - 6, 132, tw + 12, h + 2, col::border);
		uiRect(200 + ax - tw / 2 - 5, 133, tw + 10, h, col::panel);
		for (size_t i = 0; i < lines.size(); i++)
			uiTextCentered(200 + ax, 134 + i * lh, 0.55f, w.shownOwned(target) ? col::health : col::text, lines[i]);
	}
	// Notices, each wrapped to the screen's width ("Release Identification has been removed from your inventory."
	// ran off it)
	float y = 4;
	notesRight = 0.0f;
	// (while a menu pauses the world, short of the "Paused" label in the top right corner)
	float notesW = worldPaused() ? 384 - uiTextWidth("Paused", 0.5f) - 16 : 384;
	for (auto& n : notes)
		for (auto& line : uiWrap(n.text, notesW, 0.5f))
		{
			float tw = uiTextWidth(line, 0.5f);
			uiRect(4, y, tw + 8, 16, C2D_Color32(0, 0, 0, 150));
			uiText(8, y, 0.5f, col::header, line);
			notesRight = fmaxf(notesRight, 8 + tw);
			y += 18;
		}
	if (w.time < subtitleUntil && !subtitle.empty())
	{
		std::vector<std::string> lines = uiWrap(subtitle, 380, 0.5f);
		float lh = uiLineHeight(0.5f);
		float top = 236 - lines.size() * lh;
		uiRect(4, top - 2, 392, lines.size() * lh + 4, C2D_Color32(0, 0, 0, 220));   // the world barely shows through
		for (size_t i = 0; i < lines.size(); i++)
			uiTextCentered(200, top + i * lh, 0.5f, col::header, lines[i]);
	}
	// Sneaking and nobody has noticed: Morrowind's sneak eye by the crosshair
	if (hidden && !menuOpen())
	{
		auto eye = w.game.art.hud.find("sneak");
		if (eye == w.game.art.hud.end() || !uiArt(eye->second.file, eye->second.w, eye->second.h, 170, 158, 20, 20))
			uiTextCentered(200, 158, 0.45f, col::header, "(hidden)");
	}
	// Wind-up of a blow under the crosshair
	if (attackCharge >= 0.0f && !menuOpen())
	{
		uiRect(170, 150, 60, 5, C2D_Color32(0, 0, 0, 160));
		uiRect(170, 150, 60 * attackCharge, 5, col::header);
	}
	// Health of whoever was hit last
	if (enemyRef >= 0 && w.time < enemyUntil && w.active(enemyRef))
	{
		const Ref& e = w.refs[enemyRef];
		uiRect(150, 222, 100, 10, C2D_Color32(0, 0, 0, 170));
		uiRect(151, 223, 98 * fmaxf(0.0f, e.health) / fmaxf(1.0f, e.healthMax), 8, col::health);
	}
	drawHurtFlash();
	// The HUD's tapped active-effect icon: its tooltip for a few seconds, the world going on (OpenMW's is a hover)
	if (!menuOpen() && w.time < effectTipUntil && !effectTip.empty())
		for (size_t i = 0; i < effectTip.size(); i++)
			tip.emplace_back(effectTip[i], i == 0 ? col::header : col::text);
	// The selected item's or spell's tooltip
	if ((menuOpen() || w.time < effectTipUntil) && !tip.empty())
	{
		const float scale = 0.45f, lh = uiLineHeight(scale);
		float tw = 0.0f;
		for (auto& l : tip)
			tw = fmaxf(tw, uiTextWidth(l.first, l.second == col::header ? 0.52f : scale));
		tw = fminf(tw, 380.0f);
		float th = tip.size() * lh + 8, x = 200 - tw / 2 - 8, y = 120 - th / 2;
		uiRect(x - 1, y - 1, tw + 18, th + 2, col::border);
		uiRect(x, y, tw + 16, th, C2D_Color32(0, 0, 0, 215));
		for (size_t i = 0; i < tip.size(); i++)
			uiTextCentered(200, y + 4 + i * lh, tip[i].second == col::header ? 0.52f : scale, tip[i].second, tip[i].first);
	}
	// Character creation's box: a picture, or the vitals, centred and framed
	if (menuOpen() && topModal.kind != TopModal::NONE)
	{
		const float lh = uiLineHeight(0.5f);
		float bw, bh, pw = 0, ph = 0;
		if (topModal.kind == TopModal::ART && topModal.art.w > 0 && topModal.art.h > 0)
		{
			// (up to 240 x 150, its own proportions; a small picture is not blown up past twice its size)
			float s = fminf(fminf(240.0f / topModal.art.w, 150.0f / topModal.art.h), 2.0f);
			pw = topModal.art.w * s;
			ph = topModal.art.h * s;
			// (the frame is thick: words get room inside it)
			bw = fmaxf(pw + 24, fmaxf(uiTextWidth(topModal.title, 0.5f), uiTextWidth(topModal.caption, 0.5f)) + 40);
			bh = ph + 24 + (topModal.title.empty() ? 0 : lh + 4) + (topModal.caption.empty() ? 0 : lh + 4);
		}
		else
		{
			bw = topModal.attributes ? 288 : 248;
			bh = 3 * 24 + 24 + (topModal.title.empty() ? 0 : lh + 4) + (topModal.attributes ? 4 * lh + 10 : 0);
		}
		float bx = roundf(200 - bw / 2), by = roundf(120 - bh / 2), y = by + 12;
		uiRect(bx, by, bw, bh, C2D_Color32(0, 0, 0, 235));
		uiFrame(bx, by, bw, bh, "menu_thin_border", col::border);
		if (!topModal.title.empty())
		{
			uiTextCentered(200, y, 0.5f, col::header, topModal.title);
			y += lh + 4;
		}
		if (topModal.kind == TopModal::ART)
		{
			float px = roundf(200 - pw / 2);
			uiArt(topModal.art.file, topModal.art.w, topModal.art.h, px, y, pw, ph);
			// a frame of its own round the picture, as OpenMW's class and birthsign pictures have
			uiFrame(px - 2, y - 2, pw + 4, ph + 4, "menu_thin_border", col::border);
			y += ph + 4;
			if (!topModal.caption.empty())
				uiTextCentered(200, y, 0.5f, col::text, topModal.caption);
		}
		else
		{
			static const char* kName[3] = { "shealth", "smagic", "sfatigue" };
			static const char* kText[3] = { "Health", "Magicka", "Fatigue" };
			static const u32 kColor[3] = { C2D_Color32(190, 40, 30, 255), C2D_Color32(40, 70, 200, 255), C2D_Color32(30, 150, 50, 255) };
			for (int k = 0; k < 3; k++)
			{
				float ry = y + k * 24;
				uiText(bx + 12, ry + 3, 0.5f, col::header, w.game.gmstLabel(kName[k], kText[k]));
				uiBar(bx + 84, ry + 2, bw - 84 - 12, 16, topModal.vitals[k][1] > 0 ? (float)topModal.vitals[k][0] / topModal.vitals[k][1] : 0.0f, kColor[k]);
				// (centred in the bar as the HUD's are: from its top they ran out of its bottom)
				uiTextCentered(bx + 84 + (bw - 96) / 2, ry + 2 + 8 - uiLineHeight(0.42f) / 2, 0.42f, col::text,
					std::to_string(topModal.vitals[k][0]) + "/" + std::to_string(topModal.vitals[k][1]));
			}
			// the attributes, two columns of four (strength .. luck down the first)
			if (topModal.attributes)
			{
				float ay = y + 3 * 24 + 6;
				uiRect(bx + 12, ay - 4, bw - 24, 1, col::border);
				for (int i = 0; i < 8; i++)
				{
					float x = bx + 12 + (i / 4) * (bw - 24) / 2, ry = ay + (i % 4) * lh;
					uiText(x, ry, 0.5f, col::text, attrName(w, i));
					uiTextRight(x + (bw - 24) / 2 - 10, ry, 0.5f, col::header, std::to_string(w.stats.attributes[i]));
				}
			}
		}
	}
	// set by the bottom screen each frame; kept until both eyes have drawn it (clearing it after the first
	// pass left the tooltip in one eye only)
	if (hudEye == hudEyes - 1)
	{
		tip.clear();
		topModal = TopModal();
	}
	// A menu has stopped the world (both eyes draw it: it comes from the screen, not from a timer)
	if (worldPaused())
	{
		float pw = uiTextWidth("Paused", 0.5f);
		uiRect(396 - pw - 12, 4, pw + 10, 16, C2D_Color32(0, 0, 0, 150));
		uiTextRight(392, 4, 0.5f, col::header, "Paused");
	}
	// Breath under water: a bar that empties, then drowning
	if (breath >= 0.0f)
	{
		float full = w.game.gmstf("fholdbreathtime", 20.0f);
		uiRect(150, 8, 100, 8, C2D_Color32(0, 0, 0, 150));
		uiRect(151, 9, 98 * fmaxf(0.0f, breath) / fmaxf(1.0f, full), 6, C2D_Color32(120, 170, 230, 220));
		uiTextCentered(200, 18, 0.4f, breath > 0.0f ? col::text : col::health, breath > 0.0f ? w.game.gmstLabel("sbreath", "Breath") : "Drowning!");
	}
	if (fade > 0.0f)
		uiRect(0, 0, 400, 240, C2D_Color32(0, 0, 0, (u8)(fminf(fade, 1.0f) * 255)));
	if (wantName)
		uiTextCentered(200, 110, 0.6f, col::header, "Enter your name");
}

// The four quick keys as slots under the minimap (Up, Right, Down, Left), big enough to tap: the item's icon or the
// spell's first letters; a tap uses the slot as its D-pad press does (GoldenEye's D-pad moves: the taps are its quick keys)
void Session::drawQuickStrip()
{
	static const char* dirs[4] = { "U", "R", "D", "L" };
	const float kSlot = 26.0f;
	for (int k = 0; k < 4; k++)
	{
		float x = 190 + k * 30, y = 179;
		const std::string& q = w.stats.quickKeys[k];
		bool isSpell = q.compare(0, 6, "magic:") == 0 && q.compare(0, 11, "magic:item:") != 0;
		std::string id = q.compare(0, 4, "inv:") == 0 ? q.substr(4) : q.compare(0, 11, "magic:item:") == 0 ? q.substr(11)
			: q.compare(0, 6, "magic:") == 0 ? q.substr(6) : q;
		const Object* o = q.empty() || isSpell ? nullptr : w.game.object(id);
		uiRect(x, y, kSlot, kSlot, col::panel);
		if (!q.empty() && !(o && uiIcon(o->iconIx, x + 1, y + 1, kSlot - 2.0f)))
		{
			std::string name = isSpell ? (w.game.spells.count(id) ? w.game.spells[id].name : id) : o ? o->name.str() : id;
			uiTextCentered(x + kSlot / 2, y + kSlot / 2 - uiLineHeight(0.38f) / 2, 0.38f, isSpell ? col::link : col::textDim, name.substr(0, 3));
		}
		uiFrame(x, y, kSlot, kSlot);
		uiText(x + 2, y, 0.3f, col::textOver, dirs[k]);
		if (uiHit(x, y, kSlot, kSlot) && w.controlsEnabled && !playerDead)
			useQuickKey(k);
	}
}

// The active effects the HUD shows an icon for: spells, potions, blessings, and abilities and worn constant effects
// (the fortifies folded into the stats too: OpenMW lists every active effect), the same effect once
std::vector<ActiveEffect> Session::hudEffects()
{
	std::vector<ActiveEffect> shown;
	auto add = [&](const ActiveEffect& e, float remaining) {
		for (auto& sh : shown)
			if (sh.effect == e.effect && sh.attribute == e.attribute && sh.skill == e.skill)
			{
				sh.magnitude += e.magnitude;
				sh.remaining = fmaxf(sh.remaining, remaining);
				return;
			}
		ActiveEffect one;
		one.effect = e.effect;
		one.attribute = e.attribute;
		one.skill = e.skill;
		one.magnitude = e.magnitude;
		one.remaining = remaining;
		one.source = e.source;
		shown.push_back(one);
	};
	for (auto& e : w.effects)
		add(e, e.remaining);
	for (auto& e : w.abilitiesShown)
		add(e, 1e9f);
	return shown;
}

void Session::drawHud()
{
	const PlayerStats& s = w.stats;
	std::string sub;
	for (auto& r : w.game.races)
		if (lower(r.id) == lower(s.race))
			sub = r.name;
	if (const ClassDef* c = w.playerClass())
		sub += " " + c->name;
	header(s.name, sub);

	// Health, magicka, fatigue (Morrowind's colours), the numbers inside the bars
	float cur[3] = { s.health, s.magicka, s.fatigue }, max[3] = { s.healthMax, s.magickaMax, s.fatigueMax };
	u32 colors[3] = { col::health, col::magicka, col::fatigue };
	for (int i = 0; i < 3; i++)
	{
		float y = 27 + i * 15;
		uiBar(6, y, 174, 12, max[i] > 0 ? cur[i] / max[i] : 0, colors[i]);
		char buf[32];
		snprintf(buf, sizeof(buf), "%d/%d", (int)cur[i], (int)max[i]);
		uiTextCentered(93, y + 6 - uiLineHeight(0.36f) / 2, 0.36f, col::textPressed, buf);
	}

	// Weapon and spell, as the boxes by Morrowind's bars
	const InventoryItem* weapon = nullptr;
	for (auto& it : w.inventory)
		if (it.equipped)
			if (const Object* o = w.game.object(it))
				if (o->recType == REC_WEAP)
					weapon = &it;
	const Object* wo = weapon ? w.game.object(*weapon) : nullptr;
	float by = 75;
	uiRect(6, by, 36, 36, col::panel);
	if (const char* hand = wo ? nullptr : hudHandArt())
	{
		const auto& art = w.game.art.hud.find(hand)->second;
		uiArt(art.file, art.w, art.h, 8, by + 2, 32, 32);
	}
	else if (!(wo && uiIcon(wo->iconIx, 8, by + 2)))
		uiTextCentered(24, by + 18 - uiLineHeight(0.36f) / 2, 0.36f, col::textDim, wo ? wo->name.str().substr(0, 3) : "Fist");
	uiFrame(6, by, 36, 36);
	uiText(48, by + 1, 0.42f, col::text, wo ? wo->name.str() : w.game.gmstLabel("sskillhandtohand", "Hand-to-hand"));
	std::string spell = s.selectedSpell.empty() ? "" : w.game.spells[s.selectedSpell].name;
	uiText(48, by + 18, 0.42f, spell.empty() ? col::textDim : col::link, spell.empty() ? "No spell" : spell);

	// The active effects as Morrowind's HUD shows them (hudEffects); a tap shows what it is, how strong and for how
	// long (OpenMW's tooltip on hover)
	std::vector<ActiveEffect> shown = hudEffects();
	hudEffectIcons = 0;
	for (size_t k = 0; k < shown.size() && k < 10; k++)
	{
		auto me = w.game.magicEffects.find(shown[k].effect);
		float ix = 6 + k * 17;
		if (me == w.game.magicEffects.end() || me->second.iconIx < 0 || !uiIcon(me->second.iconIx, ix, 114, 15.0f))
			continue;
		hudEffectIcons++;
		if (uiHit(ix, 114, 16, 16))
		{
			const ActiveEffect& sh = shown[k];
			int flags = me->second.flags;
			std::string line = effectLabel(sh.effect, sh.skill, sh.attribute);
			if (!(flags & 0x8))
				line += " " + std::to_string((int)roundf(sh.magnitude)) + " pts";
			std::string left = sh.remaining > 1e8f ? "Constant" : sh.remaining >= 120.0f
				? std::to_string((int)(sh.remaining / 60.0f)) + " min left" : std::to_string((int)ceilf(sh.remaining)) + " s left";
			effectTip = { line, left + " (" + sh.source + ")" };
			effectTipUntil = w.time + 3.0f;
			playSound(-1, "Menu Click");
		}
	}

	// Messages
	float y = 133;
	for (auto& n : notes)
	{
		for (auto& line : uiWrap(n.text, 174, 0.42f))
		{
			if (y > 164)
				break;
			uiText(6, y, 0.42f, col::text, line);
			y += uiLineHeight(0.42f);
		}
	}

	// Minimap: tap it for the map
	bool mapOk = !w.game.map.file.empty() && ((w.menusEnabled & MENU_MAP) || !w.game.chargen);
	drawMapView(186, 26, 128, 128, 1.0f, false, w.current >= 0 && w.cells[w.current].interior);
	if (mapOk && uiHit(186, 26, 128, 128))
		openScreen(SCR_MAP);
	std::string here = w.game.cellLabel(w.cellName());
	hudCaption = here.size() > 26 ? here.substr(0, 25) + "..." : here;
	uiTextCentered(250, 155, 0.38f, col::textDim, hudCaption);

	if (target >= 0)
	{
		// two lines for a long name, a little smaller and higher, and the attack hint below them moves down
		float s = 0.45f;
		std::vector<std::string> use = targetPromptLines(target, &s);
		promptRows = (int)use.size();
		float lh = uiLineHeight(s);
		for (size_t i = 0; i < use.size(); i++)
			uiText(6, (use.size() > 1 ? 166 : 172) + i * lh, s, col::link, use[i]);
	}
	else
		promptRows = 1;
	if (w.fightingEnabled)
	{
		bool xbox = g_controlLayout == CONTROLS_XBOX || g_controlLayout == CONTROLS_GOLDENEYE;   // (GoldenEye: the Xbox buttons)
		std::string hint = w.stats.selectedSpell.empty() ? (xbox ? "R: attack (hold to wind up)" : "X: attack (hold to wind up)")
			: std::string(xbox ? "R: attack   L: cast " : "X: attack   Y: cast ") + w.game.spells[w.stats.selectedSpell].name;
		// Leave the quick key strip (from x 190) clear, and GoldenEye's 1st / 3rd button (from x 132)
		float room = g_controlLayout == CONTROLS_GOLDENEYE ? 122.0f : 180.0f;
		while (hint.size() > 3 && uiTextWidth(hint, 0.38f) > room)
			hint = hint.substr(0, hint.size() - 4) + "...";
		uiText(6, promptRows > 1 ? 192 : 190, 0.38f, col::textDim, hint);
	}
	drawQuickStrip();
	uiTextCentered(250, 166, 0.36f, col::textDim, dateText());
	// the bounty on the minimap's foot, the frame rate in its top corner (the strip took their place)
	if (w.bounty > 0)
	{
		std::string b = w.game.gmstLabel("sbounty", "Bounty") + " " + std::to_string(w.bounty);
		uiRect(312 - uiTextWidth(b, 0.4f) - 4, 140, uiTextWidth(b, 0.4f) + 4, 13, C2D_Color32(0, 0, 0, 170));
		uiTextRight(311, 140, 0.4f, col::health, b);
	}
	char perf[64];
	snprintf(perf, sizeof(perf), "%.0f fps", fps);
	uiTextRight(312, 27, 0.34f, col::textDim, perf);
	bool inv = w.menusEnabled & MENU_INVENTORY, stats = w.menusEnabled & MENU_STATS;
	if (uiButton(2, 208, 51, 28, "Items", false, inv))
		openScreen(SCR_INVENTORY);
	if (uiButton(55, 208, 51, 28, w.game.gmstLabel("smagicmenu", "Magic"), false, (w.menusEnabled & MENU_MAGIC) || !w.game.chargen))
		openScreen(SCR_MAGIC);
	if (uiButton(108, 208, 51, 28, w.game.gmstLabel("sjournal", "Journal"), false, !w.journal.empty()))
		openScreen(SCR_JOURNAL);
	if (uiButton(161, 208, 51, 28, w.game.gmstLabel("smap", "Map"), false, !w.game.map.file.empty() && ((w.menusEnabled & MENU_MAP) || !w.game.chargen)))
		openScreen(SCR_MAP);
	if (uiButton(214, 208, 51, 28, w.game.gmstLabel("sstats", "Stats"), false, stats))
		openScreen(SCR_STATS);
	if (uiButton(267, 208, 51, 28, "Saves", false, !wantName))
	{
		saveList = savesList(dataDir);
		openScreen(SCR_SAVES);
	}
}

// fades back to clear after travel; scripts can hold it (FadeOut) or bring it back slowly (FadeIn)
void Session::updateFadeAndNotes(float dt)
{
	if (fade > fadeTarget)
		fade = fmaxf(fadeTarget, fade - dt * fadeRate);
	else if (fade < fadeTarget)
		fade = fminf(fadeTarget, fade + dt * fadeRate);
	if (fade == fadeTarget && fadeTarget == 0.0f)
		fadeRate = 2.5f;
	for (size_t i = 0; i < notes.size();)
		if (w.time > notes[i].until)
			notes.erase(notes.begin() + i);
		else
			i++;
}
