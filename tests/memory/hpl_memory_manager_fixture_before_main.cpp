#include "system/MemoryManager.h"
#include "hpl_memory_manager_fixture.h"
#include <cstdlib>
namespace hpl_memory_test {
namespace {
struct BeforeMainAllocation {
    void *memory;
    BeforeMainAllocation() : memory(nullptr) {
        hpl::memory::ScopedAllocationSite site(__FILE__, __LINE__, "BeforeMainAllocation");
        memory = new unsigned char[41];
        if (!memory) std::abort();
    }
    ~BeforeMainAllocation() { delete [] static_cast<unsigned char *>(memory); }
} gBeforeMainAllocation;
}
}
