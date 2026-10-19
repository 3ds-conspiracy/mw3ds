// The console build's entry: the game is engine.cpp, shared with the PC test build (platform/platform.hpp)
#include "platform/platform.hpp"

// A scripted run's end changes nothing on the console: the game goes on (into the run's QUIT, when it has one)
void platformRunOver()
{
}

bool platformFailFast()
{
	return false;
}

int main()
{
	return appMain();
}
