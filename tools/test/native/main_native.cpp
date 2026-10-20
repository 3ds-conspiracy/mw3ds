// The PC test build's entry: the game is source/engine.cpp, the same code as on the console (platform/platform.hpp)
#include <cstdlib>

#include "components/debug/debuglog.hpp"
#include "platform/platform.hpp"

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

void platformClearVram()
{
}

int main()
{
	return appMain();
}
