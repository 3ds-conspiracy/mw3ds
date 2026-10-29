#pragma once

// The scripts' trees kept on the card between runs (scriptstore.cpp)
struct GameData;
struct Script;
struct Node;

// As the load reads the scripts: each tree to the card and out of the heap as soon as it is built (they stay when the
// card can't be written)
void scriptsSpillBegin();
void scriptSpillOne(Script& s);
void scriptsSpillEnd();
// The script's tree, read back from the card if it isn't in (and marked as run now)
const Node& scriptBody(const Script& s);
// Once a frame, never while a script runs: trees whose script hasn't run for ten seconds go again
void scriptsTick();
int scriptsLoaded();                 // trees read back and in the heap now (for the tests)
