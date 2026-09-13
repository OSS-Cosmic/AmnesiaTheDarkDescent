// Standalone contract tests for Fluid Studios' allocator/platform boundary.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "mmgr.h"
#include "mmgr_test.h"
#include "utest.h"

namespace fs = std::filesystem;
namespace {

std::string readFile(const fs::path &path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// Every allocation made by a test helper is tracked, including unexpected
// successes on failure paths. Each worker has its own independent scope.
struct AllocationScope {
 struct Entry { void *ptr; unsigned int type; };
 std::vector<Entry> live;
 static thread_local AllocationScope *current;
 AllocationScope *previous = current;
 int *utest_result;
 std::atomic<bool> *failed;
 explicit AllocationScope(int *result = nullptr, std::atomic<bool> *status = nullptr)
     : utest_result(result), failed(status) { current = this; }
 void track(void *p, unsigned int type) {
  if (!p) return;
  const unsigned int freeType = type == m_alloc_new ? m_alloc_delete :
      type == m_alloc_new_array ? m_alloc_delete_array : m_alloc_free;
  try { live.push_back({p, freeType}); }
  catch (...) { mmgrDeallocator(__FILE__, __LINE__, __FUNCTION__, freeType, p); throw; }
 }
 void forget(void *p) {
  live.erase(std::remove_if(live.begin(), live.end(), [p](const Entry &e) { return e.ptr == p; }), live.end());
 }
 ~AllocationScope() {
  if (failed && !live.empty()) *failed = true;
  if (utest_result && *utest_result == UTEST_TEST_PASSED) {
   EXPECT_TRUE_MSG(live.empty(), "test left tracked allocations alive before fallback cleanup");
  }
  for (const auto &entry : live)
   if (mmgrValidateAddress(entry.ptr)) mmgrDeallocator(__FILE__, __LINE__, __FUNCTION__, entry.type, entry.ptr);
  current = previous;
 }
};
thread_local AllocationScope *AllocationScope::current = nullptr;
struct FailureGuard {
 FailureGuard() { mmgrTestClearFailure(); }
 ~FailureGuard() { mmgrTestClearFailure(); }
};
struct DamagedGuards {
 unsigned char *bytes; size_t size; unsigned char before, after; bool active = true;
 DamagedGuards(void *p, size_t n) : bytes(static_cast<unsigned char *>(p)), size(n), before(bytes[-1]), after(bytes[n]) {
  bytes[-1] ^= 1; bytes[size] ^= 1;
 }
 void restore() {
  if (active && mmgrValidateAddress(bytes)) { bytes[-1] = before; bytes[size] = after; }
  active = false;
 }
 ~DamagedGuards() { restore(); }
};
struct Threads {
 std::vector<std::thread> threads;
 std::atomic<bool> *start;
 explicit Threads(std::atomic<bool> *gate = nullptr) : start(gate) {}
 void join() {
  if (start) start->store(true, std::memory_order_release);
  for (auto &thread : threads) if (thread.joinable()) thread.join();
 }
 ~Threads() { join(); }
};

#define FLUID_EXPECT(condition, message) ASSERT_TRUE_MSG((condition), (message))
#define FLUID_EXPECT_STATS(a, b, message) do { \
	const sMStats fluid_actual_stats = (b); \
	ASSERT_TRUE_MSG(std::memcmp(&(a), &fluid_actual_stats, sizeof(a)) == 0, (message)); \
} while (false)
// These aliases deliberately expand at the expectation site to utest's real
// macros, so an assertion failure is fatal for the current helper and cannot
// be lost in a thread-local side channel.
#define require FLUID_EXPECT
#define requireStatsEqual FLUID_EXPECT_STATS

void *allocate(size_t alignment, size_t size)
{
	void *p = mmgrAllocator(__FILE__, __LINE__, __FUNCTION__, m_alloc_malloc, alignment, size);
	AllocationScope::current->track(p, m_alloc_malloc); return p;
}

void *allocateType(unsigned int type, size_t alignment, size_t size)
{
	void *p = mmgrAllocator(__FILE__, __LINE__, __FUNCTION__, type, alignment, size);
	AllocationScope::current->track(p, type); return p;
}

void *allocateOwned(const char *owner, size_t size)
{
	void *p = mmgrAllocator(__FILE__, __LINE__, owner, m_alloc_malloc, sizeof(void *), size);
	AllocationScope::current->track(p, m_alloc_malloc); return p;
}

void release(void *p, unsigned int type = m_alloc_free)
{
	mmgrDeallocator(__FILE__, __LINE__, __FUNCTION__, type, p);
	if (p && !mmgrValidateAddress(p)) AllocationScope::current->forget(p);
}

void *reallocate(void *p, size_t size, unsigned int type = m_alloc_realloc)
{
	void *q = mmgrReallocator(__FILE__, __LINE__, __FUNCTION__, type, size, p);
	if (p && (q || (size == 0 && !mmgrValidateAddress(p)))) AllocationScope::current->forget(p);
	AllocationScope::current->track(q, m_alloc_malloc); return q;
}

void fill(void *p, size_t size, unsigned char seed)
{
	for (size_t i = 0; i < size; ++i) static_cast<unsigned char *>(p)[i] = static_cast<unsigned char>(seed + i * 13u);
}

bool checkFill(const void *p, size_t size, unsigned char seed, const char *)
{
	for (size_t i = 0; i < size; ++i) {
		if (static_cast<const unsigned char *>(p)[i] != static_cast<unsigned char>(seed + i * 13u)) {
			return false;
		}
	}
	return true;
}

bool testFirstUseConcurrency(int *utest_result)
{
	std::atomic<bool> failed(false);
	Threads group;
	for (int worker = 0; worker != 4; ++worker) group.threads.emplace_back([&, worker] {
		AllocationScope allocations(nullptr, &failed);
		try {
		for (int i = 0; i != 64; ++i) {
			const size_t size = 17 + (i % 23);
			void *p = allocate(alignof(std::max_align_t), size);
			if (!p) { failed = true; continue; }
			fill(p, size, static_cast<unsigned char>(worker + i));
			if (!checkFill(p, size, static_cast<unsigned char>(worker + i), "first-use payload")) failed = true;
			release(p);
		}
		} catch (...) { failed = true; }
	});
	group.join();
	if (failed.load()) {
		*utest_result = UTEST_TEST_FAILURE;
		std::fprintf(stderr, "FluidStudiosMemoryTests: first-use concurrent allocation failed before initMemAlloc\n");
		return false;
	}
	return true;
}

void testFailureStages(const fs::path &root, int *utest_result)
{
	require(mmgrTestAccounting(), "synthetic accounting did not cover values beyond 4 GiB");
	const sMStats beforeRaw = mmgrGetMemoryStatistics();
	mmgrTestFailNext(MMGR_TEST_FAIL_RAW);
	require(allocate(16, 31) == nullptr, "raw allocation failure was not returned");
	requireStatsEqual(beforeRaw, mmgrGetMemoryStatistics(), "raw failure changed statistics");
	mmgrTestClearFailure();
	// Exercise the raw-failure seam after the metadata reservoir has really
	// been exhausted; this must not turn a backend allocation failure into a
	// tracking/statistics mutation.
	{
		std::vector<void *> held;
		while (mmgrTestReservoirAvailable() != 0) {
			void *p = allocate(8, 1); require(p != nullptr, "could not drain metadata reservoir"); held.push_back(p);
		}
		const sMStats before = mmgrGetMemoryStatistics();
		mmgrTestFailNext(MMGR_TEST_FAIL_RAW);
		require(allocate(8, 1) == nullptr, "raw failure with empty reservoir was not returned");
		requireStatsEqual(before, mmgrGetMemoryStatistics(), "raw failure with empty reservoir changed statistics");
		mmgrTestClearFailure();
		for (void *p : held) release(p);
	}
	for (int stage : {MMGR_TEST_FAIL_RESERVOIR, MMGR_TEST_FAIL_POOL_LIST}) {
		const size_t available = mmgrTestReservoirAvailable();
		std::vector<void *> held;
		for (size_t i = 0; i < available; ++i) {
			void *p = allocate(8, 1); require(p != nullptr, "could not exhaust metadata reservoir"); held.push_back(p);
		}
		const sMStats before = mmgrGetMemoryStatistics();
		mmgrTestFailNext(stage);
		require(allocate(8, 1) == nullptr, "metadata growth failure was not returned");
		requireStatsEqual(before, mmgrGetMemoryStatistics(), "metadata failure changed statistics");
		mmgrTestClearFailure();
		for (void *p : held) release(p);
	}
	const sMStats beforeReport = mmgrGetMemoryStatistics();
	mmgrTestFailNext(MMGR_TEST_FAIL_REPORT);
	mmgrDumpMemoryReport((root / "failed-report.txt").string().c_str(), true);
	requireStatsEqual(beforeReport, mmgrGetMemoryStatistics(), "failed report changed statistics");
	require(!fs::exists(root / "failed-report.txt"), "injected report failure wrote an artifact");
	mmgrTestClearFailure();
}

void testAllocationAndAlignment(int *utest_result)
{
	const size_t oldSize = 257;
	const sMStats beforeInitial = mmgrGetMemoryStatistics();
	void *p = allocate(128, oldSize);
	require(p && reinterpret_cast<std::uintptr_t>(p) % 128 == 0, "requested alignment was lost");
	fill(p, oldSize, 0x31);
	const sMStats afterInitial = mmgrGetMemoryStatistics();
	require(afterInitial.totalReportedMemory == oldSize && afterInitial.totalAllocUnitCount == beforeInitial.totalAllocUnitCount + 1 &&
	        afterInitial.accumulatedReportedMemory == beforeInitial.accumulatedReportedMemory + oldSize &&
	        afterInitial.accumulatedActualMemory == beforeInitial.accumulatedActualMemory + oldSize + 32 + 128 &&
	        afterInitial.accumulatedAllocUnitCount == beforeInitial.accumulatedAllocUnitCount + 1,
	        "successful allocation did not update every cumulative statistic exactly");
	require(mmgrValidateAddress(p) && !mmgrValidateAddress(static_cast<unsigned char *>(p) + 1), "address validation is wrong");
	require(mmgrContainsAddress(p) && mmgrContainsAddress(static_cast<unsigned char *>(p) + oldSize - 1) &&
	        !mmgrContainsAddress(static_cast<unsigned char *>(p) + oldSize) && !mmgrContainsAddress(nullptr),
	        "address containment query is wrong");
	p = reallocate(p, 513);
	require(p && reinterpret_cast<std::uintptr_t>(p) % 128 == 0, "realloc growth lost alignment");
	require(checkFill(p, oldSize, 0x31, "realloc growth did not preserve all contents"), "realloc growth did not preserve all contents");
	const sMStats afterGrowth = mmgrGetMemoryStatistics();
	require(afterGrowth.totalReportedMemory == 513 && afterGrowth.totalAllocUnitCount == beforeInitial.totalAllocUnitCount + 1 &&
	        afterGrowth.accumulatedReportedMemory - afterInitial.accumulatedReportedMemory == 513 - oldSize &&
	        afterGrowth.accumulatedActualMemory > afterInitial.accumulatedActualMemory &&
	        afterGrowth.accumulatedAllocUnitCount == beforeInitial.accumulatedAllocUnitCount + 1 && mmgrValidateAllAllocUnits(),
	        "realloc growth totals/guards are wrong");
	const sMStats beforeShrink = afterGrowth;
	fill(p, 513, 0x42);
	p = reallocate(p, 19);
	require(p && reinterpret_cast<std::uintptr_t>(p) % 128 == 0, "realloc shrink lost alignment");
	require(checkFill(p, 19, 0x42, "realloc shrink did not preserve retained contents"), "realloc shrink did not preserve retained contents");
	const sMStats afterShrink = mmgrGetMemoryStatistics();
	require(afterShrink.totalReportedMemory == 19 && afterShrink.accumulatedReportedMemory == beforeShrink.accumulatedReportedMemory &&
	        afterShrink.accumulatedActualMemory == beforeShrink.accumulatedActualMemory &&
	        afterShrink.accumulatedAllocUnitCount == beforeShrink.accumulatedAllocUnitCount && mmgrValidateAllAllocUnits(),
	        "realloc shrink incorrectly reduced cumulative statistics");
	release(p);
	require(!mmgrContainsAddress(p), "freed address remained contained");

	for (size_t alignment : {alignof(std::max_align_t), size_t(64), size_t(4096)}) {
		void *q = allocate(alignment, 23);
		require(q && reinterpret_cast<std::uintptr_t>(q) % alignment == 0, "ordinary/extended alignment was not honored"); release(q);
	}
	for (size_t alignment : {size_t(1), size_t(3), size_t(24), size_t(4095)}) {
		const sMStats before = mmgrGetMemoryStatistics();
		require(allocate(alignment, 23) == nullptr, "invalid alignment was accepted");
		requireStatsEqual(before, mmgrGetMemoryStatistics(), "invalid alignment changed statistics");
	}
	const sMStats beforeOverflow = mmgrGetMemoryStatistics();
	require(allocate(alignof(std::max_align_t), std::numeric_limits<size_t>::max()) == nullptr, "size overflow was accepted");
	requireStatsEqual(beforeOverflow, mmgrGetMemoryStatistics(), "size overflow changed statistics");

}

void testFamiliesAndInvalidOperations(int *utest_result)
{
	void *zeroed = allocateType(m_alloc_calloc, sizeof(void *), 37);
	require(zeroed != nullptr, "calloc setup failed");
	for (size_t i = 0; i != 37; ++i) require(static_cast<unsigned char *>(zeroed)[i] == 0, "calloc was not zero-filled");
	release(zeroed);
	for (unsigned int type : {m_alloc_new, m_alloc_new_array}) {
		void *q = allocateType(type, sizeof(void *), 21); require(q != nullptr, "new-family setup failed");
		const sMStats before = mmgrGetMemoryStatistics();
		release(q, type == m_alloc_new ? m_alloc_delete_array : m_alloc_delete);
		require(mmgrValidateAddress(q), "mismatched new-family free removed ownership");
		requireStatsEqual(before, mmgrGetMemoryStatistics(), "mismatched new-family free changed statistics");
		void *old = reallocate(q, 29);
		require(old == nullptr && mmgrValidateAddress(q), "realloc accepted a new-family allocation");
		requireStatsEqual(before, mmgrGetMemoryStatistics(), "realloc of new-family allocation changed statistics");
		release(q, type == m_alloc_new ? m_alloc_delete : m_alloc_delete_array);
	}
	const sMStats beforeBadType = mmgrGetMemoryStatistics();
	require(allocateType(m_alloc_unknown, sizeof(void *), 4) == nullptr &&
	        reallocate(nullptr, 4, m_alloc_malloc) == nullptr, "out-of-range allocation types were accepted");
	requireStatsEqual(beforeBadType, mmgrGetMemoryStatistics(), "out-of-range operation changed statistics");

	void *bad = allocate(8, 7); require(bad != nullptr, "invalid-operation setup failed");
	const sMStats beforeInvalid = mmgrGetMemoryStatistics();
	release(bad, m_alloc_delete); require(mmgrValidateAddress(bad), "mismatched free removed live allocation");
	requireStatsEqual(beforeInvalid, mmgrGetMemoryStatistics(), "mismatched free changed statistics");
	require(reallocate(static_cast<unsigned char *>(bad) + 1, 99) == nullptr && reallocate(reinterpret_cast<void *>(0x12345), 99) == nullptr,
	        "interior/unknown realloc was accepted");
	requireStatsEqual(beforeInvalid, mmgrGetMemoryStatistics(), "invalid realloc changed statistics");
	const sMStats beforeInvalidFree = mmgrGetMemoryStatistics();
	release(reinterpret_cast<void *>(0x12345)); release(nullptr);
	requireStatsEqual(beforeInvalidFree, mmgrGetMemoryStatistics(), "invalid frees changed statistics");
	release(bad);
	const sMStats afterValidFree = mmgrGetMemoryStatistics();
	release(bad);
	requireStatsEqual(afterValidFree, mmgrGetMemoryStatistics(), "double free changed statistics");

}

void testZeroSize(int *utest_result)
{
	void *zero = allocate(32, 0);
	require(zero && mmgrValidateAddress(zero) && !mmgrContainsAddress(zero) &&
	        !mmgrContainsAddress(reinterpret_cast<void *>(0x12345)) && mmgrGetMemoryStatistics().totalReportedMemory == 0,
	        "zero allocation contract failed");
	require(reallocate(zero, 0) == nullptr && !mmgrValidateAddress(zero), "realloc(p,0) was not null and untracked");
	void *zeroFree = allocate(32, 0); require(zeroFree != nullptr, "second zero allocation failed"); release(zeroFree);
	const sMStats beforeNullZero = mmgrGetMemoryStatistics();
	void *nullZero = reallocate(nullptr, 0);
	require(nullZero != nullptr && mmgrValidateAddress(nullZero), "realloc(NULL,0) did not produce a tracked zero allocation");
	release(nullZero);
	const sMStats afterNullZero = mmgrGetMemoryStatistics();
	require(afterNullZero.totalReportedMemory == beforeNullZero.totalReportedMemory &&
	        afterNullZero.totalActualMemory == beforeNullZero.totalActualMemory &&
	        afterNullZero.totalAllocUnitCount == beforeNullZero.totalAllocUnitCount, "realloc(NULL,0) left live accounting");
	void *fromNull = reallocate(nullptr, 37);
	require(fromNull && mmgrValidateAddress(fromNull), "realloc(NULL,n) did not allocate"); release(fromNull);

}

void testReallocationRollback(int *utest_result)
{
	void *rollback = allocate(16, 1021); require(rollback != nullptr, "rollback setup failed"); fill(rollback, 1021, 0x77);
	const sMStats beforeRollback = mmgrGetMemoryStatistics();
	mmgrTestFailNext(MMGR_TEST_FAIL_RAW);
	require(reallocate(rollback, 8192) == nullptr, "injected realloc failure succeeded");
	requireStatsEqual(beforeRollback, mmgrGetMemoryStatistics(), "failed realloc changed statistics");
	require(mmgrValidateAddress(rollback), "failed realloc lost ownership"); require(checkFill(rollback, 1021, 0x77, "failed realloc lost payload"), "failed realloc lost payload");
	mmgrTestClearFailure(); release(rollback);
	void *shrinkFailure = allocate(16, 101); require(shrinkFailure != nullptr, "shrink rollback setup failed"); fill(shrinkFailure, 101, 0x27);
	const sMStats beforeShrinkFailure = mmgrGetMemoryStatistics();
	mmgrTestFailNext(MMGR_TEST_FAIL_RAW);
	require(reallocate(shrinkFailure, 3) == nullptr, "injected realloc shrink failure succeeded");
	requireStatsEqual(beforeShrinkFailure, mmgrGetMemoryStatistics(), "failed realloc shrink changed statistics");
	require(checkFill(shrinkFailure, 101, 0x27, "failed realloc shrink lost payload"), "failed realloc shrink lost payload"); mmgrTestClearFailure(); release(shrinkFailure);
	void *overflowRealloc = allocate(16, 33); require(overflowRealloc != nullptr, "overflow realloc setup failed"); fill(overflowRealloc, 33, 0x55);
	const sMStats beforeReallocOverflow = mmgrGetMemoryStatistics();
	require(reallocate(overflowRealloc, std::numeric_limits<size_t>::max()) == nullptr, "realloc size overflow was accepted");
	requireStatsEqual(beforeReallocOverflow, mmgrGetMemoryStatistics(), "realloc overflow changed statistics");
	require(checkFill(overflowRealloc, 33, 0x55, "realloc overflow lost payload"), "realloc overflow lost payload"); release(overflowRealloc);


}

void testHashChains(int *utest_result)
{
	std::vector<void *> chain, rejects;
	chain.push_back(allocate(8, 2)); require(chain[0] != nullptr, "hash-chain setup failed");
	for (size_t tries = 0; chain.size() != 3 && tries != 200000; ++tries) {
		void *q = allocate(8, 2); require(q != nullptr, "hash-chain candidate failed");
		if (mmgrTestHash(q) == mmgrTestHash(chain.front())) chain.push_back(q); else rejects.push_back(q);
	}
	require(chain.size() == 3, "could not form hash chain"); for (void *q : rejects) release(q);
	void *oldMiddle = chain[1];
	chain[1] = reallocate(chain[1], 23);
	require(chain[1] != nullptr && mmgrValidateAddress(chain[1]) && !mmgrValidateAddress(oldMiddle), "middle hash-chain realloc was not transactional");
	release(chain[1]); require(mmgrValidateAllAllocUnits(), "middle chain realloc/removal failed");
	chain[1] = nullptr;
	void *oldHead = chain[2];
	chain[2] = reallocate(chain[2], 31);
	require(chain[2] != nullptr && mmgrValidateAddress(chain[2]) && !mmgrValidateAddress(oldHead), "head hash-chain realloc was not transactional");
	release(chain[2]); require(mmgrValidateAllAllocUnits(), "head chain realloc/removal failed");
	release(chain[0]); require(mmgrValidateAllAllocUnits(), "tail chain removal failed");

	void *moved = allocate(16, 4096); require(moved != nullptr, "moved realloc setup failed"); fill(moved, 4096, 0x63);
	void *oldMoved = moved; moved = reallocate(moved, 65537);
	require(moved != nullptr && mmgrValidateAddress(moved) && !mmgrValidateAddress(oldMoved), "moved realloc retained old hash entry");
	require(checkFill(moved, 4096, 0x63, "moved realloc lost payload"), "moved realloc lost payload"); release(moved);
}

void testUnusedBytes(int *utest_result)
{
	for (size_t size : {size_t(1), size_t(3), size_t(5), size_t(19)}) {
		void *p = allocate(alignof(std::max_align_t), size); require(p != nullptr, "unused-byte setup failed");
		require(mmgrCalcAllUnused() == size, "unused-byte query did not count an untouched odd payload");
		static_cast<unsigned char *>(p)[size / 2] = 0xA5;
		require(mmgrCalcAllUnused() == size - 1, "unused-byte query counted a used payload byte");
		release(p);
		require(mmgrCalcAllUnused() == 0, "unused-byte query was not zero after quiescent free");
	}
}

void testGuardRejection(int *utest_result)
{
	for (size_t size : {size_t(1), size_t(3), size_t(5), size_t(17)}) {
		void *p = allocate(16, size); require(p != nullptr, "guard setup failed"); fill(p, size, 0x19);
		DamagedGuards guard(p, size);
		const sMStats damaged = mmgrGetMemoryStatistics();
		require(!mmgrValidateAllocUnit(nullptr) && reallocate(p, size + 1) == nullptr, "guard-damaged realloc was accepted");
		release(p);
		require(mmgrValidateAddress(p), "guard-damaged free was not rejected");
		requireStatsEqual(damaged, mmgrGetMemoryStatistics(), "guard-damaged operation changed statistics");
		guard.restore(); release(p);
		require(!mmgrValidateAddress(p), "repaired guard was not freeable");
	}
}

void testConcurrentUse(const fs::path &root, int *utest_result)
{
	constexpr int workerCount = 4;
	constexpr int iterations = 160;
	std::atomic<int> ready(0);
	std::atomic<bool> start(false), failed(false);
	Threads group(&start);
	for (int worker = 0; worker != workerCount; ++worker) group.threads.emplace_back([&, worker] {
		AllocationScope allocations(nullptr, &failed);
		ready.fetch_add(1);
		while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
		try {
		for (int i = 0; i != iterations; ++i) {
			const size_t size = 17 + ((worker * 31 + i) % 47);
			void *p = allocate(sizeof(void *), size);
			if (!p) { failed = true; continue; }
			fill(p, size, static_cast<unsigned char>(worker * 19 + i));
			const size_t grownSize = size + 23;
			void *q = reallocate(p, grownSize);
			if (!q) { failed = true; release(p); continue; }
			if (!checkFill(q, size, static_cast<unsigned char>(worker * 19 + i), "concurrent realloc lost payload")) failed = true;
			fill(q, grownSize, static_cast<unsigned char>(worker * 19 + i + 1));
			if ((i & 1) == 0) {
				q = reallocate(q, 9 + (i % 11));
				if (!q) { failed = true; continue; }
				if (!checkFill(q, 9 + (i % 11), static_cast<unsigned char>(worker * 19 + i + 1), "concurrent shrink lost payload")) failed = true;
			}
			release(q);
		}
		} catch (...) { failed = true; }
	});
	group.threads.emplace_back([&] {
		ready.fetch_add(1);
		while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
		try {
		for (int report = 0; report < 32; ++report) {
			(void)mmgrGetMemoryStatistics();
			if (!mmgrValidateAllAllocUnits()) failed = true;
			mmgrDumpMemoryReport((root / "concurrent-report.txt").string().c_str(), true);
		}
		} catch (...) { failed = true; }
	});
	while (ready.load(std::memory_order_acquire) != workerCount + 1) std::this_thread::yield();
	start.store(true, std::memory_order_release);
	group.join();
	const sMStats after = mmgrGetMemoryStatistics();
	require(!failed.load() && after.totalAllocUnitCount == 0 && after.totalReportedMemory == 0 && mmgrValidateAllAllocUnits(),
	        "concurrent operations failed, corrupted tracking, or leaked live allocations");
	require(readFile(root / "concurrent-report.txt").find("Allocation unit count:") != std::string::npos, "concurrent report could not be read");
}

void testReports(const fs::path &root, int *utest_result)
{
	void *aligned = allocate(64, 257); require(aligned != nullptr, "report setup allocation failed");
	void *live = allocateOwned("gVkAllocation %s%n", 113); require(live != nullptr, "live report allocation failed");
	const sMStats before = mmgrGetMemoryStatistics();
	mmgrDumpMemoryReport("live-report.txt", true);
	const std::string contents = readFile(root / "live-report.txt");
	require(contents.find("gVkAllocation %s%n") != std::string::npos && contents.find("Allocation unit count:") != std::string::npos && contents.find("Reported to application:") != std::string::npos, "live report omitted owner or summary");
	requireStatsEqual(before, mmgrGetMemoryStatistics(), "report changed statistics");
	mmgrDumpMemoryReport("live-report.txt", false); requireStatsEqual(before, mmgrGetMemoryStatistics(), "repeated report changed statistics");
	exitMemAlloc();
	// The fixture mutates its name and directory buffers after configuration.
	const fs::path exitReport = root / "FluidStudiosMemoryTests 100% ready.memleaks";
	requireStatsEqual(before, mmgrGetMemoryStatistics(), "exit report changed statistics");
	require(fs::exists(exitReport) && readFile(exitReport).find("gVkAllocation %s%n") != std::string::npos, "exit report did not preserve live state");
	release(aligned); release(live); mmgrDumpMemoryReport("clean-report.txt", true);
	const std::string clean = readFile(root / "clean-report.txt");
	require(clean.find("Allocation unit count:          0") != std::string::npos && clean.find("Reported to application:          0 bytes") != std::string::npos && clean.find("Actual total memory in use:          0 bytes") != std::string::npos, "clean report was not zero");
	const fs::path missing = root / "missing" / "subdirectory";
	mmgrSetLogFileDirectory(missing.string().c_str());
	mmgrDumpMemoryReport("unwritable-report.txt", true);
	void *afterFailure = allocate(16, 29); require(afterFailure != nullptr, "allocation after failed report failed"); release(afterFailure);
	mmgrSetLogFileDirectory(root.string().c_str());
	mmgrDumpMemoryReport("recovered-report.txt", true);
	require(fs::exists(root / "recovered-report.txt"), "reporting did not recover after bad path");
}

} // namespace

char gExecutable[4096] = {};
std::atomic<unsigned> gFixtureNumber(0);

struct FluidStudios { char root[4096]; };

UTEST_STATE();

UTEST_F_SETUP(FluidStudios)
{
 utest_fixture->root[0] = '\0';
 std::error_code error;
 const char *base = std::getenv("FLUID_STUDIOS_TEST_ROOT");
 fs::path basePath = base && *base ? fs::path(base) : fs::temp_directory_path(error);
 ASSERT_FALSE_MSG(static_cast<bool>(error), "could not find temporary directory");
 basePath = fs::absolute(basePath, error);
 ASSERT_FALSE_MSG(static_cast<bool>(error), "could not resolve fixture root");
 const unsigned number = gFixtureNumber.fetch_add(1);
 const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
 const unsigned long process = static_cast<unsigned long>(_getpid());
#else
 const unsigned long process = static_cast<unsigned long>(getpid());
#endif
 const std::string root = (basePath / ("fluid-studios-memory-" + std::to_string(process) + "-" +
     std::to_string(stamp) + "-" + std::to_string(number))).string();
 ASSERT_LT(root.size(), sizeof(utest_fixture->root));
 const bool created = fs::create_directories(root, error);
 if (error || !created) {
  // A failed recursive creation may have created part of this test's root.
  std::error_code ignored;
  if (error) fs::remove_all(root, ignored);
  ASSERT_TRUE_MSG(false, "could not create isolated fixture directory");
 }
 std::strcpy(utest_fixture->root, root.c_str());
 char appName[] = "FluidStudiosMemoryTests 100% ready";
 char copiedDirectory[4096];
 std::strcpy(copiedDirectory, utest_fixture->root);
 mmgrSetLogFileDirectory(copiedDirectory);
 const bool initialized = initMemAlloc(appName);
 std::strcpy(appName, "mutated-app-name");
 std::strcpy(copiedDirectory, "mutated-directory");
 if (!initialized) {
  fs::remove_all(utest_fixture->root, error);
  utest_fixture->root[0] = '\0';
 }
 ASSERT_TRUE_MSG(initialized, "allocator initialization failed");
}

UTEST_F_TEARDOWN(FluidStudios)
{
 mmgrTestClearFailure();
 const sMStats stats = mmgrGetMemoryStatistics();
 EXPECT_EQ(stats.totalAllocUnitCount, size_t(0));
 EXPECT_EQ(stats.totalReportedMemory, size_t(0));
 EXPECT_EQ(stats.totalActualMemory, size_t(0));
 EXPECT_TRUE(mmgrValidateAllAllocUnits());
 std::error_code error;
 if (utest_fixture->root[0]) fs::remove_all(utest_fixture->root, error);
 EXPECT_FALSE_MSG(static_cast<bool>(error), "could not remove fixture report artifacts");
}

// Catch within the fixture body so utest still calls teardown after an
// exception (its outer exception handler otherwise skips fixture teardown).
template <typename Contract>
void runContract(int *utest_result, Contract contract)
{
 AllocationScope allocations(utest_result);
 FailureGuard failure;
 try { contract(); }
 catch (const std::exception &error) { ASSERT_TRUE_MSG(false, error.what()); }
 catch (...) { ASSERT_TRUE_MSG(false, "unexpected exception in allocator contract"); }
}

UTEST_F(FluidStudios, FailureInjection)
{
 runContract(utest_result, [&] { testFailureStages(utest_fixture->root, utest_result); });
}
UTEST_F(FluidStudios, AllocationAndAlignment)
{
 runContract(utest_result, [&] { testAllocationAndAlignment(utest_result); });
}
UTEST_F(FluidStudios, FamiliesAndInvalidOperations)
{
 runContract(utest_result, [&] { testFamiliesAndInvalidOperations(utest_result); });
}
UTEST_F(FluidStudios, ZeroSize)
{
 runContract(utest_result, [&] { testZeroSize(utest_result); });
}
UTEST_F(FluidStudios, ReallocationRollback)
{
 runContract(utest_result, [&] { testReallocationRollback(utest_result); });
}
UTEST_F(FluidStudios, HashChains)
{
 runContract(utest_result, [&] { testHashChains(utest_result); });
}
UTEST_F(FluidStudios, DamagedGuards)
{
 runContract(utest_result, [&] { testGuardRejection(utest_result); });
}
UTEST_F(FluidStudios, UnusedBytes)
{
 runContract(utest_result, [&] { testUnusedBytes(utest_result); });
}
UTEST_F(FluidStudios, ConcurrentOperations)
{
 runContract(utest_result, [&] { testConcurrentUse(utest_fixture->root, utest_result); });
}
UTEST_F(FluidStudios, ReportsAndCopiedConfiguration)
{
 runContract(utest_result, [&] { testReports(utest_fixture->root, utest_result); });
}
UTEST_F(FluidStudios, FirstUseChildProcess) {
	const char *args[] = {gExecutable, "--fluid-first-use-child", utest_fixture->root, nullptr};
#if defined(_WIN32)
	EXPECT_EQ(_spawnvp(_P_WAIT, gExecutable, args), 0);
#else
	const pid_t child = fork();
	ASSERT_GE(child, 0);
	if (child == 0) { execvp(gExecutable, const_cast<char *const *>(args)); _exit(127); }
	int status = 0;
	pid_t waited;
	do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
	ASSERT_EQ(waited, child);
	ASSERT_TRUE_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 0, "first-use child failed");
#endif
}

int main(int argc, const char *const argv[])
{
 if (argc > 0 && std::strlen(argv[0]) >= sizeof(gExecutable)) return 1;
 if (argc > 0) std::strcpy(gExecutable, argv[0]);
 if (argc > 1 && std::strcmp(argv[1], "--fluid-first-use-child") == 0) {
  if (argc != 3) return 1;
  int result = UTEST_TEST_PASSED;
  // This fresh executable has never called initMemAlloc or a report setter.
  try {
   if (!testFirstUseConcurrency(&result)) return 1;
  } catch (...) {
   std::fprintf(stderr, "FluidStudios.FirstUseChildProcess: thread construction failed\n");
   return 1;
  }
  char childName[] = "FluidStudiosMemoryTests 100% ready";
  if (!initMemAlloc(childName)) return 1;
  char childDirectory[4096];
  if (std::strlen(argv[2]) >= sizeof(childDirectory)) return 1;
  std::strcpy(childDirectory, argv[2]);
  mmgrSetLogFileDirectory(childDirectory);
  std::strcpy(childName, "mutated-app-name");
  std::strcpy(childDirectory, "mutated-directory");
  const sMStats clean = mmgrGetMemoryStatistics();
  exitMemAlloc();
  const std::string report = readFile(fs::path(argv[2]) / "FluidStudiosMemoryTests 100% ready.memleaks");
  const bool ok = clean.totalAllocUnitCount == 0 && clean.totalReportedMemory == 0 &&
      clean.totalActualMemory == 0 && mmgrValidateAllAllocUnits() &&
      report.find("Congratulations! No memory leaks found!") != std::string::npos;
  if (!ok) std::fprintf(stderr, "FluidStudios.FirstUseChildProcess: clean totals or copied report configuration failed\n");
  return ok ? 0 : 1;
 }
 return utest_main(argc, argv);
}
