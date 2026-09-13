#include "resources/FileSearcher.h"
#include "system/Platform.h"
#include "utest.h"

#include <map>

namespace {

struct FakeDirectory {
  hpl::tWStringVec files;
  hpl::tWStringVec folders;
};

std::map<hpl::tWString, FakeDirectory> g_fakeDirectories;

hpl::tWString Normalize(const hpl::tWString &path) {
  hpl::tWString normalized = path;
  for (wchar_t &character : normalized) {
    if (character == L'\\')
      character = L'/';
  }
  return normalized;
}

const FakeDirectory *FindFakeDirectory(const hpl::tWString &path) {
  const auto it = g_fakeDirectories.find(Normalize(path));
  return it == g_fakeDirectories.end() ? nullptr : &it->second;
}

void SetUpFakeTree() {
  g_fakeDirectories.clear();

  g_fakeDirectories[L"/virtual/tree"].folders = {L"textures", L"mod"};
  g_fakeDirectories[L"/virtual/tree/textures"].files = {
      L"foo.dde", L"foo.dds", L"foo.ddt"};
  g_fakeDirectories[L"/virtual/tree/mod"].folders = {L"textures"};
  g_fakeDirectories[L"/virtual/tree/mod/textures"].folders = {L"detail"};
  g_fakeDirectories[L"/virtual/tree/mod/textures/detail"].files = {
      L"foo.dde", L"foo.dds", L"foo.ddt"};

  g_fakeDirectories[L"/virtual/single"].files = {L"Only.DDS"};

  g_fakeDirectories[L"/virtual/tie/a/textures"].files = {
      L"foo.dde", L"foo.dds", L"foo.ddt"};
  g_fakeDirectories[L"/virtual/tie/b/textures"].files = {
      L"foo.dde", L"foo.dds", L"foo.ddt"};

  g_fakeDirectories[L"/virtual/priority/stock"].files = {L"foo.dds"};
  g_fakeDirectories[L"/virtual/priority/override"].files = {L"foo.dds"};
  g_fakeDirectories[L"/virtual/priority/stock/materials/textures"].files = {
      L"foo.dds"};

  g_fakeDirectories[L"/virtual/bare/stock"].files = {L"foo.dds"};
  g_fakeDirectories[L"/virtual/bare/override"].files = {L"foo.dds"};

  g_fakeDirectories[L"/virtual/equal/score/winner/materials"].files = {
      L"foo.dds"};
  g_fakeDirectories[L"/virtual/equal/score/loser"].files = {L"foo.dds"};

  g_fakeDirectories[L"/virtual/dup/first"].files = {L"foo.dds"};
  g_fakeDirectories[L"/virtual/dup/second"].files = {L"foo.dds"};

  g_fakeDirectories[L"/virtual/dup/repeat"].folders = {L"nested"};
  g_fakeDirectories[L"/virtual/dup/repeat"].files = {L"repeat.dds"};
  g_fakeDirectories[L"/virtual/dup/repeat/nested"].files = {
      L"repeat.dds"};

  g_fakeDirectories[L"/virtual/dup/priority/override"].files = {
      L"foo.dds"};
  g_fakeDirectories[L"/virtual/dup/priority/default"].files = {
      L"foo.dds"};
  g_fakeDirectories[L"/virtual/dup/priority/readd"].files = {
      L"foo.dds"};
  g_fakeDirectories[L"/virtual/dup/priority/competing"].files = {
      L"foo.dds"};

  g_fakeDirectories[L"/virtual/rescan"].files = {L"original.dds"};

  g_fakeDirectories[L"/virtual/dup/delta/first"].files = {
      L"level.map_delta"};
  g_fakeDirectories[L"/virtual/dup/delta/second"].files = {
      L"level.map_delta"};

  g_fakeDirectories[L"/virtual/custom-story/stock/assets"].folders = {
      L"textures", L"meshes", L"materials"};
  g_fakeDirectories[L"/virtual/custom-story/stock/assets/textures"].files = {
      L"shared.dds"};
  g_fakeDirectories[L"/virtual/custom-story/stock/assets/meshes"].files = {
      L"shared.msh"};
  g_fakeDirectories[L"/virtual/custom-story/stock/assets/materials"].files = {
      L"shared.mat"};

  g_fakeDirectories[L"/virtual/custom-story/story"].folders = {
      L"textures", L"meshes", L"materials"};
  g_fakeDirectories[L"/virtual/custom-story/story/textures"].files = {
      L"shared.dds"};
  g_fakeDirectories[L"/virtual/custom-story/story/meshes"].files = {
      L"shared.msh"};
  g_fakeDirectories[L"/virtual/custom-story/story/materials"].files = {
      L"shared.mat"};
  for (const wchar_t *story : {L"story-a", L"story-b"}) {
    const hpl::tWString root = hpl::tWString(L"/virtual/custom-story/") + story;
    g_fakeDirectories[root].folders = {L"textures", L"meshes", L"materials"};
    g_fakeDirectories[root + L"/textures"].files = {L"shared.dds"};
    g_fakeDirectories[root + L"/meshes"].files = {L"shared.msh"};
    g_fakeDirectories[root + L"/materials"].files = {L"shared.mat"};
  }

  g_fakeDirectories[L"/virtual/scopes/stock"].folders = {
      L"textures", L"meshes", L"materials", L"recursive"};
  g_fakeDirectories[L"/virtual/scopes/stock/textures"].files = {
      L"shared.dds"};
  g_fakeDirectories[L"/virtual/scopes/stock/meshes"].files = {
      L"shared.msh"};
  g_fakeDirectories[L"/virtual/scopes/stock/materials"].files = {
      L"shared.mat"};
  g_fakeDirectories[L"/virtual/scopes/stock/recursive"].folders = {
      L"nested"};
  g_fakeDirectories[L"/virtual/scopes/stock/recursive"].files = {
      L"stock-only.dds", L"level.map_delta"};
  g_fakeDirectories[L"/virtual/scopes/stock/recursive/nested"].files = {
      L"stock-nested.dds"};

  for (const wchar_t *scope : {L"a", L"b"}) {
    const hpl::tWString root = hpl::tWString(L"/virtual/scopes/") + scope;
    g_fakeDirectories[root].folders = {
        L"textures", L"meshes", L"materials", L"recursive"};
    g_fakeDirectories[root + L"/textures"].files = {L"shared.dds"};
    g_fakeDirectories[root + L"/meshes"].files = {L"shared.msh"};
    g_fakeDirectories[root + L"/materials"].files = {L"shared.mat"};
    g_fakeDirectories[root + L"/recursive"].folders = {L"nested"};
    g_fakeDirectories[root + L"/recursive"].files = {
        (hpl::tWString(scope) + L"-only.dds"), L"level.map_delta"};
    g_fakeDirectories[root + L"/recursive/nested"].files = {
        (hpl::tWString(scope) + L"-nested.dds")};
  }

  g_fakeDirectories[L"/virtual/scopes/permanent/language"].files = {
      L"menu.lang"};
  g_fakeDirectories[L"/virtual/scopes/permanent/config"].files = {
      L"settings.cfg"};
  g_fakeDirectories[L"/virtual/scopes/overlap/parent"].folders = {L"child"};
  g_fakeDirectories[L"/virtual/scopes/overlap/parent"].files = {
      L"overlap.dds"};
  g_fakeDirectories[L"/virtual/scopes/overlap/parent/child"].files = {
      L"overlap.dds"};
  g_fakeDirectories[L"/virtual/scopes/overlap/exact"].files = {
      L"overlap.dds"};
  g_fakeDirectories[L"/virtual/scopes/overlap/competing"].files = {
      L"overlap.dds"};
  g_fakeDirectories[L"/external/permanent"].files = {L"shared.ext"};
  g_fakeDirectories[L"/external/scoped"].files = {L"shared.ext"};
  g_fakeDirectories[L"/virtual/story-language/permanent"].files = {
      L"menu.lang"};
  g_fakeDirectories[L"/virtual/story-language/story"].files = {
      L"menu.lang"};
}

void CheckDepthAndAdjacentNameResolution(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/tree", "*", true);

  int equalCount = -1;
  const hpl::tWString &resolved = searcher.GetFilePath(
      "wanted/root/mod/textures/detail/foo.dds", &equalCount);
  ASSERT_TRUE_MSG(resolved == L"/virtual/tree/mod/textures/detail/foo.dds",
                  "the deepest matching trailing directories win");
  ASSERT_EQ_MSG(equalCount, 3,
                "depth scoring counts the matching trailing directories");
  ASSERT_TRUE_MSG(resolved.find(L"foo.dds") != hpl::tWString::npos,
                  "adjacent bare filenames never replace the queried filename");
}

void CheckTieBreakOrder(int *utest_result) {
  hpl::cFileSearcher firstA;
  firstA.AddDirectory(L"/virtual/tie/a/textures", "*", false);
  firstA.AddDirectory(L"/virtual/tie/b/textures", "*", false);

  int equalCount = -1;
  const hpl::tWString &aThenB =
      firstA.GetFilePath("wanted/textures/foo.dds", &equalCount);
  ASSERT_TRUE_MSG(
      aThenB == L"/virtual/tie/a/textures/foo.dds",
      "the first equally-scored candidate wins when A is indexed first");
  ASSERT_EQ_MSG(equalCount, 1, "equal candidates report their shared depth");

  hpl::cFileSearcher firstB;
  firstB.AddDirectory(L"/virtual/tie/b/textures", "*", false);
  firstB.AddDirectory(L"/virtual/tie/a/textures", "*", false);

  const hpl::tWString &bThenA =
      firstB.GetFilePath("wanted/textures/foo.dds", &equalCount);
  ASSERT_TRUE_MSG(
      bThenA == L"/virtual/tie/b/textures/foo.dds",
      "the first equally-scored candidate wins when B is indexed first");
}

void CheckMissingAndSingleCandidate(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/single", "*", false);

  int equalCount = 99;
  const hpl::tWString &missing =
      searcher.GetFilePath("missing.dds", &equalCount);
  ASSERT_TRUE_MSG(missing.empty(), "a missing bare filename resolves to empty");
  ASSERT_EQ_MSG(equalCount, 0, "a missing bare filename reports zero matches");

  ASSERT_TRUE_MSG(searcher.GetFilePath("only.dds") ==
                      L"/virtual/single/Only.DDS",
                  "a single candidate resolves without a path hint");
}

void CheckHigherPriorityWinsWhenOverrideIndexedFirst(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/priority/override", "*", false, 10);
  searcher.AddDirectory(L"/virtual/priority/stock", "*", false, 0);

  const hpl::tWString &resolved = searcher.GetFilePath("wanted/foo.dds");
  ASSERT_TRUE_MSG(resolved == L"/virtual/priority/override/foo.dds",
                  "the higher-priority override wins when indexed first");
}

void CheckHigherPriorityWinsWhenOverrideIndexedLast(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/priority/stock", "*", false, 0);
  searcher.AddDirectory(L"/virtual/priority/override", "*", false, 10);

  const hpl::tWString &resolved = searcher.GetFilePath("wanted/foo.dds");
  ASSERT_TRUE_MSG(resolved == L"/virtual/priority/override/foo.dds",
                  "the higher-priority override wins when indexed last");
}

void CheckPriorityBeatsPathScore(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/priority/stock/materials/textures", "*",
                        false, 0);
  searcher.AddDirectory(L"/virtual/priority/override", "*", false, 10);

  int equalCount = -1;
  const hpl::tWString &resolved =
      searcher.GetFilePath("wanted/materials/textures/foo.dds", &equalCount);
  ASSERT_TRUE_MSG(resolved == L"/virtual/priority/override/foo.dds",
                  "priority beats a better matching stock path");
  ASSERT_EQ_MSG(equalCount, 0,
                "the winning priority candidate reports its path score");
}

void CheckDefaultPriorityRegression(int *utest_result) {
  hpl::cFileSearcher bestScore;
  bestScore.AddDirectory(L"/virtual/priority/stock/materials/textures", "*",
                         false);
  bestScore.AddDirectory(L"/virtual/priority/override", "*", false);

  int equalCount = -1;
  const hpl::tWString &resolved =
      bestScore.GetFilePath("wanted/materials/textures/foo.dds", &equalCount);
  ASSERT_TRUE_MSG(resolved ==
                      L"/virtual/priority/stock/materials/textures/foo.dds",
                  "default priorities preserve the best path-score winner");
  ASSERT_TRUE_MSG(equalCount == 2,
                  "default-priority resolution reports the best path score");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher firstA;
  firstA.AddDirectory(L"/virtual/tie/a/textures", "*", false);
  firstA.AddDirectory(L"/virtual/tie/b/textures", "*", false);
  const hpl::tWString &aThenB = firstA.GetFilePath("wanted/textures/foo.dds");
  ASSERT_TRUE_MSG(
      aThenB == L"/virtual/tie/a/textures/foo.dds",
      "default-priority score ties keep the first indexed candidate");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher firstB;
  firstB.AddDirectory(L"/virtual/tie/b/textures", "*", false);
  firstB.AddDirectory(L"/virtual/tie/a/textures", "*", false);
  const hpl::tWString &bThenA = firstB.GetFilePath("wanted/textures/foo.dds");
  ASSERT_TRUE_MSG(bThenA == L"/virtual/tie/b/textures/foo.dds",
                  "default-priority score ties follow the reverse index order");
}

void CheckBareFilenamePriorityAndOrder(int *utest_result) {
  hpl::cFileSearcher prioritized;
  prioritized.AddDirectory(L"/virtual/bare/stock", "*", false, 0);
  prioritized.AddDirectory(L"/virtual/bare/override", "*", false, 10);

  int equalCount = -1;
  const hpl::tWString &overrideResult =
      prioritized.GetFilePath("foo.dds", &equalCount);
  ASSERT_TRUE_MSG(
      overrideResult == L"/virtual/bare/override/foo.dds",
      "a bare filename still prefers the higher-priority candidate");
  ASSERT_TRUE_MSG(equalCount == 0,
                  "a bare filename reports zero path-score matches");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher stockFirst;
  stockFirst.AddDirectory(L"/virtual/bare/stock", "*", false);
  stockFirst.AddDirectory(L"/virtual/bare/override", "*", false);
  equalCount = -1;
  const hpl::tWString &stockResult =
      stockFirst.GetFilePath("foo.dds", &equalCount);
  ASSERT_TRUE_MSG(
      stockResult == L"/virtual/bare/stock/foo.dds",
      "default-priority bare names keep the first indexed candidate");
  ASSERT_TRUE_MSG(equalCount == 0,
                  "default-priority bare names report zero path-score matches");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher overrideFirst;
  overrideFirst.AddDirectory(L"/virtual/bare/override", "*", false);
  overrideFirst.AddDirectory(L"/virtual/bare/stock", "*", false);
  equalCount = -1;
  const hpl::tWString &reverseResult =
      overrideFirst.GetFilePath("foo.dds", &equalCount);
  ASSERT_TRUE_MSG(reverseResult == L"/virtual/bare/override/foo.dds",
                  "default-priority bare names follow the reverse index order");
  ASSERT_TRUE_MSG(equalCount == 0,
                  "reverse-order bare names report zero path-score matches");
}

void CheckEqualNonDefaultPriorityFallback(int *utest_result) {
  hpl::cFileSearcher bestScore;
  bestScore.AddDirectory(L"/virtual/equal/score/loser", "*", false, 100);
  bestScore.AddDirectory(L"/virtual/equal/score/winner/materials", "*", false,
                         100);

  int equalCount = -1;
  const hpl::tWString &resolved =
      bestScore.GetFilePath("wanted/materials/foo.dds", &equalCount);
  ASSERT_TRUE_MSG(resolved == L"/virtual/equal/score/winner/materials/foo.dds",
                  "equal non-default priorities fall back to path score");
  ASSERT_TRUE_MSG(equalCount == 1,
                  "equal non-default priorities report the winning path score");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher firstA;
  firstA.AddDirectory(L"/virtual/tie/a/textures", "*", false, 100);
  firstA.AddDirectory(L"/virtual/tie/b/textures", "*", false, 100);
  const hpl::tWString &aThenB = firstA.GetFilePath("wanted/textures/foo.dds");
  ASSERT_TRUE_MSG(
      aThenB == L"/virtual/tie/a/textures/foo.dds",
      "equal non-default priority ties keep the first indexed candidate");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher firstB;
  firstB.AddDirectory(L"/virtual/tie/b/textures", "*", false, 100);
  firstB.AddDirectory(L"/virtual/tie/a/textures", "*", false, 100);
  const hpl::tWString &bThenA = firstB.GetFilePath("wanted/textures/foo.dds");
  ASSERT_TRUE_MSG(
      bThenA == L"/virtual/tie/b/textures/foo.dds",
      "equal non-default priority ties follow the reverse index order");
}

void CheckReaddingFirstDirectoryKeepsDistinctPaths(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/dup/first", "*", false);
  searcher.AddDirectory(L"/virtual/dup/second", "*", false);
  searcher.AddDirectory(L"/virtual/dup/first", "*", false);

  hpl::tWStringVec paths;
  const size_t count = searcher.GetAllFilePaths("foo.dds", paths);
  int firstCount = 0;
  int secondCount = 0;
  for (const hpl::tWString &path : paths) {
    if (path == L"/virtual/dup/first/foo.dds")
      ++firstCount;
    if (path == L"/virtual/dup/second/foo.dds")
      ++secondCount;
  }

  ASSERT_EQ_MSG(count, 2,
                "re-adding the first directory keeps two matching paths");
  ASSERT_EQ_MSG(paths.size(), 2u,
                "re-adding the first directory keeps two matching paths");
  ASSERT_TRUE_MSG(
      firstCount == 1 && secondCount == 1,
      "re-adding the first directory keeps each matching path once");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  // Re-adding the LAST indexed directory is the case the old find()-based
  // guard missed: find() lands on the earliest equivalent entry, whose path
  // belongs to the other directory, so the guard never fired.
  hpl::cFileSearcher readdLast;
  readdLast.AddDirectory(L"/virtual/dup/first", "*", false);
  readdLast.AddDirectory(L"/virtual/dup/second", "*", false);
  readdLast.AddDirectory(L"/virtual/dup/second", "*", false);

  paths.clear();
  const size_t lastCount = readdLast.GetAllFilePaths("foo.dds", paths);
  firstCount = 0;
  secondCount = 0;
  for (const hpl::tWString &path : paths) {
    if (path == L"/virtual/dup/first/foo.dds")
      ++firstCount;
    if (path == L"/virtual/dup/second/foo.dds")
      ++secondCount;
  }

  ASSERT_EQ_MSG(lastCount, 2,
                "re-adding the last directory keeps two matching paths");
  ASSERT_EQ_MSG(paths.size(), 2u,
                "re-adding the last directory keeps two matching paths");
  ASSERT_TRUE_MSG(firstCount == 1 && secondCount == 1,
                  "re-adding the last directory keeps each matching path once");
}

void CheckReaddingRecursiveDirectoryKeepsCounts(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/dup/repeat", "*", true);

  hpl::tWStringVec paths;
  const size_t initialCount = searcher.GetAllFilePaths("repeat.dds", paths);
  searcher.AddDirectory(L"/virtual/dup/repeat", "*", true);
  searcher.AddDirectory(L"/virtual/dup/repeat", "*", true);

  paths.clear();
  const size_t repeatedCount = searcher.GetAllFilePaths("repeat.dds", paths);
  ASSERT_TRUE_MSG(initialCount == 2,
                  "recursive indexing finds both repeat-tree files");
  ASSERT_EQ_MSG(repeatedCount, 2,
                "re-adding a recursive directory leaves file counts unchanged");
  ASSERT_EQ_MSG(paths.size(), 2u,
                "re-adding a recursive directory leaves file counts unchanged");
}

void CheckReaddingDirectoryRescansForNewFiles(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/rescan", "*", false);
  g_fakeDirectories[L"/virtual/rescan"].files.push_back(L"added.dds");
  searcher.AddDirectory(L"/virtual/rescan", "*", false);

  hpl::tWStringVec paths;
  const size_t addedCount = searcher.GetAllFilePaths("added.dds", paths);
  ASSERT_TRUE_MSG(
      searcher.GetFilePath("added.dds") == L"/virtual/rescan/added.dds",
      "re-adding a directory indexes a file added after the first scan");
  ASSERT_EQ_MSG(addedCount, 1, "the newly indexed file has one matching entry");
  ASSERT_EQ_MSG(paths.size(), 1u,
                "the newly indexed file has one matching entry");
  ASSERT_TRUE_MSG(paths[0] == L"/virtual/rescan/added.dds",
                  "the newly indexed file has one matching entry");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  paths.clear();
  const size_t originalCount = searcher.GetAllFilePaths("original.dds", paths);
  ASSERT_EQ_MSG(originalCount, 1,
                "re-scanning keeps one index entry for the original file");
  ASSERT_EQ_MSG(paths.size(), 1u,
                "re-scanning keeps one index entry for the original file");
  ASSERT_TRUE_MSG(paths[0] == L"/virtual/rescan/original.dds",
                  "re-scanning keeps one index entry for the original file");
}

void CheckReaddingDirectoryKeepsHighestPriority(int *utest_result) {
  hpl::cFileSearcher raisedAfterReadd;
  raisedAfterReadd.AddDirectory(L"/virtual/dup/priority/override", "*", false,
                                0);
  raisedAfterReadd.AddDirectory(L"/virtual/dup/priority/override", "*", false,
                                10);

  hpl::tWStringVec paths;
  const size_t raisedCount = raisedAfterReadd.GetAllFilePaths("foo.dds", paths);
  ASSERT_TRUE_MSG(raisedCount == 1 && paths.size() == 1,
                  "raising a re-added directory keeps one indexed entry");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher raisedAgainstDefault;
  raisedAgainstDefault.AddDirectory(L"/virtual/dup/priority/default", "*",
                                    false);
  raisedAgainstDefault.AddDirectory(L"/virtual/dup/priority/override", "*",
                                    false, 0);
  raisedAgainstDefault.AddDirectory(L"/virtual/dup/priority/override", "*",
                                    false, 10);
  const hpl::tWString &raisedResult =
      raisedAgainstDefault.GetFilePath("foo.dds");
  ASSERT_TRUE_MSG(raisedResult == L"/virtual/dup/priority/override/foo.dds",
                  "a raised re-added priority beats the default candidate");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher raisedAboveCompeting;
  raisedAboveCompeting.AddDirectory(L"/virtual/dup/priority/readd", "*", false,
                                    0);
  raisedAboveCompeting.AddDirectory(L"/virtual/dup/priority/readd", "*", false,
                                    10);
  raisedAboveCompeting.AddDirectory(L"/virtual/dup/priority/competing", "*",
                                    false, 5);
  const hpl::tWString &raisedAboveCompetingResult =
      raisedAboveCompeting.GetFilePath("foo.dds");
  ASSERT_TRUE_MSG(raisedAboveCompetingResult ==
                      L"/virtual/dup/priority/readd/foo.dds",
                  "a re-added higher priority beats a competing priority");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher loweredAfterReadd;
  loweredAfterReadd.AddDirectory(L"/virtual/dup/priority/override", "*", false,
                                 10);
  loweredAfterReadd.AddDirectory(L"/virtual/dup/priority/override", "*", false,
                                 0);

  paths.clear();
  const size_t reverseCount =
      loweredAfterReadd.GetAllFilePaths("foo.dds", paths);
  ASSERT_TRUE_MSG(reverseCount == 1 && paths.size() == 1,
                  "lowering a re-added directory keeps one indexed entry");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher reverseAgainstDefault;
  reverseAgainstDefault.AddDirectory(L"/virtual/dup/priority/default", "*",
                                     false);
  reverseAgainstDefault.AddDirectory(L"/virtual/dup/priority/override", "*",
                                     false, 10);
  reverseAgainstDefault.AddDirectory(L"/virtual/dup/priority/override", "*",
                                     false, 0);
  const hpl::tWString &reverseResult =
      reverseAgainstDefault.GetFilePath("foo.dds");
  ASSERT_TRUE_MSG(reverseResult == L"/virtual/dup/priority/override/foo.dds",
                  "a lower re-added priority cannot displace the highest one");
}

void CheckReaddingDeltaDirectoriesKeepsStackingPaths(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/dup/delta/first", "*", false);
  searcher.AddDirectory(L"/virtual/dup/delta/second", "*", false);
  searcher.AddDirectory(L"/virtual/dup/delta/first", "*", false);
  searcher.AddDirectory(L"/virtual/dup/delta/second", "*", false);

  hpl::tWStringVec paths;
  const size_t count = searcher.GetAllFilePaths("level.map_delta", paths);
  ASSERT_TRUE_MSG(count == 2 && paths.size() == 2,
                  "re-added delta directories keep both stacking paths");
}

struct CustomStoryAsset {
  const char *requestedPath;
  const wchar_t *stockPath;
  const wchar_t *storyPath;
};

void CheckCustomStoryPriorityForAsset(const CustomStoryAsset &asset,
                                      bool storyFirst, int *utest_result) {
  hpl::cFileSearcher searcher;
  if (storyFirst) {
    searcher.AddDirectory(L"/virtual/custom-story/story", "*", true,
                          hpl::klFileSearchCustomStoryPriority);
    searcher.AddDirectory(L"/virtual/custom-story/stock/assets", "*", true);
  } else {
    searcher.AddDirectory(L"/virtual/custom-story/stock/assets", "*", true);
    searcher.AddDirectory(L"/virtual/custom-story/story", "*", true,
                          hpl::klFileSearchCustomStoryPriority);
  }

  int equalCount = -1;
  const hpl::tWString &resolved =
      searcher.GetFilePath(asset.requestedPath, &equalCount);
  ASSERT_TRUE_MSG(resolved == asset.storyPath,
                  "custom-story asset priority beats stock path score");
  ASSERT_TRUE_MSG(equalCount == 1,
                  "custom-story asset still reports its path score");
}

void CheckCustomStoryPriorityRegression(int *utest_result) {
  const CustomStoryAsset assets[] = {
      {"assets/textures/shared.dds",
       L"/virtual/custom-story/stock/assets/textures/shared.dds",
       L"/virtual/custom-story/story/textures/shared.dds"},
      {"assets/meshes/shared.msh",
       L"/virtual/custom-story/stock/assets/meshes/shared.msh",
       L"/virtual/custom-story/story/meshes/shared.msh"},
      {"assets/materials/shared.mat",
       L"/virtual/custom-story/stock/assets/materials/shared.mat",
       L"/virtual/custom-story/story/materials/shared.mat"},
  };

  for (const CustomStoryAsset &asset : assets) {
    CheckCustomStoryPriorityForAsset(asset, false, utest_result);
    if (*utest_result != UTEST_TEST_PASSED)
      return;
    CheckCustomStoryPriorityForAsset(asset, true, utest_result);
    if (*utest_result != UTEST_TEST_PASSED)
      return;

    hpl::cFileSearcher stockOnly;
    stockOnly.AddDirectory(L"/virtual/custom-story/stock/assets", "*", true);
    ASSERT_TRUE_MSG(stockOnly.GetFilePath(asset.requestedPath) ==
                        asset.stockPath,
                    "stock-only lookup keeps the default-priority fallback");
    if (*utest_result != UTEST_TEST_PASSED)
      return;
  }

  hpl::cFileSearcher reRegistered;
  reRegistered.AddDirectory(L"/virtual/custom-story/story", "*", true,
                            hpl::klFileSearchCustomStoryPriority);
  reRegistered.ClearDirectories();
  reRegistered.AddDirectory(L"/virtual/custom-story/stock/assets", "*", true);
  ASSERT_TRUE_MSG(reRegistered.GetFilePath(assets[0].requestedPath) ==
                      assets[0].stockPath,
                  "clearing removes the previously registered custom story");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  reRegistered.ClearDirectories();
  reRegistered.AddDirectory(L"/virtual/custom-story/stock/assets", "*", true);
  reRegistered.AddDirectory(L"/virtual/custom-story/story", "*", true,
                            hpl::klFileSearchCustomStoryPriority);
  ASSERT_TRUE_MSG(reRegistered.GetFilePath(assets[2].requestedPath) ==
                      assets[2].storyPath,
                  "re-registering the custom story restores its priority");
}

void CheckCustomStoryStockABStockLifecycle(int *utest_result) {
  const CustomStoryAsset assets[] = {
      {"assets/textures/shared.dds", L"",
       L"/virtual/custom-story/story-a/textures/shared.dds"},
      {"assets/meshes/shared.msh", L"",
       L"/virtual/custom-story/story-a/meshes/shared.msh"},
      {"assets/materials/shared.mat", L"",
       L"/virtual/custom-story/story-a/materials/shared.mat"},
  };
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/custom-story/stock/assets", "*", true);

  const auto checkStory = [&](const wchar_t *root, const char *label) {
    for (const CustomStoryAsset &asset : assets) {
      const hpl::tWString expected =
          hpl::tWString(root) +
          (asset.requestedPath[7] == 't' ? L"/textures/shared.dds"
           : asset.requestedPath[7] == 'm' && asset.requestedPath[8] == 'e'
               ? L"/meshes/shared.msh"
               : L"/materials/shared.mat");
      ASSERT_TRUE_MSG(searcher.GetFilePath(asset.requestedPath) == expected,
                      label);
      if (*utest_result != UTEST_TEST_PASSED)
        return;
    }
  };

  searcher.AddDirectory(L"/virtual/custom-story/story-a", "*", true,
                        hpl::klFileSearchCustomStoryPriority, "story");
  checkStory(L"/virtual/custom-story/story-a", "story A wins after stock");
  if (*utest_result != UTEST_TEST_PASSED)
    return;
  searcher.RemoveDirectoryScope("story");
  ASSERT_TRUE_MSG(searcher.GetFilePath("assets/textures/shared.dds") ==
                      L"/virtual/custom-story/stock/assets/textures/shared.dds",
                  "removing story A restores stock");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  searcher.AddDirectory(L"/virtual/custom-story/story-b", "*", true,
                        hpl::klFileSearchCustomStoryPriority, "story");
  checkStory(L"/virtual/custom-story/story-b", "story B wins after A removal");
  if (*utest_result != UTEST_TEST_PASSED)
    return;
  searcher.RemoveDirectoryScope("story");
  ASSERT_TRUE_MSG(
      searcher.GetFilePath("assets/materials/shared.mat") ==
          L"/virtual/custom-story/stock/assets/materials/shared.mat",
      "removing story B restores stock");
}

void CheckScopedStockABStockLifecycle(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/scopes/stock", "*", true);
  searcher.AddDirectory(L"/virtual/scopes/a", "*", true, 10, "A");
  searcher.AddDirectory(L"/virtual/scopes/b", "*", true, 20, "B");

  ASSERT_TRUE_MSG(searcher.GetFilePath("textures/shared.dds") ==
                      L"/virtual/scopes/b/textures/shared.dds",
                  "scope B wins same-named textures");
  ASSERT_TRUE_MSG(searcher.GetFilePath("meshes/shared.msh") ==
                      L"/virtual/scopes/b/meshes/shared.msh",
                  "scope B wins same-named meshes");
  ASSERT_TRUE_MSG(searcher.GetFilePath("materials/shared.mat") ==
                      L"/virtual/scopes/b/materials/shared.mat",
                  "scope B wins same-named materials");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::tWStringVec paths;
  const size_t allCount = searcher.GetAllFilePaths("level.map_delta", paths);
  ASSERT_EQ_MSG(allCount, 3,
                "recursive scoped indexing returns every delta path");
  ASSERT_EQ_MSG(paths.size(), 3u,
                "recursive scoped indexing returns every delta path");
  ASSERT_TRUE_MSG(
      paths[0] == L"/virtual/scopes/stock/recursive/level.map_delta" &&
          paths[1] == L"/virtual/scopes/a/recursive/level.map_delta" &&
          paths[2] == L"/virtual/scopes/b/recursive/level.map_delta",
      "recursive delta paths retain registration order");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  searcher.RemoveDirectoryScope("B");
  ASSERT_TRUE_MSG(searcher.GetFilePath("textures/shared.dds") ==
                      L"/virtual/scopes/a/textures/shared.dds",
                  "removing B restores scope A");
  ASSERT_TRUE_MSG(searcher.GetFilePath("meshes/shared.msh") ==
                      L"/virtual/scopes/a/meshes/shared.msh",
                  "removing B restores A meshes");
  ASSERT_TRUE_MSG(searcher.GetFilePath("materials/shared.mat") ==
                      L"/virtual/scopes/a/materials/shared.mat",
                  "removing B restores A materials");
  if (*utest_result != UTEST_TEST_PASSED)
    return;
  searcher.RemoveDirectoryScope("B");
  searcher.RemoveDirectoryScope("A");
  ASSERT_TRUE_MSG(searcher.GetFilePath("textures/shared.dds") ==
                      L"/virtual/scopes/stock/textures/shared.dds",
                  "removing A restores stock textures");
  ASSERT_TRUE_MSG(searcher.GetFilePath("meshes/shared.msh") ==
                      L"/virtual/scopes/stock/meshes/shared.msh",
                  "removing A restores stock meshes");
  ASSERT_TRUE_MSG(searcher.GetFilePath("materials/shared.mat") ==
                      L"/virtual/scopes/stock/materials/shared.mat",
                  "removing A restores permanent stock");
  ASSERT_TRUE_MSG(searcher.GetAllFilePaths("level.map_delta", paths) == 1,
                  "scoped recursive delta paths are removed");
  ASSERT_TRUE_MSG(
      searcher.GetFilePath("stock-nested.dds") ==
          L"/virtual/scopes/stock/recursive/nested/stock-nested.dds",
      "permanent recursive files remain after scope removal");
}

void CheckScopedOverlapPriorityRestoration(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/scopes/overlap/parent", "*", true, -10);
  searcher.AddDirectory(L"/virtual/scopes/overlap/exact", "*", false, -20);
  searcher.AddDirectory(L"/virtual/scopes/overlap/competing", "*", false, -5);
  searcher.AddDirectory(L"/virtual/scopes/overlap/parent/child", "*", false, 30,
                        "A");

  ASSERT_TRUE_MSG(searcher.GetFilePath("parent/child/overlap.dds") ==
                      L"/virtual/scopes/overlap/parent/child/overlap.dds",
                  "exact child registration wins overlap");
  ASSERT_TRUE_MSG(searcher.GetFilePath("overlap.dds") ==
                      L"/virtual/scopes/overlap/parent/child/overlap.dds",
                  "positive scoped priority beats negative permanent priority");
  if (*utest_result != UTEST_TEST_PASSED)
    return;
  searcher.RemoveDirectoryScope("A");
  const hpl::tFilePathMap &files = searcher.GetAllFiles();
  bool parentPriority = false;
  for (const auto &file : files) {
    if (file.second.msPath ==
        L"/virtual/scopes/overlap/parent/child/overlap.dds")
      parentPriority = file.second.mlPriority == -10;
  }
  ASSERT_TRUE_MSG(parentPriority,
                  "scope demotion restores exact negative priority");
  ASSERT_TRUE_MSG(searcher.GetFilePath("overlap.dds") ==
                      L"/virtual/scopes/overlap/competing/overlap.dds",
                  "demoted permanent overlap yields to competing priority");
  ASSERT_TRUE_MSG(searcher.GetFilePath("parent/child/overlap.dds") ==
                      L"/virtual/scopes/overlap/competing/overlap.dds",
                  "priority still beats path score after demotion");
}

void CheckPermanentAddedAfterScopeAndSharedScopeRemoval(int *utest_result) {
  hpl::cFileSearcher afterScope;
  afterScope.AddDirectory(L"/virtual/scopes/overlap/parent/child", "*", false,
                          30, "A");
  afterScope.AddDirectory(L"/virtual/scopes/overlap/parent", "*", true, -10);
  afterScope.AddDirectory(L"/virtual/scopes/overlap/competing", "*", false, -5);
  afterScope.RemoveDirectoryScope("A");
  bool exactPriority = false;
  for (const auto &file : afterScope.GetAllFiles())
    if (file.second.msPath ==
        L"/virtual/scopes/overlap/parent/child/overlap.dds")
      exactPriority = file.second.mlPriority == -10;
  ASSERT_TRUE_MSG(exactPriority,
                  "permanent added after scope retains negative priority");
  ASSERT_TRUE_MSG(afterScope.GetFilePath("overlap.dds") ==
                      L"/virtual/scopes/overlap/competing/overlap.dds",
                  "late permanent registration demotes scoped overlap");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  for (const char *first : {"A", "B"}) {
    const char *second = first[0] == 'A' ? "B" : "A";
    hpl::cFileSearcher shared;
    shared.AddDirectory(L"/virtual/scopes/overlap/exact", "*", false, 10,
                        first);
    shared.AddDirectory(L"/virtual/scopes/overlap/exact", "*", false, 20,
                        second);
    shared.RemoveDirectoryScope(first);
    ASSERT_TRUE_MSG(shared.GetFilePath("overlap.dds") ==
                        L"/virtual/scopes/overlap/exact/overlap.dds",
                    "shared exact file survives first scope removal");
    if (*utest_result != UTEST_TEST_PASSED)
      return;
    shared.RemoveDirectoryScope(second);
    ASSERT_TRUE_MSG(shared.GetFilePath("overlap.dds").empty(),
                    "shared exact file is removed after both scopes");
    if (*utest_result != UTEST_TEST_PASSED)
      return;
  }
  return;
}

void CheckScopedRepeatedPriorityAndTieOrder(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/tie/a/textures", "*", false);
  searcher.AddDirectory(L"/virtual/tie/b/textures", "*", false);
  searcher.AddDirectory(L"/virtual/tie/a/textures", "*", false, 10, "A");
  searcher.AddDirectory(L"/virtual/tie/b/textures", "*", false, 10, "A");
  searcher.AddDirectory(L"/virtual/tie/a/textures", "*", false, 30, "A");
  searcher.AddDirectory(L"/virtual/tie/a/textures", "*", false, 5, "A");
  searcher.AddDirectory(L"/virtual/tie/a/textures", "*", false, 20, "B");
  ASSERT_TRUE_MSG(searcher.GetFilePath("wanted/textures/foo.dds") ==
                      L"/virtual/tie/a/textures/foo.dds",
                  "repeated scope raises retain the first-index tie order");
  if (*utest_result != UTEST_TEST_PASSED)
    return;
  searcher.RemoveDirectoryScope("A");
  ASSERT_TRUE_MSG(searcher.GetFilePath("wanted/textures/foo.dds") ==
                      L"/virtual/tie/a/textures/foo.dds",
                  "removing one scope restores the other scope priority");
  if (*utest_result != UTEST_TEST_PASSED)
    return;
  searcher.RemoveDirectoryScope("B");
  ASSERT_TRUE_MSG(searcher.GetFilePath("wanted/textures/foo.dds") ==
                      L"/virtual/tie/a/textures/foo.dds",
                  "removing repeated scopes retains index order");
}

void CheckExternalAndStoryLanguageRefresh(int *utest_result) {
  hpl::cFileSearcher external;
  external.AddDirectory(L"/external/permanent", "*", false, -10);
  external.AddDirectory(L"/external/scoped", "*", false, 10, "external");
  ASSERT_TRUE_MSG(external.GetFilePath("shared.ext") ==
                      L"/external/scoped/shared.ext",
                  "external scoped overlap wins");
  if (*utest_result != UTEST_TEST_PASSED)
    return;
  external.RemoveDirectoryScope("external");
  ASSERT_TRUE_MSG(external.GetFilePath("shared.ext") ==
                      L"/external/permanent/shared.ext",
                  "external permanent overlap is restored");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  hpl::cFileSearcher language;
  language.AddDirectory(L"/virtual/story-language/permanent", "*", false, -10);
  language.AddDirectory(L"/virtual/story-language/story", "*", false, 1,
                        "story-language");
  ASSERT_TRUE_MSG(language.GetFilePath("menu.lang") ==
                      L"/virtual/story-language/story/menu.lang",
                  "story-language scope wins independently of story root");
  if (*utest_result != UTEST_TEST_PASSED)
    return;
  language.RemoveDirectoryScope("story-language");
  ASSERT_TRUE_MSG(language.GetFilePath("menu.lang") ==
                      L"/virtual/story-language/permanent/menu.lang",
                  "story-language refresh restores permanent language");
}

void CheckMultipleScopesRefreshAndClear(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/virtual/scopes/stock", "*", true);
  searcher.AddDirectory(L"/virtual/scopes/permanent/language", "*", false);
  searcher.AddDirectory(L"/virtual/scopes/permanent/config", "*", false);
  searcher.AddDirectory(L"/virtual/scopes/a", "*", true, 10, "A");
  searcher.AddDirectory(L"/virtual/scopes/b", "*", true, 20, "B");

  searcher.RemoveDirectoryScope("");
  ASSERT_TRUE_MSG(searcher.GetFilePath("menu.lang") ==
                          L"/virtual/scopes/permanent/language/menu.lang" &&
                      searcher.GetFilePath("settings.cfg") ==
                          L"/virtual/scopes/permanent/config/settings.cfg",
                  "empty scope is permanent and removal is a no-op");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  searcher.RemoveDirectoryScope("A");
  ASSERT_TRUE_MSG(searcher.GetFilePath("a-only.dds").empty(),
                  "removing one scope leaves no files from that scope");
  ASSERT_TRUE_MSG(searcher.GetFilePath("b-only.dds") ==
                      L"/virtual/scopes/b/recursive/b-only.dds",
                  "removing one scope preserves another scope");
  ASSERT_TRUE_MSG(
      searcher.GetFilePath("menu.lang") ==
              L"/virtual/scopes/permanent/language/menu.lang" &&
          searcher.GetFilePath("settings.cfg") ==
              L"/virtual/scopes/permanent/config/settings.cfg",
      "scope refresh preserves language and config analog registrations");
  if (*utest_result != UTEST_TEST_PASSED)
    return;

  searcher.AddDirectory(L"/virtual/scopes/a", "*", true, 10, "A");
  ASSERT_TRUE_MSG(searcher.GetFilePath("a-only.dds") ==
                      L"/virtual/scopes/a/recursive/a-only.dds",
                  "re-adding a scope refreshes its recursive files");
  if (*utest_result != UTEST_TEST_PASSED)
    return;
  searcher.ClearDirectories();
  searcher.RemoveDirectoryScope("");
  ASSERT_TRUE_MSG(searcher.GetFilePath("menu.lang").empty() &&
                      searcher.GetFilePath("shared.dds").empty(),
                  "ClearDirectories resets permanent and scoped registrations");
}

} // namespace

namespace hpl {

void cPlatform::FindFilesInDir(tWStringList &alstStrings,
                               const tWString &asDir,
                               const tWString & /*asMask*/, bool /*abAddHidden*/) {
  const FakeDirectory *directory = FindFakeDirectory(asDir);
  if (directory != nullptr)
    alstStrings.insert(alstStrings.end(), directory->files.begin(),
                       directory->files.end());
}

void cPlatform::FindFoldersInDir(tWStringList &alstStrings,
                                 const tWString &asDir,
                                 bool /*abAddHidden*/, bool /*abAddUpFolder*/) {
  const FakeDirectory *directory = FindFakeDirectory(asDir);
  if (directory != nullptr)
    alstStrings.insert(alstStrings.end(), directory->folders.begin(),
                       directory->folders.end());
}

tWString cPlatform::GetFullFilePath(const tWString &asFilePath) {
  return Normalize(asFilePath);
}

} // namespace hpl

struct FileSearcherFixture {};

UTEST_F_SETUP(FileSearcherFixture) {
  SetUpFakeTree();
}

UTEST_F_TEARDOWN(FileSearcherFixture) {}

UTEST_F(FileSearcherFixture, DepthAndAdjacentNameResolution) {
  CheckDepthAndAdjacentNameResolution(utest_result);
}

UTEST_F(FileSearcherFixture, TieBreakOrder) {
  CheckTieBreakOrder(utest_result);
}

UTEST_F(FileSearcherFixture, MissingAndSingleCandidate) {
  CheckMissingAndSingleCandidate(utest_result);
}

UTEST_F(FileSearcherFixture, HigherPriorityOverrideIndexedFirst) {
  CheckHigherPriorityWinsWhenOverrideIndexedFirst(utest_result);
}

UTEST_F(FileSearcherFixture, HigherPriorityOverrideIndexedLast) {
  CheckHigherPriorityWinsWhenOverrideIndexedLast(utest_result);
}

UTEST_F(FileSearcherFixture, PriorityBeatsPathScore) {
  CheckPriorityBeatsPathScore(utest_result);
}

UTEST_F(FileSearcherFixture, DefaultPriorityRegression) {
  CheckDefaultPriorityRegression(utest_result);
}

UTEST_F(FileSearcherFixture, BareFilenamePriorityAndOrder) {
  CheckBareFilenamePriorityAndOrder(utest_result);
}

UTEST_F(FileSearcherFixture, EqualNonDefaultPriorityFallback) {
  CheckEqualNonDefaultPriorityFallback(utest_result);
}

UTEST_F(FileSearcherFixture, ReaddingFirstDirectoryKeepsDistinctPaths) {
  CheckReaddingFirstDirectoryKeepsDistinctPaths(utest_result);
}

UTEST_F(FileSearcherFixture, ReaddingRecursiveDirectoryKeepsCounts) {
  CheckReaddingRecursiveDirectoryKeepsCounts(utest_result);
}

UTEST_F(FileSearcherFixture, ReaddingDirectoryRescansForNewFiles) {
  CheckReaddingDirectoryRescansForNewFiles(utest_result);
}

UTEST_F(FileSearcherFixture, ReaddingDirectoryKeepsHighestPriority) {
  CheckReaddingDirectoryKeepsHighestPriority(utest_result);
}

UTEST_F(FileSearcherFixture, ReaddingDeltaDirectoriesKeepsStackingPaths) {
  CheckReaddingDeltaDirectoriesKeepsStackingPaths(utest_result);
}

UTEST_F(FileSearcherFixture, CustomStoryPriorityRegression) {
  CheckCustomStoryPriorityRegression(utest_result);
}

UTEST_F(FileSearcherFixture, CustomStoryStockABStockLifecycle) {
  CheckCustomStoryStockABStockLifecycle(utest_result);
}

UTEST_F(FileSearcherFixture, ScopedStockABStockLifecycle) {
  CheckScopedStockABStockLifecycle(utest_result);
}

UTEST_F(FileSearcherFixture, ScopedOverlapPriorityRestoration) {
  CheckScopedOverlapPriorityRestoration(utest_result);
}

UTEST_F(FileSearcherFixture, PermanentAddedAfterScopeAndSharedScopeRemoval) {
  CheckPermanentAddedAfterScopeAndSharedScopeRemoval(utest_result);
}

UTEST_F(FileSearcherFixture, ScopedRepeatedPriorityAndTieOrder) {
  CheckScopedRepeatedPriorityAndTieOrder(utest_result);
}

UTEST_F(FileSearcherFixture, ExternalAndStoryLanguageRefresh) {
  CheckExternalAndStoryLanguageRefresh(utest_result);
}

UTEST_F(FileSearcherFixture, MultipleScopesRefreshAndClear) {
  CheckMultipleScopesRefreshAndClear(utest_result);
}

UTEST_MAIN();
