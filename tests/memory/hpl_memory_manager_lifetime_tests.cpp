#include "system/MemoryManager.h"
#include "hpl_memory_manager_fixture.h"
#include "utest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <vector>

namespace {

struct Stats {
    std::size_t bytes;
    std::size_t units;
};

Stats stats() {
    const hpl::sMemoryStatistics s = hpl::cMemoryManager::GetStatistics();
    return {s.totalReportedMemory, s.totalAllocUnitCount};
}

#define expect(result, condition, message) \
    (::hpl_memory_test::check((condition), (result), #condition, __FILE__, __LINE__, (message)))
#define expectSame(result, actual, expected, message) \
    (::hpl_memory_test::check((actual).bytes == (expected).bytes && \
                              (actual).units == (expected).units, (result), \
                              #actual " has the expected statistics as " #expected, \
                              __FILE__, __LINE__, (message)))

template <typename T> struct ScalarCleanup {
    T *object;
    ~ScalarCleanup() { if (object) hplDelete(object); }
    void release() { object = nullptr; }
};

template <typename T> struct ArrayCleanup {
    T *objects;
    ~ArrayCleanup() { if (objects) hplDeleteArray(objects); }
    void release() { objects = nullptr; }
};

struct Plain {
    explicit Plain(int v = 0) : value(v) {}
    int value;
};

struct Counted {
    static int live;
    static int destroyed;
    Counted() { ++live; }
    ~Counted() { --live; ++destroyed; }
};
int Counted::live = 0;
int Counted::destroyed = 0;

struct Ordered {
    static int next;
    static int destroyed;
    static int order[4];
    int id;
    Ordered() : id(next++) {}
    ~Ordered() { order[destroyed++] = id; }
};
int Ordered::next = 0;
int Ordered::destroyed = 0;
int Ordered::order[4] = {};

struct Throwing {
    static int attempts;
    explicit Throwing(int) { ++attempts; throw 19; }
};
int Throwing::attempts = 0;

struct ThrowingArray {
    static int constructed;
    static int destroyed;
    static int throwAt;
    ThrowingArray() {
        const int index = constructed++;
        if (index == throwAt) throw 23;
    }
    ~ThrowingArray() { ++destroyed; }
};
int ThrowingArray::constructed = 0;
int ThrowingArray::destroyed = 0;
int ThrowingArray::throwAt = 3;

struct alignas(64) Aligned {
    static int live;
    Aligned() { ++live; }
    ~Aligned() { --live; }
    char bytes[64];
};
int Aligned::live = 0;

struct alignas(128) AlignedThrowing {
    AlignedThrowing() { throw 29; }
    char bytes[128];
};

struct PoolOwned {
    static int allocated;
    static int released;
    static void *operator new(std::size_t size) {
        ++allocated;
        return std::malloc(size);
    }
    static void operator delete(void *pointer) noexcept {
        ++released;
        std::free(pointer);
    }
};
int PoolOwned::allocated = 0;
int PoolOwned::released = 0;

struct LeftSide { virtual ~LeftSide() {} };
struct VirtualBase { virtual ~VirtualBase() {} };
struct VirtualTarget : LeftSide, VirtualBase {
    static int destroyed;
    virtual ~VirtualTarget() { ++destroyed; }
};
int VirtualTarget::destroyed = 0;

struct CounterReset {
    ~CounterReset() {
        Counted::live = Counted::destroyed = 0;
        Ordered::next = Ordered::destroyed = 0;
        Throwing::attempts = 0;
        ThrowingArray::constructed = ThrowingArray::destroyed = 0;
        Aligned::live = 0;
        PoolOwned::allocated = PoolOwned::released = 0;
        VirtualTarget::destroyed = 0;
    }
};

struct SingleEvaluation {
    explicit SingleEvaluation(int v) : value(v) {}
    int value;
};

void testSmartAndStlLifetimes(int *utest_result) {
    const Stats before = stats();
    {
        std::unique_ptr<Plain> object(hplNew(Plain, (41)));
        if (!expect(utest_result, object && hpl::cMemoryManager::IsValid(object.get()) && object->value == 41,
                    "hplNew was not owned by ordinary unique_ptr deleter")) return;
        const Stats held = stats();
        if (!expect(utest_result, held.bytes > before.bytes && held.units == before.units + 1, "unique_ptr allocation was not tracked")) return;
    }
    if (!expectSame(utest_result, stats(), before, "unique_ptr deletion did not release hplNew allocation")) return;

    {
        auto object = std::make_unique<Plain>(9);
        std::vector<Plain> values;
        values.reserve(5);
        values.emplace_back(1);
        values.emplace_back(2);
        values.emplace_back(3);
        if (!expect(utest_result, object && hpl::cMemoryManager::IsValid(object.get()) &&
                    values.size() == 3 && hpl::cMemoryManager::IsValid(values.data()),
                    "STL lifetime setup failed")) return;
        if (!expect(utest_result, stats().bytes > before.bytes && stats().units > before.units, "STL allocations were not tracked")) return;
    }
    expectSame(utest_result, stats(), before, "STL allocations were not released");
}

void testArraysAndAlignment(int *utest_result) {
    CounterReset counters;
    const Stats before = stats();
    Counted::live = 0;
    Counted::destroyed = 0;
    {
        ArrayCleanup<Counted> cleanup{hplNewArray(Counted, 4)};
        if (!expect(utest_result, cleanup.objects && Counted::live == 4, "nontrivial array construction failed")) return;
        hplDeleteArray(cleanup.objects); cleanup.release();
        if (!expect(utest_result, Counted::live == 0 && Counted::destroyed == 4, "array destruction was not complete")) return;
    }
    if (!expectSame(utest_result, stats(), before, "nontrivial array allocation leaked")) return;

    Ordered::next = 0;
    Ordered::destroyed = 0;
    ArrayCleanup<Ordered> orderedCleanup{hplNewArray(Ordered, 4)};
    Ordered *ordered = orderedCleanup.objects;
    if (!expect(utest_result, ordered != nullptr, "ordered array construction failed")) return;
    hplDeleteArray(ordered);
    orderedCleanup.release();
    if (!expect(utest_result, Ordered::destroyed == 4 && Ordered::order[0] == 3 && Ordered::order[1] == 2 &&
                Ordered::order[2] == 1 && Ordered::order[3] == 0,
            "array elements were not destroyed in reverse order")) return;
    if (!expectSame(utest_result, stats(), before, "ordered array allocation leaked")) return;

    Aligned::live = 0;
    {
        ScalarCleanup<Aligned> objectCleanup{hplNew(Aligned, ())};
        ArrayCleanup<Aligned> objectsCleanup{hplNewArray(Aligned, 3)};
        Aligned *object = objectCleanup.object; Aligned *objects = objectsCleanup.objects;
        if (!expect(utest_result, object && objects && reinterpret_cast<std::uintptr_t>(object) % alignof(Aligned) == 0 &&
                    reinterpret_cast<std::uintptr_t>(objects) % alignof(Aligned) == 0,
                "over-aligned scalar or array was misaligned")) return;
        if (!expect(utest_result, Aligned::live == 4, "over-aligned construction count was wrong")) return;
        objectsCleanup.release(); hplDeleteArray(objects); objectCleanup.release(); hplDelete(object);
        if (!expect(utest_result, Aligned::live == 0, "over-aligned destruction was incomplete")) return;
    }
    expectSame(utest_result, stats(), before, "over-aligned allocation leaked");
}

void testFailuresAndPlacement(int *utest_result) {
    CounterReset counters;
    const Stats before = stats();
    bool scalarThrew = false;
    try {
        (void)hplNew(Throwing, (7));
    } catch (int value) {
        scalarThrew = value == 19;
    }
    if (!expect(utest_result, scalarThrew && Throwing::attempts == 1, "throwing scalar constructor did not propagate")) return;
    if (!expectSame(utest_result, stats(), before, "throwing scalar constructor leaked its allocation")) return;

    ThrowingArray::constructed = 0;
    ThrowingArray::destroyed = 0;
    bool arrayThrew = false;
    try {
        (void)hplNewArray(ThrowingArray, 8);
    } catch (int value) {
        arrayThrew = value == 23;
    }
    if (!expect(utest_result, arrayThrew && ThrowingArray::constructed == 4 && ThrowingArray::destroyed == 3,
            "partial array construction did not reverse-destroy constructed elements")) return;
    if (!expectSame(utest_result, stats(), before, "partial array constructor leaked its allocation")) return;

    bool alignedThrew = false;
    try {
        (void)new (std::nothrow) AlignedThrowing;
    } catch (...) {
        alignedThrew = true;
    }
    if (!expect(utest_result, alignedThrew, "aligned nothrow constructor failure was swallowed")) return;
    if (!expectSame(utest_result, stats(), before, "aligned nothrow constructor failure leaked")) return;

    alignas(Aligned) unsigned char storage[sizeof(Aligned)];
    const Stats placementBefore = stats();
    Aligned *placed = ::new (static_cast<void *>(storage)) Aligned;
    if (!expect(utest_result, placed == reinterpret_cast<Aligned *>(storage) && stats().bytes == placementBefore.bytes &&
                stats().units == placementBefore.units,
            "placement new existing storage was tracked")) { placed->~Aligned(); return; }
    placed->~Aligned();
    expectSame(utest_result, stats(), placementBefore, "placement object changed allocator statistics");
}

void testVirtualAndClassSpecificDelete(int *utest_result) {
    CounterReset counters;
    const Stats before = stats();
    VirtualTarget::destroyed = 0;
    {
        ScalarCleanup<VirtualTarget> cleanup{hplNew(VirtualTarget, ())};
        VirtualTarget *derived = cleanup.object;
        if (!expect(utest_result, derived != nullptr, "virtual target allocation failed")) return;
        VirtualBase *base = derived;
        if (!expect(utest_result, reinterpret_cast<void *>(base) != reinterpret_cast<void *>(derived),
                "test did not produce a nonzero-adjusted base")) return;
        cleanup.release();
        hplDelete(base);
        if (!expect(utest_result, VirtualTarget::destroyed == 1, "virtual deletion through adjusted base failed")) return;
    }
    if (!expectSame(utest_result, stats(), before, "virtual deletion leaked its allocation")) return;

    PoolOwned::allocated = 0;
    PoolOwned::released = 0;
    {
        ScalarCleanup<PoolOwned> cleanup{hplNew(PoolOwned, ())}; PoolOwned *object = cleanup.object;
        if (!expect(utest_result, PoolOwned::allocated == 1 && stats().bytes == before.bytes && stats().units == before.units,
                "class-specific pool allocation was incorrectly tracked")) return;
        cleanup.release();
        hplDelete(object);
        if (!expect(utest_result, PoolOwned::released == 1, "class-specific delete ownership was not preserved")) return;
    }
    expectSame(utest_result, stats(), before, "class-specific pool delete changed tracker state");
}

void testSingleEvaluation(int *utest_result) {
    CounterReset counters;
    const Stats before = stats();
    int constructorEvaluations = 0;
    ScalarCleanup<SingleEvaluation> objectCleanup{hplNew(SingleEvaluation, (++constructorEvaluations))};
    SingleEvaluation *object = objectCleanup.object;
    if (!expect(utest_result, constructorEvaluations == 1 && object && object->value == 1, "hplNew constructor operand was evaluated more than once")) return;
    objectCleanup.release();
    hplDelete(object);

    int countEvaluations = 0;
    ArrayCleanup<Counted> objectsCleanup{hplNewArray(Counted, ++countEvaluations)};
    Counted *objects = objectsCleanup.objects;
    if (!expect(utest_result, countEvaluations == 1 && objects && Counted::live == 1, "hplNewArray count was evaluated more than once")) return;
    objectsCleanup.release();
    hplDeleteArray(objects);
    expectSame(utest_result, stats(), before, "single-evaluation lifetime test leaked");
}

} // namespace

UTEST(HplNativeLifetimes, SmartAndStl) {
    hpl_memory_test::CaseScope scope(utest_result); testSmartAndStlLifetimes(utest_result);
}
UTEST(HplNativeLifetimes, ArraysAndAlignment) {
    hpl_memory_test::CaseScope scope(utest_result); testArraysAndAlignment(utest_result);
}
UTEST(HplNativeLifetimes, FailuresAndPlacement) {
    hpl_memory_test::CaseScope scope(utest_result); testFailuresAndPlacement(utest_result);
}
UTEST(HplNativeLifetimes, VirtualAndClassSpecificDelete) {
    hpl_memory_test::CaseScope scope(utest_result); testVirtualAndClassSpecificDelete(utest_result);
}
UTEST(HplNativeLifetimes, SingleEvaluation) {
    hpl_memory_test::CaseScope scope(utest_result); testSingleEvaluation(utest_result);
}
