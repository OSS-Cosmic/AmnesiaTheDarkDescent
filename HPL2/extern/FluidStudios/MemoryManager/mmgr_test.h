#ifndef HPL_MMGR_TEST_H
#define HPL_MMGR_TEST_H

#include <stddef.h>
#include <stdbool.h>

#ifndef MMGR_TESTING
#error "mmgr_test.h is private and requires MMGR_TESTING"
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum MmgrTestFailureStage {
    MMGR_TEST_FAIL_RAW = 1,
    MMGR_TEST_FAIL_RESERVOIR = 2,
    MMGR_TEST_FAIL_POOL_LIST = 3,
    MMGR_TEST_FAIL_REPORT = 4
};

void mmgrTestFailNext(int stage);
void mmgrTestClearFailure(void);
size_t mmgrTestHash(const void* address);
bool mmgrTestAccounting(void);
size_t mmgrTestReservoirAvailable(void);

#ifdef __cplusplus
}
#endif

#endif
