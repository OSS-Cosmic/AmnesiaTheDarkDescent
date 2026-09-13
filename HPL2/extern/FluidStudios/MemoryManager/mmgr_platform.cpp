#include "mmgr_platform.h"

#include <stdarg.h>

#if defined(_WIN32)
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>
static CRITICAL_SECTION gMmgrLocks[2];
static INIT_ONCE gMmgrOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK mmgrInitLocks(PINIT_ONCE, PVOID, PVOID*)
{
    InitializeCriticalSection(&gMmgrLocks[0]);
    InitializeCriticalSection(&gMmgrLocks[1]);
    return TRUE;
}
static void mmgrEnsureLocks(void) { InitOnceExecuteOnce(&gMmgrOnce, mmgrInitLocks, 0, 0); }
#else
#include <pthread.h>
static pthread_mutex_t gMmgrLocks[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };
static void mmgrEnsureLocks(void) {}
#endif

extern "C" void mmgrPlatformLock(int lockId)
{
    mmgrEnsureLocks();
#if defined(_WIN32)
    EnterCriticalSection(&gMmgrLocks[lockId & 1]);
#else
    (void)pthread_mutex_lock(&gMmgrLocks[lockId & 1]);
#endif
}

extern "C" void mmgrPlatformUnlock(int lockId)
{
#if defined(_WIN32)
    LeaveCriticalSection(&gMmgrLocks[lockId & 1]);
#else
    (void)pthread_mutex_unlock(&gMmgrLocks[lockId & 1]);
#endif
}

extern "C" void mmgrPlatformDiagnostic(const char* text)
{
    if (text != 0)
        fputs(text, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

extern "C" FILE* mmgrPlatformOpenReport(const char* path, int append)
{
#if defined(_WIN32)
	if (path == 0)
		return 0;
	const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, 0, 0);
	if (required <= 0)
		return 0;
	wchar_t *widePath = static_cast<wchar_t *>(malloc(static_cast<size_t>(required) * sizeof(wchar_t)));
	if (widePath == 0)
		return 0;
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, widePath, required) <= 0)
	{
		free(widePath);
		return 0;
	}
	FILE *file = _wfopen(widePath, append ? L"ab" : L"wb");
	free(widePath);
	return file;
#else
	return fopen(path, append ? "ab" : "wb");
#endif
}

extern "C" int mmgrPlatformCloseReport(FILE* file)
{
    if (file == 0)
        return 0;

    const int result = fclose(file);
    if (result != 0)
        mmgrPlatformDiagnostic("FluidStudios: unable to close report file");
    return result;
}
