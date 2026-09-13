#include "system/MemoryManager.h"
#include "hpl_memory_manager_fixture.h"
#include "utest.h"

#include <cstddef>
#include <cstdlib>
#include <new>
#include <type_traits>

namespace {

#define expect(result, condition, message) \
	(::hpl_memory_test::check((condition), (result), #condition, __FILE__, __LINE__, (message)))

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

struct PrivateScalar {
	static PrivateScalar *make() { return hplNew(PrivateScalar, ()); }
	private: PrivateScalar() {}
};

struct PrivateArray {
	static PrivateArray *make(int count) { return hplNewArray(PrivateArray, count); }
	private: PrivateArray() {}
};

struct NullPointerArgument {
	explicit NullPointerArgument(int *pointer) : isNull(pointer == nullptr) {}
	bool isNull;
};

struct Immovable {
	Immovable() = default;
	Immovable(const Immovable &) = delete;
	Immovable(Immovable &&) = delete;
};
struct TakesImmovable {
	explicit TakesImmovable(Immovable) {}
};

struct PrivatePool {
	static PrivatePool *make() { return hplNew(PrivatePool, ()); }
	static void *operator new(std::size_t size) { return std::malloc(size); }
	static void operator delete(void *pointer) noexcept { std::free(pointer); }
	~PrivatePool() { delete nested; }
	private:
	PrivatePool() : nested(new int(7)) {}
	int *nested;
};

struct NestedInner {};
NestedInner *namespaceScalar = hplNew(NestedInner, ());
NestedInner *namespaceArray = hplNewArray(NestedInner, 2);
struct NamespaceCleanup {
	~NamespaceCleanup() { hplDelete(namespaceScalar); hplDeleteArray(namespaceArray); }
} namespaceCleanup;

struct NestedOuter {
	NestedOuter() : inner(hplNew(NestedInner, ())) {}
	~NestedOuter() { hplDelete(inner); }
	NestedInner *inner;
};

NestedOuter *namespaceScopeAllocation() { return hplNew(NestedOuter, ()); }

struct PrivateScalarWithOrdinaryConstructor {
	static PrivateScalarWithOrdinaryConstructor *make() {
		return hplNew(PrivateScalarWithOrdinaryConstructor, ());
	}
	~PrivateScalarWithOrdinaryConstructor() { delete nested; }
	static void operator delete(void *pointer) noexcept { std::free(pointer); }

private:
	static void *operator new(std::size_t size) { return std::malloc(size); }
	PrivateScalarWithOrdinaryConstructor() : nested(new int(11)) {}
	int *nested;
};

struct PrivateArrayWithOrdinaryConstructor {
	static PrivateArrayWithOrdinaryConstructor *make(int count) {
		return hplNewArray(PrivateArrayWithOrdinaryConstructor, count);
	}
	~PrivateArrayWithOrdinaryConstructor() { delete nested; }
	static void operator delete[](void *pointer) noexcept { std::free(pointer); }

private:
	static void *operator new[](std::size_t size) { return std::malloc(size); }
	PrivateArrayWithOrdinaryConstructor() : nested(new int(13)) {}
	int *nested;
};

} // namespace

void testHplMacroContracts(int *utest_result) {
	if (!expect(utest_result, hpl::cMemoryManager::IsValid(namespaceScalar), "namespace scalar allocation was not valid")) return;
	if (!expect(utest_result, hpl::cMemoryManager::IsValid(namespaceArray), "namespace array allocation was not valid")) return;
	ScalarCleanup<PrivateScalar> scalarCleanup{PrivateScalar::make()};
	PrivateScalar *scalar = scalarCleanup.object;
	if (!expect(utest_result, scalar != nullptr, "private scalar construction failed")) return;
	scalarCleanup.release(); hplDelete(scalar);

	ArrayCleanup<PrivateArray> arrayCleanup{PrivateArray::make(2)};
	PrivateArray *array = arrayCleanup.objects;
	if (!expect(utest_result, array != nullptr, "private array construction failed")) return;
	arrayCleanup.release(); hplDeleteArray(array);

	ScalarCleanup<NullPointerArgument> nullCleanup{hplNew(NullPointerArgument, (0))};
	NullPointerArgument *nullArgument = nullCleanup.object;
	if (!expect(utest_result, nullArgument && nullArgument->isNull, "null pointer constructor argument was changed")) return;
	nullCleanup.release();
	hplDelete(nullArgument);

	ScalarCleanup<TakesImmovable> immovableCleanup{hplNew(TakesImmovable, (Immovable()))};
	TakesImmovable *immovable = immovableCleanup.object;
	if (!expect(utest_result, immovable != nullptr, "immovable argument construction failed")) return;
	immovableCleanup.release();
	hplDelete(immovable);

	ScalarCleanup<PrivatePool> poolCleanup{PrivatePool::make()};
	PrivatePool *pool = poolCleanup.object;
	if (!expect(utest_result, pool != nullptr, "private pool construction failed")) return;
	poolCleanup.release();
	hplDelete(pool);

	const std::size_t creations = hpl::cMemoryManager::GetCreationCount();
	hpl::cMemoryManager::SetLogCreation(true);
	ScalarCleanup<PrivateScalarWithOrdinaryConstructor> privateScalarCleanup{PrivateScalarWithOrdinaryConstructor::make()};
	ArrayCleanup<PrivateArrayWithOrdinaryConstructor> privateArrayCleanup{PrivateArrayWithOrdinaryConstructor::make(2)};
	ScalarCleanup<NestedOuter> nestedWithExplicitInnerCleanup{namespaceScopeAllocation()};
	PrivateScalarWithOrdinaryConstructor *privateScalar = privateScalarCleanup.object;
	PrivateArrayWithOrdinaryConstructor *privateArray = privateArrayCleanup.objects;
	NestedOuter *nestedWithExplicitInner = nestedWithExplicitInnerCleanup.object;
	hpl::cMemoryManager::SetLogCreation(false);
	if (!expect(utest_result, privateScalar != nullptr && privateArray != nullptr && nestedWithExplicitInner != nullptr,
	            "private ordinary-constructor allocations failed")) {
		return;
	}
	if (!expect(utest_result, hpl::cMemoryManager::GetCreationCount() == creations + 2,
	            "private ordinary-constructor allocations changed creation count")) {
		return;
	}
	privateScalarCleanup.release(); privateArrayCleanup.release(); nestedWithExplicitInnerCleanup.release();
	hplDelete(privateScalar);
	hplDeleteArray(privateArray);
	hplDelete(nestedWithExplicitInner);

	ScalarCleanup<NestedOuter> nestedCleanup{namespaceScopeAllocation()};
	NestedOuter *nested = nestedCleanup.object;
	if (!expect(utest_result, nested != nullptr && nested->inner != nullptr, "nested allocation failed")) {
		return;
	}
	nestedCleanup.release();
	hplDelete(nested);
}

UTEST(HplMemory, MacroContracts) {
	hpl_memory_test::CaseScope scope(utest_result); testHplMacroContracts(utest_result);
}
