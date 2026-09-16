#include "system/MemoryManager.h"
#include "hpl_memory_manager_fixture.h"
#include <cstdlib>
namespace hpl_memory_test {
namespace {
struct AfterReportAllocation {
    ~AfterReportAllocation() {
        hpl::memory::ScopedAllocationSite site(__FILE__, __LINE__, "AfterReportAllocation");
        void *memory = new unsigned char[43];
        if (!memory || !hpl::cMemoryManager::IsValid(memory)) std::abort();
        delete [] static_cast<unsigned char *>(memory);
    }
} gAfterReportAllocation;
}
}
