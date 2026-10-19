// Books and scrolls: pages, pictures, take
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mwbase/session.hpp"

// A picture a book's text names (BOOKART\x.dds), from the converted book art
static UiPicture bookPicture(const std::string& key, void* ctx)
{
	const GameData& g = *(const GameData*)ctx;
	std::string k = lower(key);
	if (k.rfind("bookart\\", 0) != 0)
		k = "bookart\\" + k;
	// (the text names them .tga, the archives hold them as .dds)
	size_t dot = k.rfind('.');
	if (dot != std::string::npos && dot > k.rfind('\\'))
		k = k.substr(0, dot) + ".dds";
	auto it = g.art.bookart.find(k);
	UiPicture p;
	if (it != g.art.bookart.end())
		p = { it->second.file, it->second.w, it->second.h };
	return p;
}

// One of the reading screen's buttons, Morrowind's own (tx_menubook_<name>_idle / _over / _pressed) at 3/4 of its
// size, centred on cx, cy; lit: the one A presses. Data without them gets a plain button
static bool bookButton(const GameData& g, const char* name, const std::string& label, float cx, float cy, float w,
	bool lit)
{
	const UiInput& in = uiIn();
	float bw = w * 0.75f, bh = 24.0f, x = roundf(cx - bw / 2), y = roundf(cy - bh / 2);
	bool held = in.touching && in.touchX >= x && in.touchX < x + bw && in.touchY >= y && in.touchY < y + bh;
	auto it = g.art.book.find(std::string(name) + (held ? "_pressed" : lit ? "_over" : "_idle"));
	if (it == g.art.book.end() || !uiArt(it->second.file, it->second.w, it->second.h, x, y, bw, bh))
		return uiButton(x, y, bw, bh, label, lit);
	return uiHit(x, y, bw, bh);
}

// Reading (OpenMW's BookWindow and ScrollWindow, their layouts scaled to the bottom screen's width): a book is the
// open book with two pages of text side by side, turned by Prev / Next (L / R, the D-pad), a page number under
// each; a scroll is the scroll with its text running down it. Take (A) when it lies in the world, Close (B)
void Session::drawBook()
{
	const Object* o = bookRef >= 0 ? w.refs[bookRef].obj : w.game.object(bookItem);
	if (!o)
	{
		closeScreen();
		return;
	}
	readBook(o);
	const UiInput& in = uiIn();
	bool canTake = bookRef >= 0 && w.refs[bookRef].visible();
	std::string text = dialogueSubstitute(w, o->text, -1);
	auto art = [&](const char* key, float x, float y, float aw, float ah) {
		auto it = w.game.art.book.find(key);
		return it != w.game.art.book.end() && uiArt(it->second.file, it->second.w, it->second.h, x, y, aw, ah);
	};
	// (the text is black on the parchment, as Morrowind's books; light on the plain panel without it)
	u32 ink = C2D_Color32(0, 0, 0, 255);
	bool take = false, close = false;
	if (o->scroll)
	{
		// the scroll window is 512 x 372: Take and Close at its top corners, the text in 60, 84, 410 x 235
		const float k = 320.0f / 512.0f, top = roundf((240.0f - 372.0f * k) / 2);
		if (!art("scroll", 0, top, 320, 372.0f * k))
		{
			uiPanel(4, 4, 312, 232);
			ink = col::text;
		}
		uiTextBoxPictures(scroll, 60 * k, top + 84 * k, 410 * k, 235 * k, text, bookPicture, &w.game, 0.45f, ink);
		take = canTake && bookButton(w.game, "take", w.game.gmst("stake", "Take"), 76 * k, top + 30 * k, 64, true);
		close = bookButton(w.game, "close", w.game.gmst("sclose", "Close"), 290, top + 36 * k, 64, false);
	}
	else
	{
		// the book window is 584 x 398: pages 250 x 328 at 30, 15 and 310, 15, the buttons and page numbers along
		// its bottom edge at y 358
		const float k = 320.0f / 584.0f, top = roundf((240.0f - 398.0f * k) / 2), scale = 0.4f;
		if (!art("book", 0, top, 320, 398.0f * k))
		{
			uiPanel(4, 4, 312, 232);
			ink = col::text;
		}
		if (text != bookPagesOf)
		{
			bookPages = uiPaginate(text, 250 * k, 328 * k, scale, bookPicture, &w.game);
			bookPagesOf = text;
			bookSpread = 0;
		}
		int pages = (int)bookPages.size();
		bool prev = bookSpread > 0, next = (bookSpread + 1) * 2 < pages;
		for (int side = 0; side < 2; side++)
		{
			int p = bookSpread * 2 + side;
			if (p >= pages)
				break;
			float px = (side ? 310 : 30) * k;
			uiDrawPage(bookPages[p], px, top + 15 * k, 250 * k, scale, ink);
			uiTextCentered(px + 125 * k, top + 358 * k, scale, ink, std::to_string(p + 1));
		}
		float by = top + 374 * k;
		int turn = 0;
		if (prev && (bookButton(w.game, "prev", w.game.gmst("sprev", "Prev"), 229 * k, by, 48, false)
			|| (in.down & (KEY_L | KEY_LEFT))))
			turn = -1;
		if (next && (bookButton(w.game, "next", w.game.gmst("snext", "Next"), 354 * k, by, 48, false)
			|| (in.down & (KEY_R | KEY_RIGHT))))
			turn = 1;
		if (turn)
		{
			// (OpenMW: "book page2" forward, "book page" back)
			playSound(-1, turn > 0 ? "book page2" : "book page");
			bookSpread += turn;
		}
		take = canTake && bookButton(w.game, "take", w.game.gmst("stake", "Take"), 72 * k, by, 64, true);
		close = bookButton(w.game, "close", w.game.gmst("sclose", "Close"), 512 * k, by, 48, false);
	}
	if (canTake && (take || (in.down & KEY_A)))
	{
		int ref = bookRef;
		closeScreen();
		pickUp(ref);
	}
	else if (close || (in.down & KEY_B))
	{
		playSound(-1, "Book Close");
		closeScreen();
	}
}
