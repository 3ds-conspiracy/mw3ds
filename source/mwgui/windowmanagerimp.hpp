#pragma once

#include <string>
#include <functional>
#include <vector>

#include "mwworld/worldimp.hpp"

// Helpers the menu screens share (mwgui/windowmanagerimp.cpp)
std::string attrName(const World& w, int i);
std::string skillName(const World& w, int i);
// A row of buttons along the bottom; D-pad left/right moves focus, A presses. Returns the one pressed.
int buttonRow(const std::vector<std::string>& labels, int& focus, float y = 208.0f, float h = 28.0f);
std::string specName(const World& w, int i);
void header(const std::string& title, const std::string& note = "");
struct Object;
struct UiGridItem;
UiGridItem gridItem(const Object* o, const std::string& id, int count, bool equipped);
extern const char* kTabs[5];                 // inventory filters: All, Weapon, Apparel, Magic, Misc
extern const char* kTabKeys[5];              // their GMSTs (sAllTab ...)
bool inTab(const Object* o, int tab);
// What decides where an item is listed (mwgui/sortfilteritemmodel.cpp): equipped (the player's own), its record, its
// condition (-1 as new) and charge (-1 full)
struct ItemListed { const Object* o; bool equipped; int condition; float charge; };
bool listedBefore(const GameData& g, const ItemListed& a, const ItemListed& b);
// A list's indices put in that order (stable: one same in every way keeps its place)
void sortListed(const GameData& g, std::vector<int>& order, const std::function<ItemListed(int)>& item);
// A magic school filter button: tab 0 "All", 1..6 the schools by their skill icon (alteration .. restoration)
bool schoolButton(const GameData& g, float x, float y, float w, float h, int tab, bool lit);
void itemInfo(const Object* o, float y, const std::string& extra = "");   // name, weight and value
