#pragma once

// Skills (OpenMW's ESM::Skill)
#include <string>

                                                        // 3 illusion, 4 mysticism, 5 restoration
struct SkillDef { int attribute, specialization; std::string desc; float use[4]; };   // use: progress per kind of use
