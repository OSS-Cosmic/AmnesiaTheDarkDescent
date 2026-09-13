#include "system/MemoryManager.h"
#include "hpl_memory_manager_fixture.h"
#include "utest.h"
#include "mmgr.h"
#include "mmgr_test.h"
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
struct Row { std::uintptr_t address; char owner[256]; };
Row rows[256]; std::size_t rowCount = 0;
#define ATTR_EXPECT(result, condition, message) \
    do { int *utest_result = (result); HPL_EXPECT(condition, message); } while (false)

void expectAt(int *result, bool condition, const char *expression,
              const char *file, unsigned line, const char *message) {
    (void)::hpl_memory_test::check(condition, result, expression, file, line, message);
}
bool readRows(const char *path, int *result) {
    rowCount = 0; std::FILE *file = std::fopen(path, "rb");
    if (!file) { expectAt(result, false, "file", __FILE__, __LINE__, "could not open memory report"); return false; }
    char line[1024];
    while (std::fgets(line, sizeof(line), file)) {
        std::size_t number = 0; unsigned long long address = 0; char owner[256] = {};
        const int fields = std::sscanf(line, " %zu 0x%llx %*s %*s %*s %*s %*s %*c %*c %255[^\n]", &number, &address, owner);
        if (fields != 3 || rowCount == sizeof(rows) / sizeof(rows[0])) continue;
        rows[rowCount].address = static_cast<std::uintptr_t>(address);
        std::strncpy(rows[rowCount].owner, owner, sizeof(rows[rowCount].owner) - 1);
        ++rowCount;
    }
    if (std::ferror(file)) { std::fclose(file); expectAt(result, false, "read report", __FILE__, __LINE__, "could not read memory report"); return false; }
    std::fclose(file); return true;
}
const char *ownerFor(const void *pointer) {
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pointer);
    for (std::size_t i = 0; i != rowCount; ++i) if (rows[i].address == address) return rows[i].owner;
    return nullptr;
}
void requireOwner(int *result, const void *pointer, unsigned line, const char *function,
                  const char *checkFile, unsigned checkLine) {
    char expected[256]; const int written = std::snprintf(expected, sizeof(expected),
        "hpl_memory_manager_attribution_tests.cpp(%05u)::%s", line, function);
    const char *actual = ownerFor(pointer);
    char message[768];
    std::snprintf(message, sizeof(message), "expected owner '%s', actual '%s'",
                  expected, actual ? actual : "<missing>");
    expectAt(result, written > 0 && static_cast<std::size_t>(written) < sizeof(expected),
             "owner expectation", checkFile, checkLine, "owner expectation was truncated");
    expectAt(result, actual && std::strcmp(actual, expected) == 0,
             "allocation owner", checkFile, checkLine, message);
}
void requireUnknown(int *result, const void *pointer, const char *checkFile, unsigned checkLine) {
    const char *actual = ownerFor(pointer);
    char message[768];
    std::snprintf(message, sizeof(message), "expected unknown owner, actual '%s'",
                  actual ? actual : "<missing>");
    expectAt(result, actual && std::strcmp(actual, "?" "?(00000)::?" "?") == 0,
             "unknown allocation owner", checkFile, checkLine, message);
}
struct NestedHpl {
    static constexpr unsigned nestedLine = __LINE__ + 3;
    int *inner;
    NestedHpl()
        : inner(hplNew(int, ())) {}
    ~NestedHpl() { hplDelete(inner); }
};
struct OrdinaryConstructor { int *inner; OrdinaryConstructor() : inner(new int(4)) {} ~OrdinaryConstructor() { delete inner; } };
struct ClassSpecific { static void *operator new(std::size_t n) { return std::malloc(n); } static void operator delete(void *p) noexcept { std::free(p); } };
struct ClassSpecificWithOrdinaryConstructor {
    static int *escaped; static void *operator new(std::size_t n) { return std::malloc(n); }
    static void operator delete(void *p) noexcept { std::free(p); } ClassSpecificWithOrdinaryConstructor() { escaped = new int(5); }
};
int *ClassSpecificWithOrdinaryConstructor::escaped = nullptr;
struct Throwing { Throwing() { throw 23; } };
struct DestructorAllocation { static int *escaped; ~DestructorAllocation() { escaped = new int(6); } };
int *DestructorAllocation::escaped = nullptr;
void *handlerAllocation = nullptr; void retryingHandler() { handlerAllocation = ::operator new(13); }
struct Cleanup {
    int *alwaysLogged = nullptr, *sameExpressionOrdinary = nullptr, *arraySideEffect = nullptr, *array = nullptr;
    NestedHpl *outer = nullptr; OrdinaryConstructor *ordinaryConstructor = nullptr; ClassSpecific *classSpecific = nullptr;
    ClassSpecificWithOrdinaryConstructor *classSpecificConstructor = nullptr; int *retried = nullptr;
    void *bufferOnly = nullptr, *buffer = nullptr, *createdBuffer = nullptr;
    int *createdObject = nullptr, *createdArray = nullptr, *afterThrow = nullptr;
    DestructorAllocation *destructorAllocation = nullptr;
    ~Cleanup() {
        mmgrTestClearFailure();
        hpl::cMemoryManager::SetLogCreation(false);
        mmgrSetAlwaysLogAll(false);
        hplDelete(outer); delete ordinaryConstructor; hplDelete(classSpecific); delete sameExpressionOrdinary;
        hplDelete(classSpecificConstructor); delete ClassSpecificWithOrdinaryConstructor::escaped;
        delete arraySideEffect; hplDeleteArray(array); hplDelete(retried); ::operator delete(handlerAllocation);
        delete alwaysLogged;
        hplFree(bufferOnly); hplFree(buffer); hplFree(createdBuffer); hplDelete(createdObject); hplDeleteArray(createdArray);
        hplDelete(destructorAllocation); delete DestructorAllocation::escaped; delete afterThrow;
        ClassSpecificWithOrdinaryConstructor::escaped = nullptr; DestructorAllocation::escaped = nullptr; handlerAllocation = nullptr;
    }
};
}

// This name is part of the report attribution contract.
void testHplAttribution(int *result) {
    Cleanup cleanup;
    const char *reportPath = hpl_memory_test::reportPath();
    mmgrSetAlwaysLogAll(true);
    cleanup.alwaysLogged = new int(1);
    ATTR_EXPECT(result, cleanup.alwaysLogged, "always-log ordinary allocation failed");
    mmgrSetAlwaysLogAll(false);
    const unsigned outerLine = __LINE__ + 1;
    cleanup.outer = hplNew(NestedHpl, ());
    ATTR_EXPECT(result, cleanup.outer, "outer HPL allocation failed");
    const unsigned constructorLine = __LINE__ + 1;
    cleanup.ordinaryConstructor = hplNew(OrdinaryConstructor, ());
    ATTR_EXPECT(result, cleanup.ordinaryConstructor, "ordinary constructor allocation failed");
    cleanup.sameExpressionOrdinary =
        (cleanup.classSpecific = hplNew(ClassSpecific, ()), new int(2));
    ATTR_EXPECT(result, cleanup.classSpecific && cleanup.sameExpressionOrdinary, "same-expression allocations failed");
    cleanup.classSpecificConstructor = hplNew(ClassSpecificWithOrdinaryConstructor, ());
    ATTR_EXPECT(result, cleanup.classSpecificConstructor && ClassSpecificWithOrdinaryConstructor::escaped, "class-specific constructor allocation failed");
    const unsigned arrayLine = __LINE__ + 1;
    cleanup.array = hplNewArray(int, (cleanup.arraySideEffect = new int(3), 3));
    ATTR_EXPECT(result, cleanup.array && cleanup.arraySideEffect, "array attribution setup failed");
    unsigned retryLine = 0;
    {
        struct HandlerScope {
            std::new_handler previous;
            explicit HandlerScope(std::new_handler handler) : previous(std::set_new_handler(handler)) {}
            ~HandlerScope() { std::set_new_handler(previous); }
        } handlerScope(retryingHandler);
        mmgrTestFailNext(MMGR_TEST_FAIL_RAW);
        retryLine = __LINE__ + 1;
        cleanup.retried = hplNew(int, (8));
        mmgrTestClearFailure();
        ATTR_EXPECT(result, cleanup.retried && handlerAllocation, "new_handler retry setup failed");
    }
    const unsigned bufferOnlyLine = __LINE__ + 1;
    cleanup.bufferOnly = hplMalloc(19);
    ATTR_EXPECT(result, cleanup.bufferOnly, "standalone HPL buffer allocation failed");
    const unsigned bufferLine = __LINE__ + 1;
    cleanup.buffer = hplMalloc(19);
    ATTR_EXPECT(result, cleanup.buffer, "HPL buffer allocation failed");
    const unsigned reallocLine = __LINE__ + 1;
    void *newBuffer = hplRealloc(cleanup.buffer, 29);
    if (newBuffer) cleanup.buffer = newBuffer;
    ATTR_EXPECT(result, newBuffer, "HPL realloc failed");
    mmgrTestFailNext(MMGR_TEST_FAIL_RAW);
    void *unexpectedAllocation = hplMalloc(31);
    if (unexpectedAllocation) hplFree(unexpectedAllocation);
    ATTR_EXPECT(result, unexpectedAllocation == nullptr, "failed HPL malloc unexpectedly succeeded");
    mmgrTestClearFailure();
    mmgrTestFailNext(MMGR_TEST_FAIL_RAW);
    newBuffer = hplRealloc(cleanup.buffer, 37);
    if (newBuffer) cleanup.buffer = newBuffer;
    ATTR_EXPECT(result, newBuffer == nullptr, "failed HPL realloc unexpectedly succeeded");
    mmgrTestClearFailure();
    const std::size_t creations = hpl::cMemoryManager::GetCreationCount();
    hpl::cMemoryManager::SetLogCreation(true);
    const unsigned createdBufferLine = __LINE__ + 1;
    cleanup.createdBuffer = hplMalloc(41);
    const unsigned createdReallocLine = __LINE__ + 1;
    newBuffer = hplRealloc(cleanup.createdBuffer, 43);
    if (newBuffer) cleanup.createdBuffer = newBuffer;
    const unsigned createdObjectLine = __LINE__ + 1;
    cleanup.createdObject = hplNew(int, ());
    const unsigned createdArrayLine = __LINE__ + 1;
    cleanup.createdArray = hplNewArray(int, 2);
    ATTR_EXPECT(result, cleanup.createdBuffer && cleanup.createdObject && cleanup.createdArray, "creation-count allocations failed");
    const std::size_t expectedCreations = creations + 4;
    mmgrTestFailNext(MMGR_TEST_FAIL_RAW);
    unexpectedAllocation = hplMalloc(47);
    if (unexpectedAllocation) hplFree(unexpectedAllocation);
    ATTR_EXPECT(result, unexpectedAllocation == nullptr, "failed creation-count malloc unexpectedly succeeded");
    mmgrTestClearFailure();
    hpl::cMemoryManager::SetLogCreation(false);
    ATTR_EXPECT(result, hpl::cMemoryManager::GetCreationCount() == expectedCreations, "creation count included failed or implicit allocations");
    cleanup.destructorAllocation = hplNew(DestructorAllocation, ());
    ATTR_EXPECT(result, cleanup.destructorAllocation, "destructor attribution setup failed");
    hplDelete(cleanup.destructorAllocation);
    cleanup.destructorAllocation = nullptr;
    bool threw = false;
    try { (void)hplNew(Throwing, ()); } catch (int value) { threw = value == 23; }
    ATTR_EXPECT(result, threw, "throwing HPL constructor did not restore allocation site");
    cleanup.afterThrow = new int(9);
    hpl::cMemoryManager::SetReportPath(reportPath);
    hpl::cMemoryManager::LogResults();
    if (!readRows(reportPath, result)) return;
    requireOwner(result, cleanup.outer, outerLine, "testHplAttribution", __FILE__, __LINE__);
    requireOwner(result, cleanup.outer ? cleanup.outer->inner : nullptr, NestedHpl::nestedLine, "NestedHpl", __FILE__, __LINE__);
    requireOwner(result, cleanup.ordinaryConstructor, constructorLine, "testHplAttribution", __FILE__, __LINE__);
    requireUnknown(result, cleanup.ordinaryConstructor ? cleanup.ordinaryConstructor->inner : nullptr, __FILE__, __LINE__);
    requireUnknown(result, cleanup.sameExpressionOrdinary, __FILE__, __LINE__);
    requireUnknown(result, ClassSpecificWithOrdinaryConstructor::escaped, __FILE__, __LINE__);
    requireOwner(result, cleanup.array, arrayLine, "testHplAttribution", __FILE__, __LINE__);
    requireUnknown(result, cleanup.arraySideEffect, __FILE__, __LINE__);
    requireOwner(result, cleanup.retried, retryLine, "testHplAttribution", __FILE__, __LINE__);
    requireUnknown(result, handlerAllocation, __FILE__, __LINE__);
    requireUnknown(result, cleanup.alwaysLogged, __FILE__, __LINE__);
    requireOwner(result, cleanup.bufferOnly, bufferOnlyLine, "testHplAttribution", __FILE__, __LINE__);
    requireOwner(result, cleanup.buffer, reallocLine, "testHplAttribution", __FILE__, __LINE__);
    requireOwner(result, cleanup.createdBuffer, createdReallocLine, "testHplAttribution", __FILE__, __LINE__);
    requireOwner(result, cleanup.createdObject, createdObjectLine, "testHplAttribution", __FILE__, __LINE__);
    requireOwner(result, cleanup.createdArray, createdArrayLine, "testHplAttribution", __FILE__, __LINE__);
    requireUnknown(result, DestructorAllocation::escaped, __FILE__, __LINE__);
    requireUnknown(result, cleanup.afterThrow, __FILE__, __LINE__);
    (void)bufferLine;
    (void)createdBufferLine;
}
UTEST(HplMemoryManager, Attribution) {
    hpl_memory_test::CaseScope scope(utest_result);
    testHplAttribution(utest_result);
}
