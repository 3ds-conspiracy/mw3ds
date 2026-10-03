#pragma once

#include <string>
#include <vector>

#include "world.h"

// Helpers the menu screens share (screens.cpp)
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
bool inTab(const Object* o, int tab);
void itemInfo(const Object* o, float y, const std::string& extra = "");   // name, weight and value
