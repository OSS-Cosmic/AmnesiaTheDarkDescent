#ifndef HPL_MEMORY_MANAGER_FIXTURE_H
#define HPL_MEMORY_MANAGER_FIXTURE_H

#include "utest.h"
#include "system/MemoryManager.h"
#include <new>
#include <memory>

namespace hpl_memory_test {

struct CaseScope {
    explicit CaseScope(int *result);
    ~CaseScope();

    int *utest_result;
    std::size_t baseline_bytes;
    std::size_t baseline_units;
    std::new_handler previous_new_handler;
    bool previous_log_creation;
};

const char *reportPath();
const char *reportRoot();
bool check(bool condition, int *utest_result, const char *expression,
           const char *file, int line, const char *message);

} // namespace hpl_memory_test

#define HPL_EXPECT(condition, message) \
    (void)::hpl_memory_test::check((condition), utest_result, #condition, \
                                   __FILE__, __LINE__, (message))

#endif
