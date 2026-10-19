// The PC test build's entry: the game is source/app.cpp, the same code as on the console (platform.h)
#include <cstdlib>

#include "log.h"
#include "platform.h"

void nativeVideoClose();

// The run is over: the log closed, the video (NATIVE_VIDEO) written out, the process ended
void platformRunOver()
{
	logExit();
	nativeVideoClose();
	exit(0);
}

bool platformFailFast()
{
	return getenv("MW3DS_FAILFAST") != nullptr;
}

int main()
{
	return appMain();
}
