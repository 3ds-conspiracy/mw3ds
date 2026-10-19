// Message boxes and their buttons
#include <cmath>
#include <cstdio>
#include <cstring>

#include "mwsound/soundmanagerimp.hpp"
#include "platform/gamma.hpp"
#include "platform/linear.hpp"
#include "components/debug/debuglog.hpp"
#include "mwrender/renderingmanager.hpp"
#include "mwgui/windowmanagerimp.hpp"
#include "mwbase/session.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <sys/stat.h>
#include <tex3ds.h>
#include <citro2d.h>
#include "mwrender/objectpaging.hpp"
#include "mwrender/animation.hpp"
#include <cstdlib>
#include "platform/memstat.hpp"
#include <malloc.h>
#include "mwbase/sessionhelpers.hpp"
#include "mwgui/windowhelpers.hpp"

void Session::drawMessage()
{
	MessageState& m = messages.front();
	uiPanel(8, 8, 304, 224);
	const UiInput& in = uiIn();
	int n = m.buttons.size();
	if (m.text != messageShown)
	{
		messageShown = m.text;
		messageScroll = UiScroll();
	}
	// The text above a row of buttons when it fits there; else the buttons go to a column on the right and the
	// text gets the whole height on the left, scrolling a line at a time (drag it, or the circle pad) with a bar
	// that shows how much more there is
	float lh = uiLineHeight(0.5f);
	std::vector<std::string> lines = uiWrap(m.text, 284, 0.5f);
	float bh = 26.0f;
	messageSidebar = lines.size() * lh > 224 - n * (bh + 4) - 4 - 12;
	float tx = 14, tw = 292, ty = 12, th = 224 - n * (bh + 4) - 4 - 12;
	// (a column button's label wraps onto a second line rather than being cut short)
	const float bx = 188, bw = 118, bs = 0.42f;
	std::vector<std::vector<std::string>> labels(n);
	std::vector<float> by(n), bhs(n);
	if (messageSidebar)
	{
		tw = 166;
		th = 216;
		lines = uiWrap(m.text, tw - 8, 0.5f);
		float y = 16, blh = uiLineHeight(bs);
		for (int i = 0; i < n; i++)
		{
			labels[i] = uiWrap(m.buttons[i], bw - 10, bs);
			if (labels[i].size() > 2)
				labels[i].resize(2);
			by[i] = y;
			bhs[i] = fmaxf(24.0f, labels[i].size() * blh + 8);
			y += bhs[i] + 4;
		}
	}
	int rows = std::max(1, (int)(th / lh)), total = (int)lines.size();
	int maxTop = std::max(0, total - rows);
	if (in.touching && in.touchX >= tx && in.touchX < tx + tw && in.touchY >= ty && in.touchY < ty + th)
		messageScroll.scroll -= in.dragDY;
	messageScroll.scroll += uiStickScroll();
	messageScroll.scroll = fmaxf(0.0f, fminf(messageScroll.scroll, maxTop * lh));
	int first = std::min(maxTop, (int)(messageScroll.scroll / lh + 0.5f));
	for (int i = 0; i < rows && first + i < total; i++)
		uiText(tx + 4, ty + i * lh, 0.5f, col::text, lines[first + i]);
	if (maxTop > 0)
	{
		float bx = tx + tw + 2, trackH = rows * lh;
		uiRect(bx, ty, 3, trackH, col::panelLight);
		uiRect(bx, ty + trackH * first / total, 3, fmaxf(6.0f, trackH * rows / total), col::header);
	}
	messageTop = first;
	messageLeft = std::max(0, total - first - rows);
	if (in.down & (KEY_DOWN | KEY_RIGHT)) focus = (focus + 1) % n;
	if (in.down & (KEY_UP | KEY_LEFT)) focus = (focus + n - 1) % n;
	if (focus >= n) focus = 0;
	int hit = -1;
	for (int i = 0; i < n; i++)
	{
		bool pick;
		if (messageSidebar)
		{
			pick = uiButton(bx, by[i], bw, bhs[i], "", i == focus);
			float blh = uiLineHeight(bs), top = by[i] + (bhs[i] - labels[i].size() * blh) / 2;
			for (size_t k = 0; k < labels[i].size(); k++)
				uiTextCentered(bx + bw / 2, top + k * blh, bs, i == focus ? col::textOver : col::text, labels[i][k]);
		}
		else
			pick = uiButton(20, 224 - n * (bh + 4) + i * (bh + 4), 280, bh, m.buttons[i], i == focus);
		if (pick)
			hit = i;
	}
	if (hit >= 0)
	{
		playSound(-1, "Menu Click");
		if (m.fromScript)
			buttonPressed = hit;
		messages.erase(messages.begin());
		focus = 0;
	}
}
