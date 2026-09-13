#include "utest.h"
#include "system/MemoryManager.h"

#include <cstdio>
#include <cstring>

extern bool hplTestLogContains(const char *);

UTEST(HplMemoryManagerDisabled, StatisticsAndValidity) {
    const hpl::sMemoryStatistics stats = hpl::cMemoryManager::GetMemoryStatistics();
    EXPECT_FALSE(stats.enabled);
    EXPECT_EQ(0, stats.totalReportedMemory);
    EXPECT_EQ(0, stats.totalActualMemory);
    EXPECT_EQ(0, stats.peakReportedMemory);
    EXPECT_EQ(0, stats.peakActualMemory);
    EXPECT_EQ(0, stats.accumulatedReportedMemory);
    EXPECT_EQ(0, stats.accumulatedActualMemory);
    EXPECT_EQ(0, stats.accumulatedAllocUnitCount);
    EXPECT_EQ(0, stats.totalAllocUnitCount);
    EXPECT_EQ(0, stats.peakAllocUnitCount);
    EXPECT_FALSE(hpl::cMemoryManager::IsValid(nullptr));
    int live = 7;
    EXPECT_FALSE(hpl::cMemoryManager::IsValid(&live));
}

UTEST(HplMemoryManagerDisabled, LogFlagAndAllocationsAreInert) {
    EXPECT_FALSE(hpl::cMemoryManager::GetLogCreation());
    hpl::cMemoryManager::SetLogCreation(true);
    EXPECT_TRUE(hpl::cMemoryManager::GetLogCreation());
    void *buffer = hplMalloc(17);
    EXPECT_TRUE(buffer != nullptr);
    EXPECT_FALSE(hpl::cMemoryManager::IsValid(buffer));
    hplFree(buffer);
    int *object = hplNew(int, (9));
    EXPECT_TRUE(object != nullptr);
    EXPECT_FALSE(hpl::cMemoryManager::IsValid(object));
    if (object) {
        EXPECT_EQ(9, *object);
        hplDelete(object);
    }
    EXPECT_EQ(0, hpl::cMemoryManager::GetCreationCount());
    hpl::cMemoryManager::SetLogCreation(false);
    EXPECT_FALSE(hpl::cMemoryManager::GetLogCreation());
}

UTEST(HplMemoryManagerDisabled, ReportIsNoOp) {
    const char *path = "disabled-memory-report-sentinel.txt";
    const char sentinel[] = "sentinel remains unchanged\n";
    std::FILE *file = std::fopen(path, "wbx");
    EXPECT_TRUE(file != nullptr);
    if (!file) return;
    EXPECT_EQ(sizeof(sentinel) - 1, std::fwrite(sentinel, 1, sizeof(sentinel) - 1, file));
    std::fclose(file);
    hpl::cMemoryManager::SetReportPath(path);
    hpl::cMemoryManager::LogResults();
    EXPECT_TRUE(hplTestLogContains("Memory tracking disabled; no memory report was written."));
    EXPECT_EQ(0, hpl::cMemoryManager::GetCreationCount());
    file = std::fopen(path, "rb");
    EXPECT_TRUE(file != nullptr);
    if (file) {
        char contents[sizeof(sentinel)] = {};
        EXPECT_EQ(sizeof(sentinel) - 1, std::fread(contents, 1, sizeof(contents) - 1, file));
        EXPECT_TRUE(std::strcmp(contents, sentinel) == 0);
        std::fclose(file);
    }
    std::remove(path);
}

UTEST_MAIN();
