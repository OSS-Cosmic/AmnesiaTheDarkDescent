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

#ifndef HPL_MEMORY_MANAGER_BRIDGE_H
#define HPL_MEMORY_MANAGER_BRIDGE_H
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
namespace hpl { namespace memory {
struct AllocationSite { const char *file; const char *function; unsigned int line; };
class ScopedAllocationSite {
public:
	ScopedAllocationSite(const char *, unsigned int, const char *, bool = true) noexcept;
	~ScopedAllocationSite() noexcept;
	void Close() noexcept;
	ScopedAllocationSite(const ScopedAllocationSite &) = delete;
	ScopedAllocationSite &operator=(const ScopedAllocationSite &) = delete;
	ScopedAllocationSite(ScopedAllocationSite &&) = delete;
	ScopedAllocationSite &operator=(ScopedAllocationSite &&) = delete;
private:
	friend bool ConsumeAllocationSite(AllocationSite &) noexcept;
	ScopedAllocationSite *mPrevious;
	AllocationSite mSite;
	bool mPending;
	bool mActive;
};

// The probe is defined at the hplNew callsite.  Keeping operator lookup in
// the lambda's lexical context is what lets a class factory access a private
// allocation function; this helper only asks whether the probe is invocable
// and never evaluates it.
template<class T, class Probe>
constexpr bool HasApplicableClassSpecificScalarNew(Probe) noexcept {
	return std::is_invocable<Probe, T *>::value;
}

template<class T, class Probe>
constexpr bool HasApplicableClassSpecificArrayNew(Probe) noexcept {
	return std::is_invocable<Probe, T *>::value;
}

#if defined(__cpp_aligned_new)
template<class T, class Probe>
constexpr bool HasApplicableClassSpecificAlignedScalarNew(Probe) noexcept {
	return std::is_invocable<Probe, T *>::value;
}

template<class T, class Probe>
constexpr bool HasApplicableClassSpecificAlignedArrayNew(Probe) noexcept {
	return std::is_invocable<Probe, T *>::value;
}
#endif

template<class T>
T *operator<<(ScopedAllocationSite &&site, T *result) noexcept {
	site.Close();
	return result;
}

bool ConsumeAllocationSite(AllocationSite &) noexcept;
void NoteExplicitAllocation() noexcept;
void *AllocateBuffer(std::size_t, const char *, unsigned int, const char *) noexcept;
void *ReallocateBuffer(void *, std::size_t, const char *, unsigned int, const char *) noexcept;
void FreeBuffer(void *, const char *, unsigned int, const char *) noexcept;

}} // namespace hpl::memory
#endif
