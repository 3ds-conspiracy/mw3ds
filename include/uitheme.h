#pragma once

#include <3ds/types.h>
#include <string>
#include <unordered_map>
#include <vector>

// Morrowind's look for the menus (game.json "ui", tools/uiassets.py). Everything is optional: what
// is missing is drawn plainly.
struct UiGlyph { u16 x, y, w, h; float advance, bearX, bearY; };
struct UiPiece { u16 x, y, w, h; };
struct UiThemeDef
{
	// Item icons: iconSize squares, iconPage wide pages (files and their heights)
	int iconSize = 32, iconPage = 256;
	std::vector<std::pair<std::string, int>> iconPages;
	// Window frame pieces (menu_thin_border_top, ...) in one atlas
	std::string atlasFile;
	int atlasW = 0, atlasH = 0;
	std::unordered_map<std::string, UiPiece> pieces;
	// Magic Cards font
	std::string fontFile;
	int fontW = 0, fontH = 0;
	float fontLine = 0.0f;
	std::unordered_map<u32, UiGlyph> glyphs;
	// Morrowind.ini [Font Color], "normal" -> 0xAABBGGRR
	std::unordered_map<std::string, u32> colors;
};
