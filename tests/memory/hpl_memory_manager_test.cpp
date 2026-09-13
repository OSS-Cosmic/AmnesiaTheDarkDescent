#include "system/MemoryManager.h"
#include "hpl_memory_manager_fixture.h"
#include "mmgr.h"
#include "mmgr_test.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <string>

extern bool hplTestLogCallbackAllocated();
extern bool hplTestLogContains(const char *);

namespace {
char gRoot[4096] = ".";
char gReport[4096] = "./hpl-memory-report.txt";

void copyPath(char *destination, std::size_t capacity, const char *source) {
    if (!source || !*source) return;
    const std::size_t length = std::strlen(source);
    if (length >= capacity) return;
    std::memcpy(destination, source, length + 1);
}

} // namespace

namespace hpl_memory_test {
const char *reportPath() { return gReport; }
const char *reportRoot() { return gRoot; }

bool check(bool condition, int *utest_result, const char *expression,
           const char *file, int line, const char *message) {
    if (condition) return true;
    if (utest_result) *utest_result = UTEST_TEST_FAILURE;
    std::fprintf(stderr, "%s:%d: HPL_EXPECT(%s): %s\n", file, line,
                 expression, message ? message : "expectation failed");
    return false;
}

CaseScope::CaseScope(int *result) : utest_result(result), baseline_bytes(0), baseline_units(0) {
    previous_new_handler = std::get_new_handler();
    previous_log_creation = hpl::cMemoryManager::GetLogCreation();
    mmgrTestClearFailure();
    hpl::cMemoryManager::SetReportPath(reportPath());
    std::set_new_handler(nullptr);
    hpl::cMemoryManager::SetLogCreation(false);
    const sMStats stats = mmgrGetMemoryStatistics();
    baseline_bytes = stats.totalReportedMemory;
    baseline_units = stats.totalAllocUnitCount;
}

CaseScope::~CaseScope() {
    mmgrTestClearFailure();
    const sMStats stats = mmgrGetMemoryStatistics();
    if (stats.totalReportedMemory != baseline_bytes || stats.totalAllocUnitCount != baseline_units) {
        check(false, utest_result, "live allocation statistics restored", __FILE__, __LINE__,
              "case left live memory");
    }
    hpl::cMemoryManager::SetReportPath(reportPath());
    hpl::cMemoryManager::SetLogCreation(previous_log_creation);
    std::set_new_handler(previous_new_handler);
}
} // namespace hpl_memory_test

namespace {
bool fileContains(const char *path, const char *needle) {
    std::FILE *file = std::fopen(path, "rb"); if (!file) return false;
    char buffer[4096]; std::size_t used = 0; bool found = false;
    while (!found && !std::feof(file)) {
        used += std::fread(buffer + used, 1, sizeof(buffer) - used - 1, file); buffer[used] = '\0';
        found = std::strstr(buffer, needle) != nullptr;
        if (used == sizeof(buffer) - 1) { const std::size_t keep = std::strlen(needle) + 1; std::memmove(buffer, buffer + used - keep, keep); used = keep; }
    }
    std::fclose(file); return found;
}
struct Annotated { int value; Annotated() : value(7) {} };
struct Nested { Annotated *inner; Nested() : inner(hplNew(Annotated, ())) {} ~Nested() { hplDelete(inner); } };
struct Throwing { Throwing() { throw 17; } };
struct Unconsumed {
    static void *operator new(std::size_t size) { return std::malloc(size); }
    static void operator delete(void *p) noexcept { std::free(p); }
};
void *handlerAllocation = nullptr;
void retryingHandler() { handlerAllocation = ::operator new(13); }
void throwingHandler() { throw std::bad_alloc(); }

struct ThreadJoiner {
    std::thread workers[4];
    void joinAll() {
        for (auto &worker : workers) if (worker.joinable()) worker.join();
    }
    ~ThreadJoiner() {
        joinAll();
    }
};

struct BufferOwner {
    void *pointer;
    explicit BufferOwner(void *value = nullptr) : pointer(value) {}
    ~BufferOwner() { hplFree(pointer); }
    void reset(void *value = nullptr) { if (pointer != value) hplFree(pointer); pointer = value; }
    void *release() { void *value = pointer; pointer = nullptr; return value; }
};
template <class T> struct ObjectOwner {
    T *pointer;
    explicit ObjectOwner(T *value = nullptr) : pointer(value) {}
    ~ObjectOwner() { hplDelete(pointer); }
    T *release() { T *value = pointer; pointer = nullptr; return value; }
};
template <class T> struct ArrayOwner {
    T *pointer;
    explicit ArrayOwner(T *value = nullptr) : pointer(value) {}
    ~ArrayOwner() { hplDeleteArray(pointer); }
};
struct NewOwner {
    void *pointer;
    explicit NewOwner(void *value = nullptr) : pointer(value) {}
    ~NewOwner() { ::operator delete(pointer); }
};
struct HandlerAllocationCleanup {
    ~HandlerAllocationCleanup() { ::operator delete(handlerAllocation); handlerAllocation = nullptr; }
};
struct HandlerGuard {
    std::new_handler previous;
    explicit HandlerGuard(std::new_handler handler) : previous(std::set_new_handler(handler)) {}
    ~HandlerGuard() { std::set_new_handler(previous); }
};
void testFacadeAndAnnotations(int *utest_result, const char *report, std::size_t baseline) {
    const sMStats rawBefore = mmgrGetMemoryStatistics();
    const hpl::sMemoryStatistics before = hpl::cMemoryManager::GetMemoryStatistics();
    HPL_EXPECT(before.totalAllocUnitCount == rawBefore.totalAllocUnitCount && before.totalReportedMemory == rawBefore.totalReportedMemory && before.enabled, "facade statistics do not match backend");
    HPL_EXPECT(!hpl::cMemoryManager::IsValid(nullptr), "null was reported valid");
    hpl::cMemoryManager::SetLogCreation(true); const std::size_t creations = hpl::cMemoryManager::GetCreationCount();
    ObjectOwner<Annotated> objectOwner(hplNew(Annotated, ()));
    Annotated *object = objectOwner.pointer;
    HPL_EXPECT(object && object->value == 7 && hpl::cMemoryManager::IsValid(object), "annotated hplNew failed");
    HPL_EXPECT(hpl::cMemoryManager::GetCreationCount() == creations + 1, "creation count did not record one annotated allocation");
    const sMStats one = mmgrGetMemoryStatistics();
    HPL_EXPECT(one.totalAllocUnitCount == rawBefore.totalAllocUnitCount + 1, "annotated allocation unit delta was not controlled");
    HPL_EXPECT(one.totalReportedMemory == rawBefore.totalReportedMemory + sizeof(Annotated), "annotated allocation byte delta was not controlled");
    hpl::cMemoryManager::SetLogCreation(false);
    BufferOwner pOwner(hplMalloc(37)); void *p = pOwner.pointer;
    HPL_EXPECT(p && hpl::cMemoryManager::IsValid(p) && hpl::cMemoryManager::IsValid(static_cast<char *>(p) + 36), "IsValid rejected an allocation boundary");
    if (!p) return;
    HPL_EXPECT(!hpl::cMemoryManager::IsValid(static_cast<char *>(p) + 37), "IsValid accepted the one-past boundary");
    pOwner.reset(); HPL_EXPECT(!hpl::cMemoryManager::IsValid(p), "IsValid retained a freed allocation");
    hpl::cMemoryManager::SetReportPath(report); hpl::cMemoryManager::LogResults();
    HPL_EXPECT(fileContains(report, "hpl_memory_manager_fixture_before_main.cpp") && fileContains(report, "testFacadeAndAnnotations") && fileContains(report, "Allocation unit count:"), "report omitted C-file attribution or summary");
    objectOwner.release(); hplDelete(object);
    HPL_EXPECT(mmgrGetMemoryStatistics().totalAllocUnitCount == baseline, "facade test leaked a live allocation");
}
void testAttributionAndRestoration(int *utest_result, const char *report) {
    ObjectOwner<Nested> nestedOwner(hplNew(Nested, ())); Nested *nested = nestedOwner.pointer;
    int count = 0; ArrayOwner<Annotated> arrayOwner(hplNewArray(Annotated, ++count)); Annotated *array = arrayOwner.pointer;
    HPL_EXPECT(nested && array && count == 1, "nested or single-evaluation array allocation failed");
    Throwing *unexpected = nullptr; bool threw = false;
    try { unexpected = hplNew(Throwing, ()); } catch (int value) { threw = true; HPL_EXPECT(value == 17, "throwing constructor did not propagate"); }
    HPL_EXPECT(threw, "throwing constructor did not throw"); if (unexpected) hplDelete(unexpected);
    ObjectOwner<Unconsumed> unconsumedOwner; Unconsumed *unconsumed = nullptr;
    volatile std::size_t ordinaryCount = 1;
    unconsumedOwner.pointer = hplNew(Unconsumed, ()); unconsumed = unconsumedOwner.pointer;
    ArrayOwner<int> ordinaryOwner((new int[ordinaryCount])); int *ordinary = ordinaryOwner.pointer;
    HPL_EXPECT(ordinary && hpl::cMemoryManager::IsValid(ordinary) && (ordinary[0] = 9) == 9, "ordinary allocation after class-specific allocation failed");
    hpl::cMemoryManager::LogResults();
    HPL_EXPECT(fileContains(report, "testAttributionAndRestoration"), "report omitted nested/array attribution");
    HPL_EXPECT(fileContains(report, "hpl_memory_manager_test.cpp") && fileContains(report, "??"), "ordinary allocation inherited an unrelated annotation");
}
void testReallocFailureAndBoundaries(int *utest_result) {
    BufferOwner owner(hplMalloc(8)); char *p = static_cast<char *>(owner.pointer); HPL_EXPECT(p, "realloc fixture allocation failed");
    if (!p) return; std::memcpy(p, "old-data", 8);
    const sMStats before = mmgrGetMemoryStatistics(); mmgrTestFailNext(MMGR_TEST_FAIL_RAW);
    void *replacement = hplRealloc(p, 64);
    if (replacement) owner.pointer = replacement;
    mmgrTestClearFailure();
    HPL_EXPECT(replacement == nullptr, "injected realloc failure unexpectedly succeeded");
    if (replacement) return;
    HPL_EXPECT(std::memcmp(p, "old-data", 8) == 0 && hpl::cMemoryManager::IsValid(p) && hpl::cMemoryManager::IsValid(p + 7) && !hpl::cMemoryManager::IsValid(p + 8), "failed realloc did not preserve data or validity boundaries");
    const sMStats after = mmgrGetMemoryStatistics(); HPL_EXPECT(after.totalAllocUnitCount == before.totalAllocUnitCount && after.totalReportedMemory == before.totalReportedMemory, "failed realloc changed count or bytes");
    hplFree(nullptr); BufferOwner fromNull(hplRealloc(nullptr, 16)); HPL_EXPECT(fromNull.pointer && hpl::cMemoryManager::IsValid(fromNull.pointer) && !hpl::cMemoryManager::IsValid(nullptr), "null realloc/free behavior was wrong");
}
void testOperatorsAndHandlers(int *utest_result, const char *report) {
    std::unique_ptr<int> one(new int(3)); std::unique_ptr<int[]> many(new int[4]); HPL_EXPECT(one && *one == 3 && many, "global new operators failed");
    struct alignas(64) Aligned { char bytes[64]; }; Aligned *aligned = new Aligned; HPL_EXPECT(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0, "aligned new failed"); delete aligned;
    HandlerAllocationCleanup handlerCleanup; HandlerGuard retryGuard(retryingHandler); mmgrTestFailNext(MMGR_TEST_FAIL_RAW); NewOwner retried(::operator new(sizeof(int))); HPL_EXPECT(retried.pointer && handlerAllocation, "retrying new_handler did not retry"); mmgrTestClearFailure();
    HandlerGuard noHandler(nullptr); mmgrTestFailNext(MMGR_TEST_FAIL_RAW); bool threw = false; NewOwner unexpected;
    try { unexpected.pointer = ::operator new(97); } catch (const std::bad_alloc &) { threw = true; } HPL_EXPECT(threw, "absent new_handler did not throw bad_alloc"); mmgrTestClearFailure();
    HandlerGuard throwingGuard(throwingHandler); mmgrTestFailNext(MMGR_TEST_FAIL_RAW); NewOwner nothrow(::operator new(97, std::nothrow)); HPL_EXPECT(!nothrow.pointer, "throwing nothrow handler did not return null"); mmgrTestClearFailure();
    hpl::cMemoryManager::LogResults(); HPL_EXPECT(fileContains(report, "??"), "handler allocation received unrelated source attribution");
}
void testSingleEvaluationAndConcurrency(int *utest_result) {
    int evaluations = 0; BufferOwner owner(hplMalloc(++evaluations)); HPL_EXPECT(owner.pointer && evaluations == 1, "hplMalloc evaluated its size more than once"); if (!owner.pointer) return; void *replacement = hplRealloc(owner.pointer, ++evaluations + 10); if (replacement) owner.pointer = replacement; HPL_EXPECT(replacement && evaluations == 2, "hplRealloc evaluated arguments incorrectly");
    std::atomic<bool> failed(false); ThreadJoiner threads;
    for (unsigned worker = 0; worker != 4; ++worker) threads.workers[worker] = std::thread([&failed, worker] { for (unsigned i = 0; i != 100; ++i) { BufferOwner owner(hplMalloc(19 + ((worker + i) % 17))); if (!owner.pointer) { failed = true; continue; } void *replacement = hplRealloc(owner.pointer, 61 + i); if (!replacement) { failed = true; continue; } owner.pointer = replacement; } });
    threads.joinAll(); HPL_EXPECT(!failed.load() && mmgrValidateAllAllocUnits(), "concurrent HPL allocation corrupted backend");
}
void testWideReportPathAndRepeatedReports(int *utest_result, const char *root) {
	const std::wstring report = std::wstring(root, root + std::strlen(root)) + L"/wide space-\u00E9-\U0001F642.memreport";
	const std::wstring failed = std::wstring(root, root + std::strlen(root)) + L"/missing \u00E9-\U0001F642/report.memreport";
	const std::string reportUtf8 = std::string(root) + "/wide space-\xC3\xA9-\xF0\x9F\x99\x82.memreport";
	hpl::cMemoryManager::SetReportPath(failed);
	hpl::cMemoryManager::LogResults();
	BufferOwner liveOwner(hplMalloc(211)); void *live = liveOwner.pointer;
    HPL_EXPECT(live && hpl::cMemoryManager::IsValid(live), "wide report fixture allocation failed"); if (!live) return;
	hpl::cMemoryManager::SetReportPath(report);
	hpl::cMemoryManager::LogResults();
    HPL_EXPECT(fileContains(reportUtf8.c_str(), "Actual total memory in use") && fileContains(reportUtf8.c_str(), "Allocation unit count:"), "wide report path or live snapshot was not written");
    HPL_EXPECT(fileContains(reportUtf8.c_str(), "testWideReportPathAndRepeatedReports"), "deliberate outstanding allocation omitted its site");
	hpl::cMemoryManager::LogResults();
	liveOwner.reset();
	hpl::cMemoryManager::LogResults();
    HPL_EXPECT(!fileContains(reportUtf8.c_str(), "testWideReportPathAndRepeatedReports"), "freed allocation remained in the repeated report");
    HPL_EXPECT(hplTestLogCallbackAllocated() && hplTestLogContains("outstanding snapshot") && hplTestLogContains("peak snapshot"), "log callback did not capture the post-report summary");
}
}
UTEST_STATE();

UTEST(HplMemory, Facade) {
    hpl_memory_test::CaseScope scope(utest_result);
    testFacadeAndAnnotations(utest_result, hpl_memory_test::reportPath(), scope.baseline_units);
}
UTEST(HplMemory, Attribution) {
    hpl_memory_test::CaseScope scope(utest_result);
    testAttributionAndRestoration(utest_result, hpl_memory_test::reportPath());
}
UTEST(HplMemory, Realloc) {
    hpl_memory_test::CaseScope scope(utest_result);
    testReallocFailureAndBoundaries(utest_result);
}
UTEST(HplMemory, Operators) {
    hpl_memory_test::CaseScope scope(utest_result);
    testOperatorsAndHandlers(utest_result, hpl_memory_test::reportPath());
}
UTEST(HplMemory, ConcurrencyAndLifetimes) {
    hpl_memory_test::CaseScope scope(utest_result);
    testSingleEvaluationAndConcurrency(utest_result);
}
UTEST(HplMemory, Reports) {
    hpl_memory_test::CaseScope scope(utest_result);
    testWideReportPathAndRepeatedReports(utest_result, hpl_memory_test::reportRoot());
}

int main(int argc, const char *const argv[]) {
    const char *root = std::getenv("HPL_MEMORY_TEST_ROOT");
    if (!root || !*root) root = std::getenv("TMPDIR");
    if (!root || !*root) root = std::getenv("TEMP");
    if (!root || !*root) root = ".";
    copyPath(gRoot, sizeof(gRoot), root);
    const std::size_t rootLength = std::strlen(gRoot);
    if (rootLength + sizeof("/hpl-memory-report.txt") <= sizeof(gReport)) {
        std::memcpy(gReport, gRoot, rootLength);
        std::memcpy(gReport + rootLength, "/hpl-memory-report.txt", sizeof("/hpl-memory-report.txt"));
    }
    char app[] = "HplMemoryManagerTests";
    if (!initMemAlloc(app)) { std::fprintf(stderr, "HplMemoryManagerTests: backend initialization failed\n"); return 1; }
    mmgrSetLogFileDirectory(gRoot);
    const sMStats beforeTests = mmgrGetMemoryStatistics();
    const int result = utest_main(argc, argv);
    const sMStats afterTests = mmgrGetMemoryStatistics();
    if (afterTests.totalReportedMemory != beforeTests.totalReportedMemory ||
        afterTests.totalAllocUnitCount != beforeTests.totalAllocUnitCount) {
        std::fprintf(stderr, "HplMemoryManagerTests: tests changed pre-main live allocation statistics\n");
        return 1;
    }
    return result;
}
