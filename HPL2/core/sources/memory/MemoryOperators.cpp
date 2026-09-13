#ifdef MEMORY_MANAGER_ACTIVE
#include "system/MemoryManagerBridge.h"
#include "mmgr.h"
#include <cstddef>
#include <new>

namespace {
constexpr std::size_t defaultNewAlignment() noexcept {
#if defined(__STDCPP_DEFAULT_NEW_ALIGNMENT__)
	return __STDCPP_DEFAULT_NEW_ALIGNMENT__;
#else
	return alignof(std::max_align_t);
#endif
}

void *allocate(std::size_t size, std::size_t alignment, unsigned int type) {
	hpl::memory::AllocationSite site = { nullptr, nullptr, 0 };
	const bool explicitSite = hpl::memory::ConsumeAllocationSite(site);
	for (;;) {
		if (void *p = mmgrAllocator(site.file, site.line, site.function, type, alignment, size)) {
			if (explicitSite) hpl::memory::NoteExplicitAllocation();
			return p;
		}
		std::new_handler handler = std::get_new_handler();
		if (!handler) throw std::bad_alloc();
		handler();
	}
}
void *allocateNoexcept(std::size_t size, std::size_t alignment, unsigned int type) noexcept {
	try { return allocate(size, alignment, type); } catch (...) { return nullptr; }
}
void release(void *p, unsigned int type) noexcept {
	if (p) mmgrDeallocator(nullptr, 0, nullptr, type, p);
}
}

void *operator new(std::size_t n) { return allocate(n, defaultNewAlignment(), m_alloc_new); }
void *operator new[](std::size_t n) { return allocate(n, defaultNewAlignment(), m_alloc_new_array); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return allocateNoexcept(n, defaultNewAlignment(), m_alloc_new); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return allocateNoexcept(n, defaultNewAlignment(), m_alloc_new_array); }

void operator delete(void *p) noexcept { release(p, m_alloc_delete); }
void operator delete[](void *p) noexcept { release(p, m_alloc_delete_array); }
void operator delete(void *p, std::size_t) noexcept { release(p, m_alloc_delete); }
void operator delete[](void *p, std::size_t) noexcept { release(p, m_alloc_delete_array); }
void operator delete(void *p, const std::nothrow_t &) noexcept { release(p, m_alloc_delete); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { release(p, m_alloc_delete_array); }

#if defined(__cpp_aligned_new)
void *operator new(std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a), m_alloc_new); }
void *operator new[](std::size_t n, std::align_val_t a) { return allocate(n, static_cast<std::size_t>(a), m_alloc_new_array); }
void *operator new(std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept { return allocateNoexcept(n, static_cast<std::size_t>(a), m_alloc_new); }
void *operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t &) noexcept { return allocateNoexcept(n, static_cast<std::size_t>(a), m_alloc_new_array); }
void operator delete(void *p, std::align_val_t) noexcept { release(p, m_alloc_delete); }
void operator delete[](void *p, std::align_val_t) noexcept { release(p, m_alloc_delete_array); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { release(p, m_alloc_delete); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { release(p, m_alloc_delete_array); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { release(p, m_alloc_delete); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { release(p, m_alloc_delete_array); }
#endif
#endif
