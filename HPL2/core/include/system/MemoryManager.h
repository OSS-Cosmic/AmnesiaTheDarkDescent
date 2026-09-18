/*
 * Copyright © 2009-2020 Frictional Games
 * 
 * This file is part of Amnesia: The Dark Descent.
 * 
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version. 

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef HPL_MEMORY_MANAGER_H
#define HPL_MEMORY_MANAGER_H

#include <cstddef>
#include <cstdlib>
#include <string>
#include "system/MemoryManagerBridge.h"

namespace hpl {
struct sMemoryStatistics {
	std::size_t totalReportedMemory, totalActualMemory, peakReportedMemory, peakActualMemory;
	std::size_t accumulatedReportedMemory, accumulatedActualMemory, accumulatedAllocUnitCount;
	std::size_t totalAllocUnitCount, peakAllocUnitCount;
	bool enabled;
};

class cMemoryManager {
public:
	static bool IsValid(const void *apData);
	static void LogResults();
	static void SetReportPath(const char *apPath);
	static void SetReportPath(const std::wstring &asPath);
	// Statistics and creation counts use size_t for long-running processes.
	static sMemoryStatistics GetStatistics();
	static sMemoryStatistics GetMemoryStatistics(); // compatibility alias
	// Counts successful explicitly annotated allocations/reallocations while
	// enabled. Toggling the window does not reset the cumulative count.
	static void SetLogCreation(bool abX);
	static bool GetLogCreation();
	static std::size_t GetCreationCount();
};

#ifdef MEMORY_MANAGER_ACTIVE
// Keep the native new-expression at the macro callsite: this preserves access
// to private constructors and exact constructor argument semantics. Accessible
// class-specific allocation functions are deliberately unannotated because
// they may bypass the global allocator; global allocations remain tracked.
// MSVC rejects __FUNCTION__ at namespace scope; its source-location intrinsic
// is valid there and still reports the surrounding function at block scope.
#if defined(_MSC_VER)
#define HPL_CURRENT_FUNCTION __builtin_FUNCTION()
#else
#define HPL_CURRENT_FUNCTION __FUNCTION__
#endif
#if defined(__cpp_aligned_new)
#define hplNew(classType, constructor) \
	(hpl::memory::ScopedAllocationSite(__FILE__, __LINE__, HPL_CURRENT_FUNCTION, !(hpl::memory::HasApplicableClassSpecificScalarNew<classType>([](auto *hplProbe) -> decltype(hplProbe->operator new(std::size_t{})) { return nullptr; }) || hpl::memory::HasApplicableClassSpecificAlignedScalarNew<classType>([](auto *hplProbe) -> decltype(hplProbe->operator new(std::size_t{}, std::align_val_t{})) { return nullptr; }))) << new classType constructor)
#define hplNewArray(classType, amount) \
	[](auto hplArrayCount, const char *hplFile, unsigned int hplLine, const char *hplFunction) { return hpl::memory::ScopedAllocationSite(hplFile, hplLine, hplFunction, !(hpl::memory::HasApplicableClassSpecificArrayNew<classType>([](auto *hplProbe) -> decltype(hplProbe->operator new[](std::size_t{})) { return nullptr; }) || hpl::memory::HasApplicableClassSpecificAlignedArrayNew<classType>([](auto *hplProbe) -> decltype(hplProbe->operator new[](std::size_t{}, std::align_val_t{})) { return nullptr; }))) << new classType[hplArrayCount]; }(amount, __FILE__, __LINE__, HPL_CURRENT_FUNCTION)
#else
#define hplNew(classType, constructor) \
	(hpl::memory::ScopedAllocationSite(__FILE__, __LINE__, HPL_CURRENT_FUNCTION, !hpl::memory::HasApplicableClassSpecificScalarNew<classType>([](auto *hplProbe) -> decltype(hplProbe->operator new(std::size_t{})) { return nullptr; })) << new classType constructor)
#define hplNewArray(classType, amount) \
	[](auto hplArrayCount, const char *hplFile, unsigned int hplLine, const char *hplFunction) { return hpl::memory::ScopedAllocationSite(hplFile, hplLine, hplFunction, !hpl::memory::HasApplicableClassSpecificArrayNew<classType>([](auto *hplProbe) -> decltype(hplProbe->operator new[](std::size_t{})) { return nullptr; })) << new classType[hplArrayCount]; }(amount, __FILE__, __LINE__, HPL_CURRENT_FUNCTION)
#endif
#define hplMalloc(amount) hpl::memory::AllocateBuffer((amount), __FILE__, __LINE__, HPL_CURRENT_FUNCTION)
#define hplRealloc(data, amount) hpl::memory::ReallocateBuffer((data), (amount), __FILE__, __LINE__, HPL_CURRENT_FUNCTION)
#define hplDelete(data) delete (data)
#define hplDeleteArray(data) delete [] (data)
#define hplFree(data) hpl::memory::FreeBuffer((data), __FILE__, __LINE__, HPL_CURRENT_FUNCTION)
#else
#define hplNew(classType, constructor) new classType constructor
#define hplNewArray(classType, amount) new classType [ amount ]
#define hplMalloc(amount) malloc(amount)
#define hplRealloc(data, amount) realloc((data), (amount))
#define hplDelete(data) delete (data)
#define hplDeleteArray(data) delete [] (data)
#define hplFree(data) free(data)
#endif
} // namespace hpl
#endif // HPL_MEMORY_MANAGER_H
