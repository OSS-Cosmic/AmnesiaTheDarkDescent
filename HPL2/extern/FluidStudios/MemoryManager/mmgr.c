// ---------------------------------------------------------------------------------------------------------------------------------
//
//
//  _ __ ___  _ __ ___   __ _ _ __      ___ _ __  _ ___
// | '_ ` _ \| '_ ` _ \ / _` | '__|    / __| '_ \| '_  |
// | | | | | | | | | | | (_| | |    _ | (__| |_) | |_) |
// |_| |_| |_|_| |_| |_|\__, |_|   (_) \___| .__/| .__/
//                       __/ |             | |   | |
//                      |___/              |_|   |_|
//
// Memory manager & tracking software
//
// Best viewed with 8-character tabs and (at least) 132 columns
//
// ---------------------------------------------------------------------------------------------------------------------------------
//
// Restrictions & freedoms pertaining to usage and redistribution of this software:
//
//  * This software is 100% free
//  * If you use this software (in part or in whole) you must credit the author.
//  * This software may not be re-distributed (in part or in whole) in a modified
//    form without clear documentation on how to obtain a copy of the original work.
//  * You may not use this software to directly or indirectly cause harm to others.
//  * This software is provided as-is and without warrantee. Use at your own risk.
//
// For more information, visit HTTP://www.FluidStudios.com
//
// ---------------------------------------------------------------------------------------------------------------------------------
// Originally created on 12/22/2000 by Paul Nettle
//
// Copyright 2000, Fluid Studios, Inc., all rights reserved.
// ---------------------------------------------------------------------------------------------------------------------------------
//
// !!IMPORTANT!!
//
// This software is self-documented with periodic comments. Before you start using this software, perform a search for the string
// "-DOC-" to locate pertinent information about how to use this software.
//
// You are also encouraged to read the comment blocks throughout this source file. They will help you understand how this memory
// tracking software works, so you can better utilize it within your applications.
//
// NOTES:
//
// 1. If you get compiler errors having to do with set_new_handler, then go through this source and search/replace
//    "std::set_new_handler" with "set_new_handler".
//
// 2. This code purposely uses no external routines that allocate RAM (other than the raw allocation routines, such as malloc). We
//    do this because we want this to be as self-contained as possible. As an example, we don't use assert, because when running
//    under WIN32, the assert brings up a dialog box, which allocates RAM. Doing this in the middle of an allocation would be bad.
//
// 3. When trying to override new/delete under MFC (which has its own version of global new/delete) the linker will complain. In
//    order to fix this error, use the compiler option: /FORCE, which will force it to build an executable even with linker errors.
//    Be sure to check those errors each time you compile, otherwise, you may miss a valid linker error.
//
// 4. If you see something that looks odd to you or seems like a strange way of going about doing something, then consider that this
//    code was carefully thought out. If something looks odd, then just assume I've got a good reason for doing it that way (an
//    example is the use of the class MemStaticTimeTracker.)
//
// 5. With MFC applications, you will need to comment out any occurance of "#define new DEBUG_NEW" from all source files.
//
// 6. Include file dependencies are _very_important_ for getting the MMGR to integrate nicely into your application. Be careful if
//    you're including standard includes from within your own project includes; that will break this very specific dependency order.
//    It should look like this:
//
//		#include <stdio.h>   // Standard includes MUST come first
//		#include <stdlib.h>  //
//		#include <streamio>  //
//
//		#include "mmgr.h"    // mmgr.h MUST come next
//
//		#include "myfile1.h" // Project includes MUST come last
//		#include "myfile2.h" //
//		#include "myfile3.h" //
//
// ---------------------------------------------------------------------------------------------------------------------------------

// #include "stdafx.h"
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mmgr.h"
#include "mmgr_platform.h"
#ifdef MMGR_TESTING
#include "mmgr_test.h"
#endif

#define MMGR_ASSERT(expression) ((void)((expression) ? 0 : (mmgrPlatformDiagnostic("FluidStudios: allocator invariant failed"), 0)))

#if MMGR_BACKTRACE
#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <execinfo.h>
#endif
#endif

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- If you're like me, it's hard to gain trust in foreign code. This memory manager will try to INDUCE your code to crash (for
// very good reasons... like making bugs obvious as early as possible.) Some people may be inclined to remove this memory tracking
// software if it causes crashes that didn't exist previously. In reality, these new crashes are the BEST reason for using this
// software!
//
// Whether this software causes your application to crash, or if it reports errors, you need to be able to TRUST this software. To
// this end, you are given some very simple debugging tools.
//
// The quickest way to locate problems is to enable the STRESS_TEST macro (below.) This should catch 95% of the crashes before they
// occur by validating every allocation each time this memory manager performs an allocation function. If that doesn't work, keep
// reading...
//
// If you enable the TEST_MEMORY_MANAGER #define (below), this memory manager will log an entry in the memory.log file each time it
// enters and exits one of its primary allocation handling routines. Each call that succeeds should place an "ENTER" and an "EXIT"
// into the log. If the program crashes within the memory manager, it will log an "ENTER", but not an "EXIT". The log will also
// report the name of the routine.
//
// Just because this memory manager crashes does not mean that there is a bug here! First, an application could inadvertantly damage
// the heap, causing malloc(), realloc() or free() to crash. Also, an application could inadvertantly damage some of the memory used
// by this memory tracking software, causing it to crash in much the same way that a damaged heap would affect the standard
// allocation routines.
//
// In the event of a crash within this code, the first thing you'll want to do is to locate the actual line of code that is
// crashing. You can do this by adding log() entries throughout the routine that crashes, repeating this process until you narrow
// in on the offending line of code. If the crash happens in a standard C allocation routine (i.e. malloc, realloc or free) don't
// bother contacting me, your application has damaged the heap. You can help find the culprit in your code by enabling the
// STRESS_TEST macro (below.)
//
// If you truely suspect a bug in this memory manager (and you had better be sure about it! :) you can contact me at
// midnight@FluidStudios.com. Before you do, however, check for a newer version at:
//
//	http://www.FluidStudios.com/publications.html
//
// When using this debugging aid, make sure that you are NOT setting the alwaysLogAll variable on, otherwise the log could be
// cluttered and hard to read.
// ---------------------------------------------------------------------------------------------------------------------------------

// #define	TEST_MEMORY_MANAGER

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- Enable this sucker if you really want to stress-test your app's memory usage, or to help find hard-to-find bugs
// ---------------------------------------------------------------------------------------------------------------------------------

// #define	STRESS_TEST

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- Enable this sucker if you want to stress-test your app's error-handling. Set RANDOM_FAIL to the percentage of failures you
//       want to test with (0 = none, >100 = all failures).
// ---------------------------------------------------------------------------------------------------------------------------------

// #define	RANDOM_FAILURE 10.0

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- Enable this sucker if you want to make sure you aren't getting lucky with memory alignment. If this is enabled and you ask
//       for memory aligned to 16 bytes (for example), you will only get memory aligned to 16 bytes, NOT 32+ bytes.
// ---------------------------------------------------------------------------------------------------------------------------------

// #define	FORCE_EXACT_ALIGNMENT

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- Locals -- modify these flags to suit your needs
// ---------------------------------------------------------------------------------------------------------------------------------

#ifdef STRESS_TEST
#define hashBits 12u
static bool               randomWipe = true;
static bool               alwaysValidateAll = true;
static bool               alwaysLogAll = true;
static bool               alwaysWipeAll = true;
static bool               cleanupLogOnFirstRun = true;
static const unsigned int paddingSize = 1024; // An extra 8K per allocation!
#else
#define hashBits 12u
static bool               randomWipe = false;
static bool               alwaysValidateAll = false;
static bool               alwaysLogAll = false;
static bool               alwaysWipeAll = true;
static bool               cleanupLogOnFirstRun = true;
static const unsigned int paddingSize = 4;
#endif

#define UNREF_PARAM(value) ((void)(value))

#ifndef __has_feature
#define __has_feature(...) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define ASAN_POISON(addr, size)   __asan_poison_memory_region(addr, size)
#define ASAN_UNPOISON(addr, size) __asan_unpoison_memory_region(addr, size)
#else
#define ASAN_POISON(...)
#define ASAN_UNPOISON(...)
#endif

// ---------------------------------------------------------------------------------------------------------------------------------
// Here, we turn off our macros because any place in this source file where the word 'new' or the word 'delete' (etc.)
// appear will be expanded by the macro. So to avoid problems using them within this source file, we'll just #undef them.
// ---------------------------------------------------------------------------------------------------------------------------------

#undef new
#undef delete
#undef malloc
#undef calloc
#undef realloc
#undef free

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- Get to know these values. They represent the values that will be used to fill unused and deallocated RAM.
// ---------------------------------------------------------------------------------------------------------------------------------

static unsigned int prefixPattern = 0xbaadf00d;   // Fill pattern for bytes preceeding allocated blocks
static unsigned int postfixPattern = 0xdeadc0de;  // Fill pattern for bytes following allocated blocks
static unsigned int unusedPattern = 0xfeedface;   // Fill pattern for freshly allocated blocks
static unsigned int releasedPattern = 0xdeadbeef; // Fill pattern for deallocated blocks

// ---------------------------------------------------------------------------------------------------------------------------------
// Other locals
// ---------------------------------------------------------------------------------------------------------------------------------

#define hashSize (1u << hashBits)
static const char*  allocationTypes[] = { "Unknown", "new", "new[]", "malloc", "calloc", "realloc", "delete", "delete[]", "free" };
static sAllocUnit*  hashTable[hashSize];
static sAllocUnit*  reservoir;
static size_t currentAllocationCount = 0;
static size_t breakOnAllocationCount = 0;
static sMStats      stats;
static sAllocUnit** reservoirBuffer = NULL;
static size_t reservoirBufferSize = 0;
// static const char*  memoryLogFile = "memory.log";
static void         doCleanupLogOnFirstRun(void);
static char*        LogToMemory(char* log);
static char         mAppNameStorage[4096];
static char         mLogDirectoryStorage[4096];
static const char*   mAppName = NULL;

#if defined(__cplusplus)
#define MMGR_THREAD_LOCAL thread_local
#else
#define MMGR_THREAD_LOCAL _Thread_local
#endif
static MMGR_THREAD_LOCAL const char* tlsSourceFile = "??";
static MMGR_THREAD_LOCAL const char* tlsSourceFunc = "??";
static MMGR_THREAD_LOCAL unsigned int tlsSourceLine = 0;
#ifdef MMGR_TESTING
static int mmgrTestFailureStage = 0;
#endif

#if MMGR_BACKTRACE
static MMGR_THREAD_LOCAL int stackSkipCount = 0;
static int stackCaptureSkip(void)
{
    return stackSkipCount < INT_MAX ? stackSkipCount + 1 : INT_MAX;
}
#ifdef _WIN32
// Process-lifetime unique handle for symbols; reporting does not shut it down.
static HANDLE gProcessHandle;
#endif
#endif
//

// The allocation lock protects the hash table, allocation units, and statistics.
// The logging lock protects the in-memory event log.  Lock ordering is always
// allocation -> logging; code holding the logging lock must never acquire the
// allocation lock.  Reports take the allocation lock for one consistent snapshot.

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*x))
#if defined(__GNUC__) || defined(__clang__)
#define MMGR_UNUSED __attribute__((unused))
#else
#define MMGR_UNUSED
#endif

#define MUTEX_LOCK(mutex) mmgrPlatformLock(mutex)
#define MUTEX_UNLOCK(mutex) mmgrPlatformUnlock(mutex)
#define allocMutex 0
#define logMutex 1

static bool validateAllocUnitUnlocked(const sAllocUnit* allocUnit);
static bool validateAllAllocUnitsUnlocked(void);
static size_t calcUnusedUnlocked(const sAllocUnit* allocUnit);
static size_t calcAllUnusedUnlocked(void);
static void dumpAllocUnitUnlocked(const sAllocUnit* allocUnit, const char* prefix);
static bool isValidAllocationType(unsigned int type);
static bool isValidReallocationType(unsigned int type);
static bool isValidDeallocationType(unsigned int type);
static bool findAllocUnitRecordUnlocked(const sAllocUnit* record);
static bool checkedStatsAdd(const sMStats* before, size_t reported, size_t actual, sMStats* after);
static bool checkedStatsReallocate(const sMStats* before, size_t oldReported, size_t oldActual,
                                   size_t newReported, size_t newActual, sMStats* after);
static void invalidOperationDiagnostic(const char* operation, const void* address)
{
    char message[160];
    snprintf(message, sizeof(message), "FluidStudios: invalid %s address 0x%zx", operation, (size_t)address);
    mmgrPlatformDiagnostic(message);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Local functions only
// ---------------------------------------------------------------------------------------------------------------------------------

#define BUFFER_SIZE 2048

typedef struct tm tm;

inline static char* tf_strncpy(char* dst, size_t dstSize, const char* src, size_t count)
{
    size_t size = dstSize < count ? dstSize - 1 : count;
    char*  ret = strncpy(dst, src, size);
    ret[size] = '\0';
    return ret;
}
#define tf_strncpyarr(dst, src, count) tf_strncpy(dst, ARRAY_SIZE(dst), src, count)

inline static char* tf_strcpy(char* dst, size_t dstSize, const char* src)
{
    size_t srcSize = strlen(src);
    return tf_strncpy(dst, dstSize, src, srcSize);
}

#define tf_strcpyarr(dst, src)                tf_strcpy(dst, ARRAY_SIZE(dst), src)

#define tf_sprintf(dst, dstSize, format, ...) snprintf(dst, dstSize, format, __VA_ARGS__)

#define tf_sprintfarr(dst, format, ...)       tf_sprintf(dst, ARRAY_SIZE(dst), format, __VA_ARGS__)

static char* Log(const char* format, ...)
{
    // Build the buffer

    /*logMutex->lock();*/
    MUTEX_LOCK(logMutex);

    static char buffer[BUFFER_SIZE];
    va_list     ap;
    va_start(ap, format);
    const int charsWritten = vsnprintf(buffer, BUFFER_SIZE, format, ap);
    va_end(ap);

    // Open the log file

    // Too slow for writing to disk every time

    // FILE*fp = NULL;
    // fopen_s(&fp, memoryLogFile, "ab");

    //// If you hit this assert, then the memory logger is unable to log information to a file (can't open the file for some
    //// reason.) You can interrogate the variable 'buffer' to see what was supposed to be logged (but won't be.)
    // m_assert(fp);

    // if (!fp) return;

    //// Spit out the data to the log

    // fprintf(fp, "%s\r\n", buffer);
    // fclose(fp);

    // Add a newline to the end
    int       newlinePos = charsWritten < 0 ? 0 : charsWritten;
    const int lastNewlinePos = BUFFER_SIZE - 2;
    newlinePos = newlinePos > lastNewlinePos ? lastNewlinePos : newlinePos;
    buffer[newlinePos] = '\n';
    buffer[newlinePos + 1] = '\0';

    // Quicker

    char* logAddress = LogToMemory(buffer);

    // logMutex->unlock();
    MUTEX_UNLOCK(logMutex);
    return logAddress;
}

// ---------------------------------------------------------------------------------------------------------------------------------

static void doCleanupLogOnFirstRun(void)
{
    if (cleanupLogOnFirstRun)
    {
        // #ifndef NX64
        //		_unlink(memoryLogFile);
        // #endif
        cleanupLogOnFirstRun = false;

        // Print a header for the log

        //		time_t t = time(NULL);
        //		tm     localt;
        // #ifdef _WIN32
        //		localtime_s(&localt, &t);
        // #else
        //        localtime_s(&t, &localt);
        // #endif
        // char asciiTime[64];
        // use strftime instead of asctime so we don't get the trailing newline. (We're writing the
        // strftime(asciiTime, 64, "%c", &localt);
        // Log("--------------------------------------------------------------------------------");
        // Log("");
        // Log("      %s - Memory logging file created on %s", memoryLogFile, asciiTime);
        // Log("");
        Log("--------------------------------------------------------------------------------");
        Log("");
        Log("This file contains a log of all memory operations performed during the last run.");
        Log("");
        Log("Interrogate this file to track errors or to help track down memory-related");
        Log("issues. You can do this by tracing the allocations performed by a specific owner");
        Log("or by tracking a specific address through a series of allocations and");
        Log("reallocations.");
        Log("");
        Log("There is a lot of useful information here which, when used creatively, can be");
        Log("extremely helpful.");
        Log("");
        Log("Note that the following guides are used throughout this file:");
        Log("");
        Log("   [!] - Error");
        Log("   [+] - Allocation");
        Log("   [~] - Reallocation");
        Log("   [-] - Deallocation");
        Log("   [I] - Generic information");
        Log("   [F] - Failure induced for the purpose of stress-testing your application");
        Log("   [D] - Information used for debugging this memory manager");
        Log("");
        Log("...so, to find all errors in the file, search for \"[!]\"");
        Log("");
        Log("--------------------------------------------------------------------------------");
    }
}

/* Called with allocMutex held.  It is intentionally non-locking so the first
   allocation can initialize all process state without recursive locking. */
static void initializeMmgrUnlocked(const char* appName)
{
    if (appName != NULL || mAppName == NULL)
    {
        const char* initialName = appName ? appName : "memory";
        const size_t initialNameLength = strlen(initialName);
        if (initialNameLength >= sizeof(mAppNameStorage))
        {
            mmgrPlatformDiagnostic("FluidStudios: application name is too long");
            tf_strcpy(mAppNameStorage, sizeof(mAppNameStorage), "memory");
        }
        else
            memcpy(mAppNameStorage, initialName, initialNameLength + 1);
        mAppName = mAppNameStorage;
    }
#if MMGR_BACKTRACE && defined(_WIN32)
    if (gProcessHandle == NULL)
    {
        HANDLE currentProcess = GetCurrentProcess();
        DuplicateHandle(currentProcess, currentProcess, currentProcess, &gProcessHandle, 0, true, DUPLICATE_SAME_ACCESS);
    }
#endif
    if (cleanupLogOnFirstRun)
    {
        doCleanupLogOnFirstRun();
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------

static const char* sourceFileStripper(const char* sourceFile)
{
    if (sourceFile == NULL)
        return "??";
    const char* ptr = strrchr(sourceFile, '\\');
    if (ptr)
        return ptr + 1;
    ptr = strrchr(sourceFile, '/');
    if (ptr)
        return ptr + 1;
    return sourceFile;
}

// ---------------------------------------------------------------------------------------------------------------------------------

static const char* ownerString(const char* sourceFile, const unsigned int sourceLine, const char* sourceFunc)
{
    static MMGR_THREAD_LOCAL char str[320];
    memset(str, 0, sizeof(str));
    tf_sprintfarr(str, "%s(%05u)::%s", sourceFileStripper(sourceFile), sourceLine, sourceFunc ? sourceFunc : "??");
    return str;
}

// ---------------------------------------------------------------------------------------------------------------------------------

static const char* insertCommas(size_t value)
{
    static MMGR_THREAD_LOCAL char str[3 * sizeof(size_t) + 1 + sizeof(size_t) / 3 + 1];
    char digits[sizeof(size_t) * 2 + 1];
    tf_sprintfarr(digits, "%zu", value);
    const size_t digitCount = strlen(digits);
    size_t out = 0;
    for (size_t i = 0; i < digitCount; ++i)
    {
        if (i != 0 && (digitCount - i) % 3 == 0)
            str[out++] = ',';
        str[out++] = digits[i];
    }
    str[out] = '\0';
    return str;
}

// ---------------------------------------------------------------------------------------------------------------------------------

static const char* memorySizeString(size_t size)
{
    static MMGR_THREAD_LOCAL char str[90];
    if (size > (1024 * 1024))
        tf_sprintfarr(str, "%10s (%7.2fM)", insertCommas(size), ((double)size) / (1024.0 * 1024.0));
    else if (size > 1024)
        tf_sprintfarr(str, "%10s (%7.2fK)", insertCommas(size), ((double)size) / 1024.0);
    else
        tf_sprintfarr(str, "%10s bytes     ", insertCommas(size));
    return str;
}

// ---------------------------------------------------------------------------------------------------------------------------------

static sAllocUnit* findAllocUnit(const void* reportedAddress)
{
    if (reportedAddress == NULL)
        return NULL;

    // Use the address to locate the hash index. Note that we shift off the lower four bits. This is because most allocated
    // addresses will be on four-, eight- or even sixteen-byte boundaries. If we didn't do this, the hash index would not have
    // very good coverage.

    size_t      hashIndex = (((size_t)reportedAddress) >> 4) & (hashSize - 1);
    sAllocUnit* ptr = hashTable[hashIndex];
    while (ptr)
    {
        if (ptr->reportedAddress == reportedAddress)
            return ptr;
        ptr = ptr->next;
    }

    return NULL;
}

static bool containsAddressUnlocked(const void* address)
{
    if (address == NULL)
        return false;

    const uintptr_t candidate = (uintptr_t)address;
    for (size_t i = 0; i < hashSize; ++i)
    {
        for (sAllocUnit* ptr = hashTable[i]; ptr != NULL; ptr = ptr->next)
        {
            /* Use subtraction after the lower-bound check.  This avoids
               wrapping when a tracked range ends at the top of the address
               space, and deliberately excludes both zero-sized ranges and
               the one-past-the-end address. */
            const uintptr_t begin = (uintptr_t)ptr->reportedAddress;
            if (candidate >= begin && candidate - begin < ptr->reportedSize)
                return true;
        }
    }
    return false;
}

static bool findAllocUnitRecordUnlocked(const sAllocUnit* record)
{
    if (record == NULL) return false;
    for (size_t i = 0; i < hashSize; ++i)
        for (sAllocUnit* ptr = hashTable[i]; ptr != NULL; ptr = ptr->next)
            if (ptr == record) return true;
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------------------

static size_t calculateActualSize(const size_t reportedSize)
{
    // We use DWORDS as our padding, and a uint32_t is guaranteed to be 4 bytes, but an int is not (ANSI defines an int as
    // being the standard word size for a processor; on a 32-bit machine, that's 4 bytes, but on a 64-bit machine, it's
    // 8 bytes, which means an int can actually be larger than a uint32_t.)

    const size_t padding = paddingSize * sizeof(uint32_t) * 2;
    if (reportedSize > SIZE_MAX - padding)
        return 0;
    return reportedSize + padding;
}

static bool checkedAllocationSize(size_t reportedSize, size_t alignment, size_t* result)
{
    const size_t base = calculateActualSize(reportedSize);
    if (base == 0 || alignment == 0 || alignment > SIZE_MAX - base)
        return false;
    size_t size = base + alignment;
#ifdef FORCE_EXACT_ALIGNMENT
    if (alignment > SIZE_MAX - size)
        return false;
    size += alignment;
#endif
    *result = size;
    return true;
}

static size_t mmgrMaxAlignment(void)
{
#if defined(__cplusplus)
    return alignof(max_align_t);
#elif defined(_MSC_VER)
    // MSVC supports C11 _Alignof but its C headers do not define max_align_t.
    // A union of the widest fundamental scalar types provides the equivalent
    // alignment floor required for ordinary malloc-compatible allocations.
    typedef union mmgr_max_align_t
    {
        long double longDoubleValue;
        long long longLongValue;
        void* pointerValue;
    } mmgr_max_align_t;
    return _Alignof(mmgr_max_align_t);
#else
    return _Alignof(max_align_t);
#endif
}

static bool validAlignment(size_t alignment)
{
    return alignment >= sizeof(void*) && (alignment & (alignment - 1)) == 0;
}

static bool isValidAllocationType(unsigned int type)
{
    return type == m_alloc_new || type == m_alloc_new_array || type == m_alloc_malloc || type == m_alloc_calloc;
}

static bool isValidReallocationType(unsigned int type)
{
    return type == m_alloc_realloc;
}

static bool isValidDeallocationType(unsigned int type)
{
    return type == m_alloc_delete || type == m_alloc_delete_array || type == m_alloc_free;
}

static bool checkedStatsAdd(const sMStats* before, size_t reported, size_t actual, sMStats* after)
{
    *after = *before;
    if (before->totalReportedMemory > SIZE_MAX - reported ||
        before->totalActualMemory > SIZE_MAX - actual ||
        before->totalAllocUnitCount == SIZE_MAX ||
        before->accumulatedReportedMemory > SIZE_MAX - reported ||
        before->accumulatedActualMemory > SIZE_MAX - actual ||
        before->accumulatedAllocUnitCount == SIZE_MAX)
        return false;
    after->totalReportedMemory += reported;
    after->totalActualMemory += actual;
    after->totalAllocUnitCount++;
    after->accumulatedReportedMemory += reported;
    after->accumulatedActualMemory += actual;
    after->accumulatedAllocUnitCount++;
    if (after->totalReportedMemory > after->peakReportedMemory) after->peakReportedMemory = after->totalReportedMemory;
    if (after->totalActualMemory > after->peakActualMemory) after->peakActualMemory = after->totalActualMemory;
    if (after->totalAllocUnitCount > after->peakAllocUnitCount) after->peakAllocUnitCount = after->totalAllocUnitCount;
    return true;
}

static bool checkedStatsReallocate(const sMStats* before, size_t oldReported, size_t oldActual,
                                   size_t newReported, size_t newActual, sMStats* after)
{
    *after = *before;
    if (before->totalAllocUnitCount == 0 ||
        before->totalReportedMemory < oldReported || before->totalActualMemory < oldActual)
        return false;

    after->totalReportedMemory -= oldReported;
    after->totalActualMemory -= oldActual;
    if (after->totalReportedMemory > SIZE_MAX - newReported ||
        after->totalActualMemory > SIZE_MAX - newActual)
        return false;
    after->totalReportedMemory += newReported;
    after->totalActualMemory += newActual;

    if (newReported > oldReported && before->accumulatedReportedMemory > SIZE_MAX - (newReported - oldReported))
        return false;
    if (newActual > oldActual && before->accumulatedActualMemory > SIZE_MAX - (newActual - oldActual))
        return false;
    if (newReported > oldReported)
        after->accumulatedReportedMemory += newReported - oldReported;
    if (newActual > oldActual)
        after->accumulatedActualMemory += newActual - oldActual;

    if (after->totalReportedMemory > after->peakReportedMemory) after->peakReportedMemory = after->totalReportedMemory;
    if (after->totalActualMemory > after->peakActualMemory) after->peakActualMemory = after->totalActualMemory;
    if (after->totalAllocUnitCount > after->peakAllocUnitCount) after->peakAllocUnitCount = after->totalAllocUnitCount;
    return true;
}

#ifdef MMGR_TESTING
static bool mmgrTestShouldFail(int stage)
{
    if (mmgrTestFailureStage == stage) {
        mmgrTestFailureStage = 0;
        return true;
    }
    return false;
}
#else
#define mmgrTestShouldFail(stage) false
#endif

// ---------------------------------------------------------------------------------------------------------------------------------

// static	size_t	calculateReportedSize(const size_t actualSize)
//{
//	// We use DWORDS as our padding, and a uint32_t is guaranteed to be 4 bytes, but an int is not (ANSI defines an int as
//	// being the standard word size for a processor; on a 32-bit machine, that's 4 bytes, but on a 64-bit machine, it's
//	// 8 bytes, which means an int can actually be larger than a uint32_t.)
//
//	return actualSize - paddingSize * sizeof(uint32_t) * 2;
// }

// ---------------------------------------------------------------------------------------------------------------------------------

static void* calculateReportedAddress(const void* actualAddress)
{
    // We allow this...

    if (!actualAddress)
        return NULL;

    // JUst account for the padding

    return (void*)(((const char*)actualAddress) + sizeof(uint32_t) * paddingSize);
}

// ---------------------------------------------------------------------------------------------------------------------------------

static void wipeWithPattern(sAllocUnit* allocUnit, uint32_t pattern, const size_t originalReportedSize)
{
    // For a serious test run, we use wipes of random a random value. However, if this causes a crash, we don't want it to
    // crash in a differnt place each time, so we specifically DO NOT call srand. If, by chance your program calls srand(),
    // you may wish to disable that when running with a random wipe test. This will make any crashes more consistent so they
    // can be tracked down easier.

    if (randomWipe)
    {
        pattern = ((uint32_t)(rand() & 0xff) << 24) | ((uint32_t)(rand() & 0xff) << 16) |
                  ((uint32_t)(rand() & 0xff) << 8) | (uint32_t)(rand() & 0xff);
    }

    // -DOC- We should wipe with 0's if we're not in debug mode, so we can help hide bugs if possible when we release the
    // product. So uncomment the following line for releases.
    //
    // Note that the "alwaysWipeAll" should be turned on for this to have effect, otherwise it won't do much good. But we'll
    // leave it this way (as an option) because this does slow things down.
    //	pattern = 0;

    // This part of the operation is optional

    if (alwaysWipeAll && allocUnit->reportedSize > originalReportedSize)
    {
        // Fill the bulk

        const size_t length = allocUnit->reportedSize - originalReportedSize;
        uint8_t* bytes = (uint8_t*)allocUnit->reportedAddress + originalReportedSize;
        for (size_t i = 0; i < length; ++i)
            bytes[i] = (uint8_t)(pattern >> (((originalReportedSize + i) % sizeof(uint32_t)) * 8));
    }

    // Write in the prefix/postfix bytes

    // Calculate the correct start addresses for pre and post patterns relative to
    // allocUnit->reportedAddress, since it may have been offset due to alignment requirements
    uint8_t* pre = (uint8_t*)allocUnit->reportedAddress - paddingSize * sizeof(uint32_t);
    uint8_t* post = (uint8_t*)allocUnit->reportedAddress + allocUnit->reportedSize;

    const size_t paddingBytes = paddingSize * sizeof(uint32_t);
    for (size_t i = 0; i < paddingBytes; i++, pre++, post++)
    {
        *pre = (prefixPattern >> ((i % sizeof(uint32_t)) * 8)) & 0xFF;
        *post = (postfixPattern >> ((i % sizeof(uint32_t)) * 8)) & 0xFF;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
static void dumpLine(FILE* fileToWrite, const char* format, ...)
{
    va_list args;
    char    buffer[BUFFER_SIZE] = { 0 };
    va_start(args, format);
    vsnprintf(buffer, BUFFER_SIZE, format, args);
    va_end(args);

    mmgrPlatformDiagnostic(buffer);
    if (fileToWrite != NULL)
    {
        if (fputs(buffer, fileToWrite) == EOF || fputc('\n', fileToWrite) == EOF)
            mmgrPlatformDiagnostic("FluidStudios: report write failed");
    }
}

#if MMGR_BACKTRACE
static void dumpBacktrace(FILE* fh, const sAllocUnit* ptr)
{
    if (ptr->backtrace_nptrs)
    {
#ifdef _WIN32
        HANDLE process = gProcessHandle;
        SymInitialize(process, NULL, TRUE);

        char         buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;

        char             buffer_line[sizeof(IMAGEHLP_LINE64)];
        PIMAGEHLP_LINE64 pLine = (PIMAGEHLP_LINE64)buffer_line;

        char module_name[MAX_PATH];

        for (int i = ptr->backtrace_skip; i < ptr->backtrace_nptrs; i++)
        {
            DWORD64 address = (DWORD64)(ptr->backtrace_buffer[i]);

            // Get symbol name for address
            memset(buffer, 0, sizeof(buffer));
            pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            pSymbol->MaxNameLen = MAX_SYM_NAME;
            const BOOL haveSymbol = SymFromAddr(process, address, 0, pSymbol);

            // Try to get line
            memset(buffer_line, 0, sizeof(buffer_line));
            pLine->SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD displacement;
            if (SymGetLineFromAddr64(process, address, &displacement, pLine))
            {
                if (fh)
                {
                    dumpLine(fh, "    at %s in %s: line: %lu: address: 0x%016llX", haveSymbol ? pSymbol->Name : "<unknown>",
                             pLine->FileName, pLine->LineNumber,
                             (unsigned long long)(haveSymbol ? pSymbol->Address : address));
                }
                else
                {
                    Log("    at %s in %s: line: %lu: address: 0x%016llX", haveSymbol ? pSymbol->Name : "<unknown>",
                        pLine->FileName, pLine->LineNumber,
                        (unsigned long long)(haveSymbol ? pSymbol->Address : address));
                }
            }
            else
            {
                // Try get the module name
                HMODULE hModule = NULL;
                module_name[0] = '\0';
                GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCTSTR)(address),
                                  &hModule);
                if (hModule != NULL)
                    GetModuleFileNameA(hModule, module_name, MAX_PATH);

                if (fh)
                {
                    dumpLine(fh, "    at %s, address 0x%016llX in %s", haveSymbol ? pSymbol->Name : "<unknown>",
                             (unsigned long long)(haveSymbol ? pSymbol->Address : address), module_name);
                }
                else
                {
                    Log("    at %s, address 0x%016llX in %s", haveSymbol ? pSymbol->Name : "<unknown>",
                        (unsigned long long)(haveSymbol ? pSymbol->Address : address), module_name);
                }
            }
        }

        SymCleanup(process);
#else
        char** strings = backtrace_symbols(ptr->backtrace_buffer, ptr->backtrace_nptrs);
        if (strings != NULL)
        {
            for (int j = ptr->backtrace_skip; j < ptr->backtrace_nptrs; j++)
            {
                if (fh)
                {
                    dumpLine(fh, "\t%s", strings[j]);
                }
                else
                {
                    Log("\t%s", strings[j]);
                }
            }

            free(strings);
        }
#endif
    }
}
#endif

static void dumpAllocations(FILE* fh)
{
    dumpLine(fh, "Alloc.        Addr           Size           Addr           Size                        BreakOn BreakOn");
    dumpLine(fh, "Number      Reported       Reported        Actual         Actual     Unused    Method  Dealloc Realloc  Allocated by");
    dumpLine(fh, "------ ------------------ ---------- ------------------ ---------- ---------- -------- ------- ------- "
                 "---------------------------------------------------");

    for (size_t i = 0; i < hashSize; i++)
    {
        sAllocUnit* ptr = hashTable[i];
        while (ptr)
        {
            dumpLine(fh, "% 6zu 0x%016zX 0x%08zX 0x%016zX 0x%08zX %-10s %-8s    %c       %c    %s", ptr->allocationNumber,
                     (size_t)(uintptr_t)(ptr->reportedAddress), ptr->reportedSize, (size_t)(uintptr_t)(ptr->actualAddress), ptr->actualSize,
                     "unqueried", allocationTypes[ptr->allocationType],
                     ptr->breakOnDealloc ? 'Y' : 'N', ptr->breakOnRealloc ? 'Y' : 'N',
                     ownerString(ptr->sourceFile, ptr->sourceLine, ptr->sourceFunc));
#if MMGR_BACKTRACE
            dumpBacktrace(fh, ptr);
#endif
            ptr = ptr->next;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------

static void dumpLeakReport(void)
{
    FILE* fh = NULL;
    char* outputFileName = NULL;

    MUTEX_LOCK(allocMutex);

    if (mAppName)
    {
        // Open the report file
        // NOTE: we can't use any allocating FileSystem functions here since
        // the FileSystem may have already been destroyed by the time we get
        // here.
        const size_t directoryLength = strlen(mLogDirectoryStorage);
        const size_t appNameLength = strlen(mAppName);
        const bool hasSeparator = directoryLength != 0 && mLogDirectoryStorage[directoryLength - 1] != '/' &&
                                  mLogDirectoryStorage[directoryLength - 1] != '\\';
        const size_t outputLength = directoryLength + (hasSeparator ? 1 : 0) + appNameLength + strlen(".memleaks") + 1;
        outputFileName = (char*)malloc(outputLength);
        if (outputFileName != NULL)
        {
            const int written = snprintf(outputFileName, outputLength, "%s%s%s.memleaks", mLogDirectoryStorage,
                                         hasSeparator ? "/" : "", mAppName);
            if (written < 0 || (size_t)written >= outputLength)
            {
                free(outputFileName);
                outputFileName = NULL;
            }
        }

        fh = outputFileName != NULL ? mmgrPlatformOpenReport(outputFileName, 0) : NULL;
        if (fh == NULL)
        {
            mmgrPlatformDiagnostic("FluidStudios: unable to write leak report");
        }
    }

    {
        // Header
        time_t    t = time(NULL);
        struct tm tme;
#ifdef _WIN32
        localtime_s(&tme, &t);
#else
        (void)localtime_r(&t, &tme);
#endif
        dumpLine(fh, " -------------------------------------------------------------------------------");
        dumpLine(fh, "                Memory leak report %02d/%02d/%04d %02d:%02d:%02d:                  ", tme.tm_mon + 1, tme.tm_mday,
                 tme.tm_year + 1900, tme.tm_hour, tme.tm_min, tme.tm_sec);
        dumpLine(fh, "                %s                  ", outputFileName ? outputFileName : "<unavailable>");
        // use LF instead of CRLF
        dumpLine(fh, " -------------------------------------------------------------------------------");
        if (stats.totalAllocUnitCount)
        {
            dumpLine(fh, "%zu memory leak%s found:\n", stats.totalAllocUnitCount, stats.totalAllocUnitCount == 1 ? "" : "s");
        }
        else
        {
            dumpLine(fh, "Congratulations! No memory leaks found!");

            // Reporting is observational; manager-owned storage remains valid.
        }

        if (stats.totalAllocUnitCount)
        {
            dumpAllocations(fh);
        }

        char* allMemoryLog = Log("----All Allocations and Deallocations----");

        dumpLine(fh, "%s", allMemoryLog);

        if (!stats.totalAllocUnitCount)
        {
            dumpLine(fh, " ------------------------------------------------------------------------------");
            dumpLine(fh, "Congratulations! No memory leaks found!");
            dumpLine(fh, " ------------------------------------------------------------------------------");
        }
    }

    (void)mmgrPlatformCloseReport(fh);
    free(outputFileName);
    MUTEX_UNLOCK(allocMutex);
}
// ---------------------------------------------------------------------------------------------------------------------------------
// We use a static class to let us know when we're in the midst of static deinitialization
// ---------------------------------------------------------------------------------------------------------------------------------
bool initMemAlloc(const char* appName)
{
    MUTEX_LOCK(allocMutex);
    initializeMmgrUnlocked(appName);
    MUTEX_UNLOCK(allocMutex);
    return true;
}

void exitMemAlloc(void)
{
    dumpLeakReport();
}

void mmgrSetExecutableName(const char* name, size_t length)
{
    if (name == NULL)
        return;
    if (length >= sizeof(mAppNameStorage))
    {
        mmgrPlatformDiagnostic("FluidStudios: application name is too long");
        return;
    }
    MUTEX_LOCK(allocMutex);
    memcpy(mAppNameStorage, name, length);
    mAppNameStorage[length] = '\0';
    mAppName = mAppNameStorage;
    MUTEX_UNLOCK(allocMutex);
}

void mmgrSetLogFileDirectory(const char* directory)
{
    MUTEX_LOCK(allocMutex);
    if (directory == NULL)
    {
        mLogDirectoryStorage[0] = '\0';
        MUTEX_UNLOCK(allocMutex);
        return;
    }
    const size_t length = strlen(directory);
    if (length >= sizeof(mLogDirectoryStorage))
    {
        mLogDirectoryStorage[0] = '\0';
        mmgrPlatformDiagnostic("FluidStudios: log directory is too long");
        MUTEX_UNLOCK(allocMutex);
        return;
    }
    memcpy(mLogDirectoryStorage, directory, length + 1);
    MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- Flags & options -- Call these routines to enable/disable the following options
// ---------------------------------------------------------------------------------------------------------------------------------

void mmgrSetAlwaysValidateAll(bool enabled)
{
    MUTEX_LOCK(allocMutex); alwaysValidateAll = enabled; MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------

void mmgrSetAlwaysLogAll(bool enabled)
{
    MUTEX_LOCK(allocMutex); alwaysLogAll = enabled; MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------

void mmgrSetAlwaysWipeAll(bool enabled)
{
    MUTEX_LOCK(allocMutex); alwaysWipeAll = enabled; MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------

void mmgrSetRandomWipe(bool enabled)
{
    MUTEX_LOCK(allocMutex); randomWipe = enabled; MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- Simply call this routine with the address of an allocated block of RAM, to cause it to force a breakpoint when it is
// reallocated.
// ---------------------------------------------------------------------------------------------------------------------------------

void mmgrSetBreakOnRealloc(void* reportedAddress, bool enabled)
{
    MUTEX_LOCK(allocMutex);
    // Locate the existing allocation unit

    sAllocUnit* au = findAllocUnit(reportedAddress);

    // If you hit this assert, you tried to set a breakpoint on reallocation for an address that doesn't exist. Interrogate the
    // stack frame or the variable 'au' to see which allocation this is.
    if (au == NULL || !(au->allocationType == m_alloc_malloc || au->allocationType == m_alloc_calloc || au->allocationType == m_alloc_realloc))
    {
        MUTEX_UNLOCK(allocMutex);
        return;
    }

    // If you hit this assert, you tried to set a breakpoint on reallocation for an address that wasn't allocated in a way that
    // is compatible with reallocation.
    au->breakOnRealloc = enabled;
    MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- Simply call this routine with the address of an allocated block of RAM, to cause it to force a breakpoint when it is
// deallocated.
// ---------------------------------------------------------------------------------------------------------------------------------

void mmgrSetBreakOnDealloc(void* reportedAddress, bool enabled)
{
    MUTEX_LOCK(allocMutex);
    // Locate the existing allocation unit

    sAllocUnit* au = findAllocUnit(reportedAddress);

    // If you hit this assert, you tried to set a breakpoint on deallocation for an address that doesn't exist. Interrogate the
    // stack frame or the variable 'au' to see which allocation this is.
    if (au == NULL)
    {
        MUTEX_UNLOCK(allocMutex);
        return;
    }
    au->breakOnDealloc = enabled;
    MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- When tracking down a difficult bug, use this routine to force a breakpoint on a specific allocation count
// ---------------------------------------------------------------------------------------------------------------------------------

void m_breakOnAllocation(size_t count)
{
    MUTEX_LOCK(allocMutex);
    breakOnAllocationCount = count;
    MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Used by the macros
// ---------------------------------------------------------------------------------------------------------------------------------

void mmgrSetOwner(const char* file, const unsigned int line, const char* func)
{
    // You're probably wondering about this...
    //
    // It's important for this memory manager to primarily work with global new/delete in their original forms (i.e. with
    // no extra parameters.) In order to do this, we use macros that call this function prior to operators new & delete. This
    // is fine... usually. Here's what actually happens when you use this macro to delete an object:
    //
    // mmgrSetOwner(__FILE__, __LINE__, __FUNCTION__) --> object::~object() --> delete
    //
    // Note that the compiler inserts a call to the object's destructor just prior to calling our overridden operator delete.
    // But what happens when we delete an object whose destructor deletes another object, whose desctuctor deletes another
    // object? Here's a diagram (indentation follows stack depth):
    //
    // mmgrSetOwner(...) -> ~obj1()                          // original call to delete obj1
    //     mmgrSetOwner(...) -> ~obj2()                      // obj1's destructor deletes obj2
    //         mmgrSetOwner(...) -> ~obj3()                  // obj2's destructor deletes obj3
    //             ...                                     // obj3's destructor just does some stuff
    //         delete                                      // back in obj2's destructor, we call delete
    //     delete                                          // back in obj1's destructor, we call delete
    // delete                                              // back to our original call, we call delete
    //
    // Because mmgrSetOwner() just sets up some static variables (below) it's important that each call to mmgrSetOwner() and
    // successive calls to new/delete alternate. However, in this case, three calls to mmgrSetOwner() happen in succession
    // followed by three calls to delete in succession (with a few calls to destructors mixed in for fun.) This means that
    // only the final call to delete (in this chain of events) will have the proper reporting, and the first two in the chain
    // will not have ANY owner-reporting information. The deletes will still work fine, we just won't know who called us.
    //
    // "Then build a stack, my friend!" you might think... but it's a very common thing that people will be working with third-
    // party libraries (including MFC under Windows) which is not compiled with this memory manager's macros. In those cases,
    // mmgrSetOwner() is never called, and rightfully should not have the proper trace-back information. So if one of the
    // destructors in the chain ends up being a call to a delete from a non-mmgr-compiled library, the stack will get confused.
    //
    // I've been unable to find a solution to this problem, but at least we can detect it and report the data before we
    // lose it. That's what this is all about. It makes it somewhat confusing to read in the logs, but at least ALL the
    // information is present...
    //
    // There's a caveat here... The compiler is not required to call operator delete if the value being deleted is NULL.
    // In this case, any call to delete with a NULL will sill call mmgrSetOwner(), which will make mmgrSetOwner() think that
    // there is a destructor chain becuase we setup the variables, but nothing gets called to clear them. Because of this
    // we report a "Possible destructor chain".
    //
    // Thanks to J. Woznack (from Kodiak Interactive Software Studios -- www.kodiakgames.com) for pointing this out.

    MUTEX_LOCK(allocMutex);
    if (tlsSourceLine && alwaysLogAll)
    {
        Log("[I] NOTE! Possible destructor chain: previous owner is %s", ownerString(tlsSourceFile, tlsSourceLine, tlsSourceFunc));
    }

    // Okay... save this stuff off so we can keep track of the caller

    tlsSourceFile = file ? file : "??";
    tlsSourceLine = line;
    tlsSourceFunc = func ? func : "??";
    MUTEX_UNLOCK(allocMutex);
}

void memSetStackSkipCount(int stackDepth)
{
    UNREF_PARAM(stackDepth); 
#if MMGR_BACKTRACE
    // Only set if another call hasn't set this previously
    if (!stackSkipCount)
    {
        stackSkipCount = stackDepth < 0 ? 0 : stackDepth;
    }
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------------

static void resetGlobals(void)
{
    tlsSourceFile = "??";
    tlsSourceLine = 0;
    tlsSourceFunc = "??";

#if MMGR_BACKTRACE
    stackSkipCount = 0;
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Allocate memory and track it
// ---------------------------------------------------------------------------------------------------------------------------------
void* mmgrAllocator(const char* sourceFile, const unsigned int sourceLine, const char* sourceFunc, const unsigned int allocationType,
                    size_t alignment, size_t reportedSize)
{
    if (!validAlignment(alignment) || !isValidAllocationType(allocationType))
        return NULL;
    if (alignment < mmgrMaxAlignment())
        alignment = mmgrMaxAlignment();
    size_t actualSize;
    if (!checkedAllocationSize(reportedSize, alignment, &actualSize))
        return NULL;

    // if (!allocMutex)
    //	/* process-lifetime lock */
    //

    MUTEX_LOCK(allocMutex);
    {
        initializeMmgrUnlocked(NULL);
        if (currentAllocationCount == SIZE_MAX)
        {
            MUTEX_UNLOCK(allocMutex);
            return NULL;
        }
#ifdef TEST_MEMORY_MANAGER
        Log("[D] ENTER: mmgrAllocator()");
#endif

        // Log the request

        if (alwaysLogAll)
            Log("[+] %05zu %8s of size 0x%08zX(%08zu) by %s", currentAllocationCount, allocationTypes[allocationType], reportedSize,
                reportedSize, ownerString(sourceFile, sourceLine, sourceFunc));

        // If you hit this assert, you requested a breakpoint on a specific allocation count
        if (breakOnAllocationCount != 0)
            MMGR_ASSERT(currentAllocationCount != breakOnAllocationCount);

        // If necessary, grow the reservoir of unused allocation units

        if (!reservoir)
        {
            // Allocate 256 reservoir elements

            if (mmgrTestShouldFail(MMGR_TEST_FAIL_RESERVOIR) || sizeof(sAllocUnit) > SIZE_MAX / 256)
            {
                MUTEX_UNLOCK(allocMutex);
                return NULL;
            }
            sAllocUnit* newReservoir = (sAllocUnit*)malloc(sizeof(sAllocUnit) * 256);

            // If you hit this assert, then the memory manager failed to allocate internal memory for tracking the
            // allocations
            MMGR_ASSERT(newReservoir != NULL);

            // Danger Will Robinson!

            if (newReservoir == NULL)
            {
                MUTEX_UNLOCK(allocMutex);
                return NULL;
            }
            // Build a linked-list of the elements in our reservoir

            if (mmgrTestShouldFail(MMGR_TEST_FAIL_POOL_LIST))
            {
                free(newReservoir);
                MUTEX_UNLOCK(allocMutex);
                return NULL;
            }
            memset(newReservoir, 0, sizeof(sAllocUnit) * 256);
            for (unsigned int i = 0; i < 256 - 1; i++)
            {
                newReservoir[i].next = &newReservoir[i + 1];
            }

            // Add this address to our reservoirBuffer so we can free it later

            if (reservoirBufferSize >= SIZE_MAX / sizeof(sAllocUnit*))
            {
                free(newReservoir);
                MUTEX_UNLOCK(allocMutex);
                return NULL;
            }
            sAllocUnit** temp = (sAllocUnit**)realloc(reservoirBuffer, (reservoirBufferSize + 1) * sizeof(sAllocUnit*));
            MMGR_ASSERT(temp);
            if (temp)
            {
                reservoirBuffer = temp;
                reservoirBuffer[reservoirBufferSize++] = newReservoir;
                reservoir = newReservoir;
            }
            else { free(newReservoir); MUTEX_UNLOCK(allocMutex); return NULL; }
        }

        // Logical flow says this should never happen...
        MMGR_ASSERT(reservoir != NULL);

        // Grab a new allocaton unit from the front of the reservoir

        sAllocUnit* au = reservoir;
        reservoir = au->next;

        // Populate it with some real data

        memset(au, 0, sizeof(sAllocUnit));
        au->actualSize = actualSize;
#ifdef FORCE_EXACT_ALIGNMENT
        /* included by checkedAllocationSize */
#endif
#ifdef RANDOM_FAILURE
        double a = rand();
        double b = RAND_MAX / 100.0 * RANDOM_FAILURE;
        if (a > b)
        {
            au->actualAddress = malloc(au->actualSize);
        }
        else
        {
            Log("[F] Random faiure");
            au->actualAddress = NULL;
        }
#else
        au->actualAddress = mmgrTestShouldFail(MMGR_TEST_FAIL_RAW) ? NULL : malloc(au->actualSize);
#endif
        au->reportedSize = reportedSize;
        au->alignment = alignment;
        au->allocationType = allocationType;
        au->sourceLine = sourceLine;

        /* Do not perform pointer arithmetic or capture a trace for a failed
           raw allocation. */
        if (au->actualAddress == NULL)
        {
            au->next = reservoir;
            reservoir = au;
            MUTEX_UNLOCK(allocMutex);
            return NULL;
        }
        au->reportedAddress = calculateReportedAddress(au->actualAddress);

        // Make sure the address we return to user is aligned to the specified alignment
        size_t offset = ((size_t)au->reportedAddress) % alignment;
        if (offset)
        {
            au->reportedAddress = (uint8_t*)au->reportedAddress + (alignment - offset);
        }

#ifdef FORCE_EXACT_ALIGNMENT
        if (!((size_t)au->reportedAddress & alignment))
        {
            // Try to "unalign" this pointer as much as possible, to make repeatable the
            // "random" failures during testing due to insufficiently strict alignment
            // during allocations
            au->reportedAddress = (uint8_t*)au->reportedAddress + alignment;
            offset += alignment;
        }
#endif

        au->offset = offset;

        if (sourceFile)
            tf_strncpyarr(au->sourceFile, sourceFileStripper(sourceFile), sizeof(au->sourceFile) - 1);
        else
            tf_strcpyarr(au->sourceFile, "??");
        if (sourceFunc)
            tf_strncpyarr(au->sourceFunc, sourceFunc, sizeof(au->sourceFunc) - 1);
        else
            tf_strcpyarr(au->sourceFunc, "??");

#if MMGR_BACKTRACE
#ifdef _WIN32
        au->backtrace_nptrs = CaptureStackBackTrace(stackCaptureSkip(), MMGR_BACKTRACE_SIZE, au->backtrace_buffer, NULL);
        // Skipped for us above
        au->backtrace_skip = 0;
#else
        au->backtrace_nptrs = backtrace(au->backtrace_buffer, MMGR_BACKTRACE_SIZE);
        au->backtrace_skip = stackCaptureSkip();
#endif
#endif

        // We don't want to assert with random failures, because we want the application to deal with them.

#ifndef RANDOM_FAILURE
        // If you hit this assert, then the requested allocation simply failed (you're out of memory.) Interrogate the
        // variable 'au' or the stack frame to see what you were trying to do.
        MMGR_ASSERT(au->actualAddress != NULL);
#endif

        // If you hit this assert, then this allocation was made from a source that isn't setup to use this memory tracking
        // software, use the stack frame to locate the source and include our H file.
        MMGR_ASSERT(allocationType != m_alloc_unknown);

        /* Check accounting before publishing either the hash entry or the
           allocation number.  A failed allocation is invisible. */
        sMStats nextStats;
        if (!checkedStatsAdd(&stats, au->reportedSize, au->actualSize, &nextStats))
        {
            free(au->actualAddress);
            au->next = reservoir;
            reservoir = au;
            MUTEX_UNLOCK(allocMutex);
            return NULL;
        }

        currentAllocationCount++;
        au->allocationNumber = currentAllocationCount;

        // Insert the new allocation into the hash table

        size_t hashIndex = (((size_t)au->reportedAddress) >> 4) & (hashSize - 1);
        if (hashTable[hashIndex])
            hashTable[hashIndex]->prev = au;
        au->next = hashTable[hashIndex];
        au->prev = NULL;
        hashTable[hashIndex] = au;
        stats = nextStats;

        wipeWithPattern(au, unusedPattern, 0);
        if (allocationType == m_alloc_calloc)
            memset(au->reportedAddress, 0, au->reportedSize);
        if (alwaysValidateAll)
            validateAllAllocUnitsUnlocked();
        if (alwaysLogAll)
            Log("[+] ---->             addr 0x%08zX", (size_t)(au->reportedAddress));
        resetGlobals();

        MUTEX_UNLOCK(allocMutex);
        return au->reportedAddress;
    }
}

/* Reallocation is a transactional move.  The old raw block is never handed
   to realloc: until the replacement and the complete accounting transition
   are ready, the old record remains the sole published allocation. */
void* mmgrReallocator(const char* sourceFile, const unsigned int sourceLine, const char* sourceFunc, const unsigned int reallocationType,
                     size_t reportedSize, void* reportedAddress)
{
    if (!isValidReallocationType(reallocationType))
    {
        invalidOperationDiagnostic("reallocation type", reportedAddress);
        return NULL;
    }
    if (reportedAddress == NULL)
        return mmgrAllocator(sourceFile, sourceLine, sourceFunc, m_alloc_malloc, mmgrMaxAlignment(), reportedSize);
    if (reportedSize == 0)
    {
        mmgrDeallocator(sourceFile, sourceLine, sourceFunc, m_alloc_free, reportedAddress);
        return NULL;
    }

    MUTEX_LOCK(allocMutex);
    initializeMmgrUnlocked(NULL);
    if (currentAllocationCount == SIZE_MAX)
    {
        MUTEX_UNLOCK(allocMutex);
        return NULL;
    }
    sAllocUnit* au = findAllocUnit(reportedAddress);
    if (au == NULL)
    {
        invalidOperationDiagnostic("reallocation of unknown or interior", reportedAddress);
        MUTEX_UNLOCK(allocMutex);
        return NULL;
    }
    if (!(au->allocationType == m_alloc_malloc || au->allocationType == m_alloc_calloc || au->allocationType == m_alloc_realloc))
    {
        invalidOperationDiagnostic("mismatched reallocation", reportedAddress);
        MUTEX_UNLOCK(allocMutex);
        return NULL;
    }
    if (!validateAllocUnitUnlocked(au))
    {
        MUTEX_UNLOCK(allocMutex);
        return NULL;
    }
    if (au->breakOnRealloc)
        MMGR_ASSERT(false);

    const size_t alignment = au->alignment;
    void* oldActualAddress = au->actualAddress;
    size_t newActualSize;
    if (!checkedAllocationSize(reportedSize, alignment, &newActualSize))
    {
        MUTEX_UNLOCK(allocMutex);
        return NULL;
    }

    sMStats nextStats;
    if (!checkedStatsReallocate(&stats, au->reportedSize, au->actualSize, reportedSize, newActualSize, &nextStats))
    {
        MUTEX_UNLOCK(allocMutex);
        return NULL;
    }

    void* replacement = mmgrTestShouldFail(MMGR_TEST_FAIL_RAW) ? NULL : malloc(newActualSize);
    if (replacement == NULL)
    {
        MUTEX_UNLOCK(allocMutex);
        return NULL;
    }
    void* replacementReported = calculateReportedAddress(replacement);
    size_t offset = ((size_t)replacementReported) % alignment;
    if (offset) offset = alignment - offset;
    replacementReported = (uint8_t*)replacementReported + offset;
#ifdef FORCE_EXACT_ALIGNMENT
    if (!((size_t)replacementReported & alignment))
    {
        replacementReported = (uint8_t*)replacementReported + alignment;
        offset += alignment;
    }
#endif
    const size_t copySize = au->reportedSize < reportedSize ? au->reportedSize : reportedSize;
    if (copySize) memcpy(replacementReported, au->reportedAddress, copySize);

    sAllocUnit candidate = *au;
    candidate.actualAddress = replacement;
    candidate.actualSize = newActualSize;
    candidate.reportedAddress = replacementReported;
    candidate.reportedSize = reportedSize;
    candidate.offset = offset;
    candidate.allocationType = m_alloc_realloc;
    candidate.allocationNumber = currentAllocationCount + 1;
    candidate.sourceLine = sourceLine;
    tf_strncpyarr(candidate.sourceFile, sourceFile ? sourceFileStripper(sourceFile) : "??", sizeof(candidate.sourceFile) - 1);
    tf_strncpyarr(candidate.sourceFunc, sourceFunc ? sourceFunc : "??", sizeof(candidate.sourceFunc) - 1);
#if MMGR_BACKTRACE
#ifdef _WIN32
    candidate.backtrace_nptrs = CaptureStackBackTrace(stackCaptureSkip(), MMGR_BACKTRACE_SIZE, candidate.backtrace_buffer, NULL);
    candidate.backtrace_skip = 0;
#else
    candidate.backtrace_nptrs = backtrace(candidate.backtrace_buffer, MMGR_BACKTRACE_SIZE);
    candidate.backtrace_skip = stackCaptureSkip();
#endif
#endif
    wipeWithPattern(&candidate, unusedPattern, copySize);

    const size_t oldHash = (((size_t)au->reportedAddress) >> 4) & (hashSize - 1);
    const size_t newHash = (((size_t)candidate.reportedAddress) >> 4) & (hashSize - 1);
    if (oldHash != newHash)
    {
        if (au->prev) au->prev->next = au->next;
        else hashTable[oldHash] = au->next;
        if (au->next) au->next->prev = au->prev;
        candidate.prev = NULL;
        candidate.next = hashTable[newHash];
        if (candidate.next) candidate.next->prev = au;
        hashTable[newHash] = au;
    }
    currentAllocationCount++;
    *au = candidate;
    stats = nextStats;
    free(oldActualAddress);
    resetGlobals();
    MUTEX_UNLOCK(allocMutex);
    return au->reportedAddress;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Deallocate memory and track it
// ---------------------------------------------------------------------------------------------------------------------------------

void mmgrDeallocator(const char* sourceFile, const unsigned int sourceLine, const char* sourceFunc, const unsigned int deallocationType,
                     const void* reportedAddress)
{
    /* Validate before indexing allocationTypes or taking any stateful path. */
    if (!isValidDeallocationType(deallocationType))
        return;
    /* Process-lifetime lock; allocation and logging locks are distinct. */
    MUTEX_LOCK(allocMutex);
#ifdef TEST_MEMORY_MANAGER
    Log("[D] ENTER: mmgrDeallocator()");
#endif

    // Log the request

    if (alwaysLogAll)
        Log("[-] ----- %8s of addr 0x%08zX           by %s", allocationTypes[deallocationType], (size_t)((void*)(reportedAddress)),
            ownerString(sourceFile, sourceLine, sourceFunc));

    // We should only ever get here with a null pointer if they try to do so with a call to free() (delete[] and delete will
    // both bail before they get here.) So, since ANSI allows free(NULL), we'll not bother trying to actually free the allocated
    // memory or track it any further.

    if (reportedAddress)
    {
        // Go get the allocation unit

        sAllocUnit* au = findAllocUnit(reportedAddress);

        // If you hit this assert, you tried to deallocate RAM that wasn't allocated by this memory manager.
        MMGR_ASSERT(au != NULL);
        if (au == NULL)
        {
            invalidOperationDiagnostic("deallocation of unknown or interior", reportedAddress);
            MUTEX_UNLOCK(allocMutex);
            return;
        }

        // If asan is active then unpoision the memory that may have been poisoned by another
        // library. Otherwise we trip asan here during the validation steps
        ASAN_UNPOISON(au->actualAddress, au->actualSize);

        // If you hit this assert, then the allocation unit that is about to be deallocated is damaged. But you probably
        // already know that from a previous assert you should have seen in validateAllocUnit() :)
        if (!validateAllocUnitUnlocked(au))
        {
            MUTEX_UNLOCK(allocMutex);
            return;
        }

        // If you hit this assert, then this deallocation was made from a source that isn't setup to use this memory
        // tracking software, use the stack frame to locate the source and include our H file.
        // If you hit this assert, you were trying to deallocate RAM that was not allocated in a way that is compatible with
        // the deallocation method requested. In other words, you have a allocation/deallocation mismatch.
        if (!((deallocationType == m_alloc_delete && au->allocationType == m_alloc_new) ||
                 (deallocationType == m_alloc_delete_array && au->allocationType == m_alloc_new_array) ||
                 (deallocationType == m_alloc_free && au->allocationType == m_alloc_malloc) ||
                 (deallocationType == m_alloc_free && au->allocationType == m_alloc_calloc) ||
                 (deallocationType == m_alloc_free && au->allocationType == m_alloc_realloc)))
        {
            invalidOperationDiagnostic("mismatched deallocation", reportedAddress);
            MUTEX_UNLOCK(allocMutex);
            return;
        }

        // If you hit this assert, then the "break on dealloc" flag for this allocation unit is set. Interrogate the 'au'
        // variable to determine information about this allocation unit.
        MMGR_ASSERT(au->breakOnDealloc == false);

        // Wipe the deallocated RAM with a new pattern. This doen't actually do us much good in debug mode under WIN32,
        // because Microsoft's memory debugging & tracking utilities will wipe it right after we do. Oh well.

        wipeWithPattern(au, releasedPattern, 0);

        // Inform asan that the memory is poisoned (again). Free should handle this for us but
        // we might as well
        ASAN_POISON(au->actualAddress, au->actualSize);

        // Do the deallocation

        free(au->actualAddress);

        // Remove this allocation unit from the hash table

        size_t hashIndex = ((size_t)(au->reportedAddress) >> 4) & (hashSize - 1);
        if (hashTable[hashIndex] == au)
        {
            hashTable[hashIndex] = au->next;
            if (hashTable[hashIndex])
                hashTable[hashIndex]->prev = NULL;
        }
        else
        {
            if (au->prev)
                au->prev->next = au->next;
            if (au->next)
                au->next->prev = au->prev;
        }

        // Remove this allocation from our stats

        if (stats.totalReportedMemory >= au->reportedSize)
            stats.totalReportedMemory -= au->reportedSize;
        if (stats.totalActualMemory >= au->actualSize)
            stats.totalActualMemory -= au->actualSize;
        if (stats.totalAllocUnitCount)
            stats.totalAllocUnitCount--;

        // Add this allocation unit to the front of our reservoir of unused allocation units

        memset(au, 0, sizeof(sAllocUnit));
        au->next = reservoir;
        reservoir = au;
    }

    // Resetting the globals insures that if at some later time, somebody calls our memory manager from an unknown
    // source (i.e. they didn't include our H file) then we won't think it was the last allocation.

    resetGlobals();

    // Validate every single allocated unit in memory

    if (alwaysValidateAll)
        validateAllAllocUnitsUnlocked();

#ifdef TEST_MEMORY_MANAGER
    Log("[D] EXIT : mmgrDeallocator()");
#endif
    MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- The following utilitarian allow you to become proactive in tracking your own memory, or help you narrow in on those tough
// bugs.
// ---------------------------------------------------------------------------------------------------------------------------------

bool mmgrValidateAddress(const void* reportedAddress)
{
    // Just see if the address exists in our allocation routines

    MUTEX_LOCK(allocMutex);
    const bool result = findAllocUnit(reportedAddress) != NULL;
    MUTEX_UNLOCK(allocMutex);
    return result;
}

bool mmgrContainsAddress(const void* address)
{
    MUTEX_LOCK(allocMutex);
    const bool result = containsAddressUnlocked(address);
    MUTEX_UNLOCK(allocMutex);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------------------

static bool validateAllocUnitUnlocked(const sAllocUnit* allocUnit)
{
    // Make sure the padding is untouched

    uint8_t*     pre = ((uint8_t*)allocUnit->reportedAddress - paddingSize * sizeof(uint32_t));
    uint8_t*     post = ((uint8_t*)allocUnit->reportedAddress + allocUnit->reportedSize);
    bool         errorFlag = false;
    const size_t paddingBytes = paddingSize * sizeof(uint32_t);
    for (size_t i = 0; i < paddingBytes; i++, pre++, post++)
    {
        const uint8_t expectedPrefixByte = (prefixPattern >> ((i % sizeof(uint32_t)) * 8)) & 0xFF;
        if (*pre != expectedPrefixByte)
        {
            Log("[!] A memory allocation unit was corrupt because of an underrun:");
            dumpAllocUnitUnlocked(allocUnit, "  ");
            errorFlag = true;
        }

        // If you hit this assert, then you should know that this allocation unit has been damaged. Something (possibly the
        // owner?) has underrun the allocation unit (modified a few bytes prior to the start). You can interrogate the
        // variable 'allocUnit' to see statistics and information about this damaged allocation unit.
        MMGR_ASSERT(*pre == expectedPrefixByte);

        const uint8_t expectedPostfixByte = (postfixPattern >> ((i % sizeof(uint32_t)) * 8)) & 0xFF;
        if (*post != expectedPostfixByte)
        {
            Log("[!] A memory allocation unit was corrupt because of an overrun:");
            dumpAllocUnitUnlocked(allocUnit, "  ");
            errorFlag = true;
        }

        // If you hit this assert, then you should know that this allocation unit has been damaged. Something (possibly the
        // owner?) has overrun the allocation unit (modified a few bytes after the end). You can interrogate the variable
        // 'allocUnit' to see statistics and information about this damaged allocation unit.
        MMGR_ASSERT(*post == expectedPostfixByte);
    }

    // Return the error status (we invert it, because a return of 'false' means error)

    return !errorFlag;
}

// ---------------------------------------------------------------------------------------------------------------------------------

static bool validateAllAllocUnitsUnlocked(void)
{
    // Just go through each allocation unit in the hash table and count the ones that have errors

    size_t errors = 0;
    size_t allocCount = 0;
    for (unsigned int i = 0; i < hashSize; i++)
    {
        sAllocUnit* ptr = hashTable[i];
        while (ptr)
        {
            allocCount++;
            if (!validateAllocUnitUnlocked(ptr))
                errors++;
            ptr = ptr->next;
        }
    }

    // Test for hash-table correctness

    if (allocCount != stats.totalAllocUnitCount)
    {
        Log("[!] Memory tracking hash table corrupt!");
        errors++;
    }

    // If you hit this assert, then the internal memory (hash table) used by this memory tracking software is damaged! The
    // best way to track this down is to use the alwaysLogAll flag in conjunction with STRESS_TEST macro to narrow in on the
    // offending code. After running the application with these settings (and hitting this assert again), interrogate the
    // memory.log file to find the previous successful operation. The corruption will have occurred between that point and this
    // assertion.
    MMGR_ASSERT(allocCount == stats.totalAllocUnitCount);

    // If you hit this assert, then you've probably already been notified that there was a problem with a allocation unit in a
    // prior call to validateAllocUnit(), but this assert is here just to make sure you know about it. :)
    MMGR_ASSERT(errors == 0);

    // Log any errors

    if (errors)
        Log("[!] While validating all allocation units, %zu allocation unit(s) were found to have problems", errors);

    // Return the error status

    return errors == 0;
}

bool mmgrValidateAllocUnit(const sAllocUnit* allocUnit)
{
    if (allocUnit == NULL)
        return false;
    MUTEX_LOCK(allocMutex);
    const bool result = findAllocUnitRecordUnlocked(allocUnit) && validateAllocUnitUnlocked(allocUnit);
    MUTEX_UNLOCK(allocMutex);
    return result;
}

bool mmgrValidateAllAllocUnits(void)
{
    MUTEX_LOCK(allocMutex);
    const bool result = validateAllAllocUnitsUnlocked();
    MUTEX_UNLOCK(allocMutex);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- Unused RAM calculation routines. Use these to determine how much of your RAM is unused (in bytes)
// ---------------------------------------------------------------------------------------------------------------------------------

static size_t calcUnusedUnlocked(const sAllocUnit* allocUnit)
{
    const uint8_t* ptr = (const uint8_t*)(allocUnit->reportedAddress);
    size_t count = 0;

    for (size_t i = 0; i < allocUnit->reportedSize; ++i)
    {
        const uint8_t expected = (uint8_t)(unusedPattern >> ((i % sizeof(uint32_t)) * 8));
        if (ptr[i] == expected)
            ++count;
    }

    return count;
}

// ---------------------------------------------------------------------------------------------------------------------------------

static size_t calcAllUnusedUnlocked(void)
{
    // Just go through each allocation unit in the hash table and count the unused RAM

    size_t total = 0;
    for (unsigned int i = 0; i < hashSize; i++)
    {
        sAllocUnit* ptr = hashTable[i];
        while (ptr)
        {
            const size_t unused = calcUnusedUnlocked(ptr);
            if (total > SIZE_MAX - unused)
                return SIZE_MAX;
            total += unused;
            ptr = ptr->next;
        }
    }

    return total;
}

size_t mmgrCalcUnused(const sAllocUnit* allocUnit)
{
    if (allocUnit == NULL)
        return 0;
    MUTEX_LOCK(allocMutex);
    const size_t result = findAllocUnitRecordUnlocked(allocUnit) ? calcUnusedUnlocked(allocUnit) : 0;
    MUTEX_UNLOCK(allocMutex);
    return result;
}

size_t mmgrCalcAllUnused(void)
{
    MUTEX_LOCK(allocMutex);
    const size_t result = calcAllUnusedUnlocked();
    MUTEX_UNLOCK(allocMutex);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// -DOC- The following functions are for logging and statistics reporting.
// ---------------------------------------------------------------------------------------------------------------------------------

static void dumpAllocUnitUnlocked(const sAllocUnit* allocUnit, const char* prefix)
{
    Log("[I] %sAddress (reported): %010p", prefix, allocUnit->reportedAddress);
    Log("[I] %sAddress (actual)  : %010p", prefix, allocUnit->actualAddress);
    Log("[I] %sSize (reported)   : 0x%08zX (%s)", prefix, allocUnit->reportedSize,
        memorySizeString(allocUnit->reportedSize));
    Log("[I] %sSize (actual)     : 0x%08zX (%s)", prefix, allocUnit->actualSize,
        memorySizeString(allocUnit->actualSize));
    Log("[I] %sOwner             : %s(%u)::%s", prefix, allocUnit->sourceFile, allocUnit->sourceLine, allocUnit->sourceFunc);

#if MMGR_BACKTRACE
    Log("[I] %sBacktrace             : ", prefix);
    dumpBacktrace(NULL, allocUnit);
#endif

    Log("[I] %sAllocation type   : %s", prefix, allocationTypes[allocUnit->allocationType]);
    Log("[I] %sAllocation number : %zu", prefix, allocUnit->allocationNumber);
}

void mmgrDumpAllocUnit(const sAllocUnit* allocUnit, const char* prefix)
{
    if (allocUnit == NULL)
        return;
    MUTEX_LOCK(allocMutex);
    if (findAllocUnitRecordUnlocked(allocUnit))
        dumpAllocUnitUnlocked(allocUnit, prefix ? prefix : "");
    MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------
static void fsPrintf(FILE* fileStream, const char* format, ...)
{
    va_list args;
    char    buffer[BUFFER_SIZE] = { 0 };
    va_start(args, format);
    vsnprintf(buffer, BUFFER_SIZE, format, args);
    va_end(args);
    const size_t length = strlen(buffer);
    if (fwrite(buffer, 1, length, fileStream) != length)
        mmgrPlatformDiagnostic("FluidStudios: report write failed");
}

void mmgrDumpMemoryReport(const char* filename, const bool overwrite)
{
    FILE* fh = NULL;
    if (filename == NULL || filename[0] == '\0')
    {
        mmgrPlatformDiagnostic("FluidStudios: memory report path is null or empty");
        return;
    }

    MUTEX_LOCK(allocMutex);
    const size_t filenameLength = strlen(filename);
    const size_t directoryLength = strlen(mLogDirectoryStorage);
    const bool relative = filename[0] != '/' && filename[0] != '\\' && !(filenameLength > 1 && filename[1] == ':');
    const bool hasDirectory = relative && directoryLength != 0;
    const bool hasSeparator = hasDirectory && mLogDirectoryStorage[directoryLength - 1] != '/' &&
                              mLogDirectoryStorage[directoryLength - 1] != '\\';
    const size_t prefixLength = hasDirectory ? directoryLength + (hasSeparator ? 1 : 0) : 0;
    if (filenameLength > SIZE_MAX - prefixLength - 1)
    {
        mmgrPlatformDiagnostic("FluidStudios: memory report path size overflow");
        MUTEX_UNLOCK(allocMutex);
        return;
    }
    const size_t outputLength = prefixLength + filenameLength + 1;
    char* outputFileName = (char*)malloc(outputLength);
    if (outputFileName == NULL)
    {
        mmgrPlatformDiagnostic("FluidStudios: unable to allocate memory report path");
        MUTEX_UNLOCK(allocMutex);
        return;
    }
    const int pathWritten = snprintf(outputFileName, outputLength, "%s%s%s", hasDirectory ? mLogDirectoryStorage : "",
                                     hasSeparator ? "/" : "", filename);
    if (pathWritten < 0 || (size_t)pathWritten >= outputLength)
    {
        mmgrPlatformDiagnostic("FluidStudios: memory report path is too long");
        free(outputFileName);
        MUTEX_UNLOCK(allocMutex);
        return;
    }

    /* Keep the complete report as one allocation-bookkeeping snapshot. */
    if (mmgrTestShouldFail(MMGR_TEST_FAIL_REPORT))
    {
        mmgrPlatformDiagnostic("FluidStudios: report failure injected");
        free(outputFileName);
        MUTEX_UNLOCK(allocMutex);
        return;
    }
    fh = mmgrPlatformOpenReport(outputFileName, overwrite ? 0 : 1);

    if (fh == NULL)
    {
        mmgrPlatformDiagnostic("FluidStudios: unable to write memory report");
        free(outputFileName);
        MUTEX_UNLOCK(allocMutex);
        return;
    }

        // Header
        static char timeString[25];
        memset(timeString, 0, sizeof(timeString));
        time_t    t = time(NULL);
        struct tm tme;
#ifdef _WIN32
        localtime_s(&tme, &t);
#else
        (void)localtime_r(&t, &tme);
#endif

        fsPrintf(
            fh,
            " -----------------------------------------------------------------------------------------------------------------------------"
            "-----\n");
        fsPrintf(
            fh,
            "|                                             Memory report for: %02d/%02d/%04d %02d:%02d:%02d                                "
            "          |\n",
            tme.tm_mon + 1, tme.tm_mday, tme.tm_year + 1900, tme.tm_hour, tme.tm_min, tme.tm_sec);
        fsPrintf(
            fh,
            " -----------------------------------------------------------------------------------------------------------------------------"
            "-----\n");
        fsPrintf(fh, "\n");

        // Report summary
        fsPrintf(
            fh,
            " -----------------------------------------------------------------------------------------------------------------------------"
            "----- \n");
        fsPrintf(
            fh,
            "|                                                           T O T A L S                                                       "
            "     |\n");
        fsPrintf(
            fh,
            " -----------------------------------------------------------------------------------------------------------------------------"
            "----- \n");
        fsPrintf(fh, "              Allocation unit count: %10s\n", insertCommas(stats.totalAllocUnitCount));
        fsPrintf(fh, "            Reported to application: %s\n", memorySizeString(stats.totalReportedMemory));
        fsPrintf(fh, "         Actual total memory in use: %s\n", memorySizeString(stats.totalActualMemory));
        fsPrintf(fh, "           Memory tracking overhead: %s\n", memorySizeString(stats.totalActualMemory - stats.totalReportedMemory));
        fsPrintf(fh, "\n");

        fsPrintf(
            fh,
            " -----------------------------------------------------------------------------------------------------------------------------"
            "----- \n");
        fsPrintf(
            fh,
            "|                                                            P E A K S                                                        "
            "     |\n");
        fsPrintf(
            fh,
            " -----------------------------------------------------------------------------------------------------------------------------"
            "----- \n");
        fsPrintf(fh, "              Allocation unit count: %10s\n", insertCommas(stats.peakAllocUnitCount));
        fsPrintf(fh, "            Reported to application: %s\n", memorySizeString(stats.peakReportedMemory));
        fsPrintf(fh, "                             Actual: %s\n", memorySizeString(stats.peakActualMemory));
        fsPrintf(fh, "           Memory tracking overhead: not independently measurable (peak reported and actual values are separate snapshots)\n");
        fsPrintf(fh, "\n");

        fsPrintf(
            fh,
            " -----------------------------------------------------------------------------------------------------------------------------"
            "----- \n");
        fsPrintf(
            fh,
            "|                                                      A C C U M U L A T E D                                                  "
            "     |\n");
        fsPrintf(
            fh,
            " -----------------------------------------------------------------------------------------------------------------------------"
            "----- \n");
        fsPrintf(fh, "              Allocation unit count: %s\n", memorySizeString(stats.accumulatedAllocUnitCount));
        fsPrintf(fh, "            Reported to application: %s\n", memorySizeString(stats.accumulatedReportedMemory));
        fsPrintf(fh, "                             Actual: %s\n", memorySizeString(stats.accumulatedActualMemory));
        fsPrintf(fh, "\n");

        fsPrintf(
            fh,
            " -----------------------------------------------------------------------------------------------------------------------------"
            "----- \n");
        fsPrintf(
            fh,
            "|                                                           U N U S E D                                                       "
            "     |\n");
        fsPrintf(
            fh,
            " -----------------------------------------------------------------------------------------------------------------------------"
            "----- \n");
        fsPrintf(fh, "Memory allocated but not in use: unavailable (use mmgrCalcAllUnused while quiescent)\n");
        fsPrintf(fh, "\n");

        dumpAllocations(fh);

    (void)mmgrPlatformCloseReport(fh);
    free(outputFileName);
    MUTEX_UNLOCK(allocMutex);
}

// ---------------------------------------------------------------------------------------------------------------------------------

sMStats mmgrGetMemoryStatistics(void)
{
    sMStats snapshot;
    MUTEX_LOCK(allocMutex);
    snapshot = stats;
    MUTEX_UNLOCK(allocMutex);
    return snapshot;
}

#ifdef MMGR_TESTING
void mmgrTestFailNext(int stage)
{
    MUTEX_LOCK(allocMutex);
    mmgrTestFailureStage = stage;
    MUTEX_UNLOCK(allocMutex);
}

void mmgrTestClearFailure(void)
{
    MUTEX_LOCK(allocMutex);
    mmgrTestFailureStage = 0;
    MUTEX_UNLOCK(allocMutex);
}

size_t mmgrTestHash(const void* address)
{
    if (address == NULL)
        return SIZE_MAX;
    return (((size_t)address) >> 4) & (hashSize - 1);
}

bool mmgrTestAccounting(void)
{
    size_t actual = 0;
    const size_t large = (size_t)UINT32_MAX + (size_t)1;
    if (!checkedAllocationSize(large, sizeof(void*), &actual) || actual <= large)
        return false;
    sMStats synthetic = { large, actual, large, actual, large, actual, 1, 1, large };
    sMStats transitioned;
    if (!checkedStatsAdd(&synthetic, 1, 1, &transitioned))
        return false;
    if (!checkedStatsReallocate(&transitioned, large + 1, actual + 1, large + 9, actual + 9, &synthetic) ||
        synthetic.totalReportedMemory != large + 9 || synthetic.accumulatedReportedMemory != large + 9 ||
        synthetic.totalAllocUnitCount != 2 || synthetic.accumulatedAllocUnitCount != 2)
        return false;
    const sMStats beforeShrink = synthetic;
    if (!checkedStatsReallocate(&synthetic, large + 9, actual + 9, large + 2, actual + 2, &transitioned) ||
        transitioned.totalReportedMemory != large + 2 || transitioned.accumulatedReportedMemory != beforeShrink.accumulatedReportedMemory ||
        transitioned.totalAllocUnitCount != beforeShrink.totalAllocUnitCount)
        return false;

    synthetic.totalReportedMemory = SIZE_MAX;
    if (checkedStatsAdd(&synthetic, 1, 0, &transitioned))
        return false;
    synthetic = beforeShrink;
    synthetic.accumulatedReportedMemory = SIZE_MAX;
    if (checkedStatsReallocate(&synthetic, large + 9, actual + 9, large + 10, actual + 10, &transitioned))
        return false;
    synthetic = beforeShrink;
    synthetic.totalAllocUnitCount = SIZE_MAX;
    synthetic.accumulatedAllocUnitCount = SIZE_MAX;
    return !checkedStatsAdd(&synthetic, 1, 0, &transitioned) &&
           checkedStatsReallocate(&synthetic, large + 9, actual + 9, large + 10, actual + 10, &transitioned);
}

size_t mmgrTestReservoirAvailable(void)
{
    size_t count = 0;
    MUTEX_LOCK(allocMutex);
    for (sAllocUnit* ptr = reservoir; ptr; ptr = ptr->next)
        ++count;
    MUTEX_UNLOCK(allocMutex);
    return count;
}
#endif

static char* LogToMemory(char* log)
{
    static char logMemory[65536];
    static size_t memoryLength = 0;
    size_t logLength = strlen(log);
    size_t available = sizeof(logMemory) - memoryLength - 1;
    if (logLength > available)
        logLength = available;
    if (logLength != 0)
        memcpy(logMemory + memoryLength, log, logLength);
    memoryLength += logLength;
    logMemory[memoryLength] = '\0';
    return logMemory;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// mmgr.cpp - End of file
// ---------------------------------------------------------------------------------------------------------------------------------
