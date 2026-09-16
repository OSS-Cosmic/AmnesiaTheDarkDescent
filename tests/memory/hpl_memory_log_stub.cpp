#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
bool gCallbackAllocated = false;
char gLastCallback[4096] = {};
}

// The standalone memory runner does not link LowLevelSystemSDL.cpp.  Keep the
// production MemoryManager->HPL Log dependency intact while providing the
// smallest equivalent sink for those headless tests.
namespace hpl {
void Log(const char *format, ...)
{
	if (!format) return;
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	// This models a normal allocating log callback.  LogResults must invoke it
	// only after the backend report lock has been released.
	char captured[4096];
	va_start(args, format);
	vsnprintf(captured, sizeof(captured), format, args);
	va_end(args);
	{ std::string callbackText(captured); gCallbackAllocated = !callbackText.empty(); }
	std::snprintf(gLastCallback, sizeof(gLastCallback), "%s", captured);
}
}

bool hplTestLogCallbackAllocated() { return gCallbackAllocated; }
bool hplTestLogContains(const char *needle) { return needle && std::strstr(gLastCallback, needle) != nullptr; }
