// The console build's entry: the game is app.cpp, shared with the PC test build (platform.h)
#include "platform.h"

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
