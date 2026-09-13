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

#include "system/MemoryManager.h"
#include "system/LowLevelSystem.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>

#ifdef MEMORY_MANAGER_ACTIVE
#include "mmgr.h"
#endif

namespace {
thread_local hpl::memory::ScopedAllocationSite *gSite = nullptr;
std::atomic<bool> gLogCreation(false);
std::atomic<std::size_t> gCreationCount(0);
std::atomic_flag gReportPathLock = ATOMIC_FLAG_INIT;
char gReportPath[4096] = "memreport.log";

struct ReportPathLock {
	ReportPathLock() noexcept { while (gReportPathLock.test_and_set(std::memory_order_acquire)) {} }
	~ReportPathLock() noexcept { gReportPathLock.clear(std::memory_order_release); }
};
}

namespace hpl { namespace memory {
ScopedAllocationSite::ScopedAllocationSite(const char *file, unsigned int line, const char *function, bool enabled) noexcept
	: mPrevious(gSite), mSite({ file, function, line }), mPending(enabled), mActive(true) { gSite = this; }
ScopedAllocationSite::~ScopedAllocationSite() noexcept { Close(); }
void ScopedAllocationSite::Close() noexcept {
	if (!mActive) return;
	gSite = mPrevious;
	mActive = false;
}
bool ConsumeAllocationSite(AllocationSite &site) noexcept {
	if (!gSite || !gSite->mPending || !gSite->mActive) return false;
	site = gSite->mSite; gSite->mPending = false; return true;
}
void NoteExplicitAllocation() noexcept {
	if (gLogCreation.load(std::memory_order_relaxed)) gCreationCount.fetch_add(1, std::memory_order_relaxed);
}

#ifdef MEMORY_MANAGER_ACTIVE
void *AllocateBuffer(std::size_t size, const char *file, unsigned int line, const char *function) noexcept {
	void *p = mmgrAllocator(file, line, function, m_alloc_malloc, alignof(std::max_align_t), size);
	if (p) NoteExplicitAllocation();
	return p;
}
void *ReallocateBuffer(void *data, std::size_t size, const char *file, unsigned int line, const char *function) noexcept {
	void *p = mmgrReallocator(file, line, function, m_alloc_realloc, size, data);
	if (p) NoteExplicitAllocation();
	return p;
}
void FreeBuffer(void *data, const char *file, unsigned int line, const char *function) noexcept {
	mmgrDeallocator(file, line, function, m_alloc_free, data);
}
#else
void *AllocateBuffer(std::size_t size, const char *, unsigned int, const char *) noexcept { return std::malloc(size); }
void *ReallocateBuffer(void *data, std::size_t size, const char *, unsigned int, const char *) noexcept { return std::realloc(data, size); }
void FreeBuffer(void *data, const char *, unsigned int, const char *) noexcept { std::free(data); }
#endif
}} // namespace hpl::memory

namespace hpl {
bool cMemoryManager::IsValid(const void *data) {
#ifdef MEMORY_MANAGER_ACTIVE
	return data && mmgrContainsAddress(data);
#else
	(void)data; return false;
#endif
}
void cMemoryManager::SetReportPath(const char *path) {
	if (!path || !*path) return;
	const std::size_t n = std::strlen(path);
	if (n >= sizeof(gReportPath)) {
		std::fputs("Memory report path is too long; retaining the previous path.\n", stderr);
		return;
	}
	ReportPathLock lock;
	std::memcpy(gReportPath, path, n + 1);
}
void cMemoryManager::SetReportPath(const std::wstring &path) {
	// Convert before taking ReportPathLock.  The conversion may allocate through
	// the tracked global allocator, so it must never happen while the facade
	// path lock (or the backend allocation lock) is held.
	std::string utf8;
	utf8.reserve(path.size());
	for (std::size_t i = 0; i < path.size();) {
		std::uint32_t codePoint = static_cast<std::uint32_t>(path[i++]);
#if defined(_WIN32)
		if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
			if (i < path.size()) {
				const std::uint32_t low = static_cast<std::uint32_t>(path[i]);
				if (low >= 0xDC00 && low <= 0xDFFF) {
					++i;
					codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
				} else codePoint = 0xFFFD;
			} else codePoint = 0xFFFD;
		} else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) codePoint = 0xFFFD;
#else
		if (codePoint >= 0xD800 && codePoint <= 0xDFFF) codePoint = 0xFFFD;
#endif
		if (codePoint > 0x10FFFF) codePoint = 0xFFFD;
		if (codePoint <= 0x7F) utf8.push_back(static_cast<char>(codePoint));
		else if (codePoint <= 0x7FF) {
			utf8.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
			utf8.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		} else if (codePoint <= 0xFFFF) {
			utf8.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
			utf8.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			utf8.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		} else {
			utf8.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
			utf8.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
			utf8.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
			utf8.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
		}
	}
	SetReportPath(utf8.c_str());
}
sMemoryStatistics cMemoryManager::GetStatistics() {
	sMemoryStatistics result = {};
#ifdef MEMORY_MANAGER_ACTIVE
	const sMStats s = mmgrGetMemoryStatistics();
	result.totalReportedMemory=s.totalReportedMemory; result.totalActualMemory=s.totalActualMemory;
	result.peakReportedMemory=s.peakReportedMemory; result.peakActualMemory=s.peakActualMemory;
	result.accumulatedReportedMemory=s.accumulatedReportedMemory; result.accumulatedActualMemory=s.accumulatedActualMemory;
	result.accumulatedAllocUnitCount=s.accumulatedAllocUnitCount; result.totalAllocUnitCount=s.totalAllocUnitCount;
	result.peakAllocUnitCount=s.peakAllocUnitCount; result.enabled=true;
#endif
	return result;
}
sMemoryStatistics cMemoryManager::GetMemoryStatistics() { return GetStatistics(); }
void cMemoryManager::LogResults() {
#ifdef MEMORY_MANAGER_ACTIVE
	char reportPath[sizeof(gReportPath)];
	{
		ReportPathLock lock;
		std::memcpy(reportPath, gReportPath, sizeof(reportPath));
	}
	mmgrDumpMemoryReport(reportPath, true);
	const sMemoryStatistics s = GetStatistics();
	// mmgrDumpMemoryReport and GetStatistics have both released the backend
	// allocation lock before this normal engine log call.  Keep this summary
	// useful even when the report could not be opened: it describes the live
	// snapshot, not the report write result.
	Log("Memory tracking enabled: report generation attempted at %s; outstanding snapshot: %zu allocations, %zu reported bytes, %zu actual bytes, %zu overhead bytes; peak snapshot: %zu allocations, %zu reported bytes, %zu actual bytes (peak overhead is not independently measurable).\n",
		reportPath, s.totalAllocUnitCount, s.totalReportedMemory, s.totalActualMemory,
		s.totalActualMemory - s.totalReportedMemory, s.peakAllocUnitCount,
		s.peakReportedMemory, s.peakActualMemory);
#else
	Log("Memory tracking disabled; no memory report was written.\n");
#endif
}
void cMemoryManager::SetLogCreation(bool enabled) { gLogCreation.store(enabled, std::memory_order_relaxed); }
bool cMemoryManager::GetLogCreation() { return gLogCreation.load(std::memory_order_relaxed); }
std::size_t cMemoryManager::GetCreationCount() { return gCreationCount.load(std::memory_order_relaxed); }
} // namespace hpl
