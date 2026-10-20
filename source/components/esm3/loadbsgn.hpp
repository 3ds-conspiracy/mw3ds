#pragma once

// Birthsigns (OpenMW's ESM::BirthSign)
#include <string>
#include <vector>

struct BirthDef { std::string id, name, desc, texture; std::vector<std::string> spells; };
