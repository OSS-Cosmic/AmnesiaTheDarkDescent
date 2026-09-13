#ifndef HPL_MMGR_PLATFORM_H
#define HPL_MMGR_PLATFORM_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void mmgrPlatformLock(int lockId);
void mmgrPlatformUnlock(int lockId);
void mmgrPlatformDiagnostic(const char* text);
FILE* mmgrPlatformOpenReport(const char* path, int append);
int mmgrPlatformCloseReport(FILE* file);

#ifdef __cplusplus
}
#endif

#endif
