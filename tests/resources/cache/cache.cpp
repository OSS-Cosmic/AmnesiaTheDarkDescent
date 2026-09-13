// CPU regression for the real FileSearcher/ResourceManager/SharedResourceHandle seams.
// Fake parents retain managed textures; aggregate snapshots model production keys
// without constructing GPU-backed MaterialManager or TextureManager resources.
#include "resources/FileSearcher.h"
#include "resources/ResourceBase.h"
#include "resources/ResourceManager.h"
#include "system/Platform.h"
#include "system/String.h"
#include "utest.h"

#include <cstdio>
#include <map>
#include <string>

namespace {
struct FakeDirectory { hpl::tWStringVec files; };
std::map<hpl::tWString, FakeDirectory> g_files;
int g_destroyed = 0;

// These fake paths are ASCII. Keep converted strings alive while utest prints
// actual and expected paths, including the differing cube face or frame.
#define WSTREQ(x, y, msg) do { \
  const auto actualPath = hpl::cString::To8Char(x); \
  const auto expectedPath = hpl::cString::To8Char(y); \
  ASSERT_STREQ_MSG(actualPath.c_str(), expectedPath.c_str(), msg); \
} while (0)

void File(const char *root, const char *name) {
  g_files[hpl::cString::To16Char(root)].files.push_back(hpl::cString::To16Char(name));
}

class FakeResource final : public hpl::iResourceBase {
public:
  FakeResource(const hpl::tString &name, const hpl::tWString &key,
               const hpl::tWStringVec &deps = {}) : iResourceBase(name, key, 0), dependencies(deps) {}
  ~FakeResource() override { ++g_destroyed; }
  bool Reload() override { return true; }
  void Unload() override {}
  void Destroy() override {}
  hpl::tWStringVec dependencies;
  hpl::SharedResourceHandle<FakeResource> texture;
};
using Handle = hpl::SharedResourceHandle<FakeResource>;

class FakeManager final : public hpl::iResourceManager {
public:
  explicit FakeManager(hpl::cFileSearcher *searcher, bool dependent = false)
      : iResourceManager(searcher, nullptr, nullptr, dependent) {}
  using iResourceManager::FindLoadedResource;
  void Register(FakeResource *resource, bool dependent = false, bool addToSet = true) { AddResource(resource, false, addToSet, dependent); }
  size_t Count() const { return m_mapResources.size(); }
  void Unload(hpl::iResourceBase *) override {}
};

const char *const kFaces[] = {"_pos_x", "_neg_x", "_pos_y", "_neg_y", "_pos_z", "_neg_z"};
struct World {
  hpl::cFileSearcher searcher;
  FakeManager textures, parents, aggregates;
  World() : textures(&searcher), parents(&searcher, true), aggregates(&searcher) {}
  size_t Count() const { return textures.Count() + parents.Count() + aggregates.Count(); }

  Handle Texture() {
    hpl::tWString path;
    auto *old = textures.FindLoadedResource("shared.dds", path);
    if (old) return hpl::AcquireResource<FakeResource>(&textures, old);
    if (path.empty()) return {};
    auto *fresh = new FakeResource("shared.dds", path);
    textures.Register(fresh);
    return hpl::AcquireResource<FakeResource>(&textures, fresh);
  }
  Handle Parent() {
    hpl::tWString path;
    auto *old = parents.FindLoadedResource("shared.mat", path);
    if (old) return hpl::AcquireResource<FakeResource>(&parents, old);
    if (path.empty()) return {};
    auto texture = Texture();
    if (!texture) return {};
    auto *fresh = new FakeResource("shared.mat", path);
    fresh->texture = std::move(texture);
    parents.Register(fresh);
    return hpl::AcquireResource<FakeResource>(&parents, fresh);
  }
  Handle Aggregate(const hpl::tWString &key, const hpl::tWStringVec &deps) {
    if (auto *old = aggregates.GetResource(key, true))
      return hpl::AcquireResource<FakeResource>(&aggregates, old);
    auto *fresh = new FakeResource("aggregate", key, deps);
    aggregates.Register(fresh, true);
    return hpl::AcquireResource<FakeResource>(&aggregates, fresh);
  }
  Handle Cube() {
    hpl::tWStringVec faces;
    for (const char *suffix : kFaces) {
      auto path = searcher.GetFilePath(std::string("sky") + suffix + ".dds");
      if (path.empty()) return {};
      faces.push_back(path);
    }
    // Same requested-name key as the production non-DDS aggregate path.
    return Aggregate(L"sky", faces);
  }
  Handle Animation() {
    auto first = searcher.GetFilePath("anim01.png");
    if (first.empty()) return {};
    hpl::tWStringVec frames;
    for (int n = 1; ; ++n) {
      char file[32]; std::snprintf(file, sizeof(file), "anim%02d.png", n);
      auto path = searcher.GetFilePath(file);
      if (path.empty()) break;
      frames.push_back(path);
    }
    // Deliberately unchanged when only a later frame is overridden.
    return frames.size() < 2 ? Handle() : Aggregate(hpl::cString::GetFilePathW(first) + L"anim.png", frames);
  }
};

void SetUp() {
  g_files.clear(); g_destroyed = 0;
  for (const char *file : {"shared.mat", "shared.dds", "anim01.png", "anim02.png"})
    File("/cache/stock", file);
  for (const char *suffix : kFaces)
    File("/cache/stock", (std::string("sky") + suffix + ".dds").c_str());
  // Platform directory listings return basenames, as the real FileSearcher expects.
  for (const char *root : {"/cache/A", "/cache/B"}) {
    File(root, "shared.dds"); File(root, "anim02.png"); File(root, "sky_pos_x.dds");
  }
}

struct Snapshot {
  Handle parent, cube, anim;
  explicit Snapshot(World &world) : parent(world.Parent()), cube(world.Cube()), anim(world.Animation()) {}
  void CheckPaths(int *utest_result, const wchar_t *root) const {
    ASSERT_TRUE_MSG(parent, "material resource exists");
    ASSERT_TRUE_MSG(parent->texture, "material owned texture exists");
    ASSERT_TRUE_MSG(cube, "cube resource exists");
    ASSERT_TRUE_MSG(anim, "animation resource exists");
    hpl::tWString prefix(root);
    hpl::tWStringVec faces;
    for (size_t i = 0; i < 6; ++i)
      faces.push_back((i == 0 ? prefix : L"/cache/stock") + L"/sky" + hpl::cString::To16Char(kFaces[i]) + L".dds");
    WSTREQ(parent->GetFullPath(), hpl::tWString(L"/cache/stock/shared.mat"), "material root path");
    WSTREQ(parent->texture->GetFullPath(), prefix + L"/shared.dds", "texture root path");
    ASSERT_EQ_MSG(cube->dependencies.size(), faces.size(), "cube face path count");
    for (size_t i = 0; i < faces.size(); ++i)
      WSTREQ(cube->dependencies[i], faces[i], "cube face path");
    const hpl::tWStringVec frames{L"/cache/stock/anim01.png", prefix + L"/anim02.png"};
    ASSERT_EQ_MSG(anim->dependencies.size(), frames.size(), "animation frame path count");
    for (size_t i = 0; i < frames.size(); ++i)
      WSTREQ(anim->dependencies[i], frames[i], "animation frame path");
  }
  void Reuses(int *utest_result, World &world) const {
    auto currentParent = world.Parent();
    ASSERT_EQ_MSG(currentParent.Get(), parent.Get(), "same generation reuses parent");
    currentParent = {};
    auto currentCube = world.Cube();
    ASSERT_EQ_MSG(currentCube.Get(), cube.Get(), "same generation reuses cube");
    currentCube = {};
    auto currentAnimation = world.Animation();
    ASSERT_EQ_MSG(currentAnimation.Get(), anim.Get(), "same generation reuses animation");
    currentAnimation = {};
  }
  void Distinct(int *utest_result, const Snapshot &other) const {
    ASSERT_NE_MSG(parent.Get(), other.parent.Get(), "new context creates fresh parent");
    ASSERT_NE_MSG(cube.Get(), other.cube.Get(), "new context creates fresh cube");
    ASSERT_NE_MSG(anim.Get(), other.anim.Get(), "new context creates fresh animation");
  }
};

void CheckCacheGenerations(int *utest_result) {
  World world;
  world.searcher.AddDirectory(L"/cache/stock", "*", false, 0, "stock");
  Snapshot stock(world);
  stock.CheckPaths(utest_result, L"/cache/stock");
  if (*utest_result != UTEST_TEST_PASSED) return;
  stock.Reuses(utest_result, world);
  if (*utest_result != UTEST_TEST_PASSED) return;
  auto direct = world.Texture();
  world.searcher.AddDirectory(L"/cache/A", "*", false, 10, "story-a");
  Snapshot a(world);
  a.CheckPaths(utest_result, L"/cache/A");
  if (*utest_result != UTEST_TEST_PASSED) return;
  a.Reuses(utest_result, world);
  if (*utest_result != UTEST_TEST_PASSED) return;
  a.Distinct(utest_result, stock);
  if (*utest_result != UTEST_TEST_PASSED) return;
  world.searcher.RemoveDirectoryScope("story-a");
  world.searcher.AddDirectory(L"/cache/B", "*", false, 10, "story-b");
  Snapshot b(world);
  b.CheckPaths(utest_result, L"/cache/B");
  if (*utest_result != UTEST_TEST_PASSED) return;
  b.Reuses(utest_result, world);
  if (*utest_result != UTEST_TEST_PASSED) return;
  b.Distinct(utest_result, a);
  if (*utest_result != UTEST_TEST_PASSED) return;
  world.searcher.RemoveDirectoryScope("story-b");
  Snapshot restored(world);
  restored.CheckPaths(utest_result, L"/cache/stock");
  if (*utest_result != UTEST_TEST_PASSED) return;
  restored.Reuses(utest_result, world);
  if (*utest_result != UTEST_TEST_PASSED) return;
  restored.Distinct(utest_result, stock);
  if (*utest_result != UTEST_TEST_PASSED) return;
  restored.Distinct(utest_result, a);
  if (*utest_result != UTEST_TEST_PASSED) return;
  restored.Distinct(utest_result, b);
  if (*utest_result != UTEST_TEST_PASSED) return;
  stock.CheckPaths(utest_result, L"/cache/stock");
  if (*utest_result != UTEST_TEST_PASSED) return;
  a.CheckPaths(utest_result, L"/cache/A");
  if (*utest_result != UTEST_TEST_PASSED) return;
  b.CheckPaths(utest_result, L"/cache/B");
  if (*utest_result != UTEST_TEST_PASSED) return;
  EXPECT_EQ_MSG(restored.parent->texture.Get(), direct.Get(), "independent stock texture reuses path identity");
  auto directAgain = world.Texture();
  EXPECT_EQ_MSG(directAgain.Get(), direct.Get(), "direct stock texture reuses path identity");
  directAgain = {};
  const uint64_t oldGeneration = stock.parent->GetResolutionGeneration();
  world.parents.Register(stock.parent.Get());
  world.parents.Register(stock.parent.Get(), false, false);
  EXPECT_EQ_MSG(world.Count(), size_t(15), "all old handles live without duplicate map entries");
  EXPECT_EQ_MSG(g_destroyed, 0, "all old handles live without duplicate map entries");
  EXPECT_EQ_MSG(stock.parent->GetResolutionGeneration(), oldGeneration, "duplicate old-pointer registration cannot restamp");
  restored.Reuses(utest_result, world);
  if (*utest_result != UTEST_TEST_PASSED) return;

  direct = {};
  a.parent = {}; // Frees A parent and its exclusively owned texture.
  EXPECT_EQ_MSG(world.Count(), size_t(13), "releasing old parent frees only its dependency");
  EXPECT_EQ_MSG(g_destroyed, 2, "releasing old parent frees only its dependency");
  restored.Reuses(utest_result, world);
  if (*utest_result != UTEST_TEST_PASSED) return;
  restored.cube = {}; stock.anim = {}; b.cube = {};
  EXPECT_EQ_MSG(world.Count(), size_t(10), "mixed generation aggregate frees erase exact pointers");
  EXPECT_EQ_MSG(g_destroyed, 5, "mixed generation aggregate frees erase exact pointers");
  EXPECT_EQ_MSG(world.aggregates.GetResource(L"sky", true), nullptr, "freed current cube cannot fall back to an old generation");
  stock.parent = {};
  EXPECT_EQ_MSG(world.Count(), size_t(9), "restored parent keeps shared stock texture alive");
  EXPECT_EQ_MSG(g_destroyed, 6, "restored parent keeps shared stock texture alive");
  EXPECT_EQ_MSG(restored.parent->texture->GetReferenceCount(), size_t(1), "restored parent keeps shared stock texture alive");
  b.parent = {}; restored.parent = {};
  stock.cube = {}; a.cube = {}; a.anim = {}; b.anim = {}; restored.anim = {};
  EXPECT_EQ_MSG(world.Count(), size_t(0), "all handles release exact entries and dependencies without DestroyAll");
  EXPECT_EQ_MSG(g_destroyed, 15, "all handles release exact entries and dependencies without DestroyAll");
}

void CheckMixedPolicies(int *utest_result) {
  hpl::cFileSearcher searcher;
  searcher.AddDirectory(L"/cache/stock", "*", false, 0, "stock");
  FakeManager manager(&searcher);
  auto *plain = new FakeResource("shared.mat", L"/cache/stock/shared.mat");
  manager.Register(plain);
  auto plainHandle = hpl::AcquireResource<FakeResource>(&manager, plain);
  EXPECT_EQ_MSG(manager.GetResource(plain->GetFullPath(), true), nullptr, "dependent lookup excludes independent same-path resource");
  auto *dependent = new FakeResource("shared.mat", plain->GetFullPath());
  manager.Register(dependent, true);
  auto dependentHandle = hpl::AcquireResource<FakeResource>(&manager, dependent);
  EXPECT_EQ_MSG(manager.GetResource(plain->GetFullPath()), plain, "independent lookup selects independent resource");
  EXPECT_EQ_MSG(manager.GetResource(plain->GetFullPath(), true), dependent, "dependent lookup selects dependent resource");
  plainHandle = {};
  EXPECT_EQ_MSG(manager.GetResource(dependent->GetFullPath()), nullptr, "independent lookup excludes dependent same-path resource");
  dependentHandle = {};
  EXPECT_EQ_MSG(manager.Count(), size_t(0), "mixed policy entries release independently");
  EXPECT_EQ_MSG(g_destroyed, 2, "mixed policy entries release independently");
}

void CheckSearcherInvalidation(int *utest_result) {
  hpl::cFileSearcher searcher;
  const uint64_t initial = searcher.GetResolutionGeneration();
  searcher.AddDirectory(L"/cache/stock", "*", false, 0, "stock"); const uint64_t added = searcher.GetResolutionGeneration();
  searcher.AddDirectory(L"/cache/stock", "*", false, 0, "stock");
  EXPECT_EQ_MSG(searcher.GetResolutionGeneration(), added, "identical add is a no-op");
  searcher.AddDirectory(L"/cache/stock", "*", false, -1, "stock");
  EXPECT_EQ_MSG(searcher.GetResolutionGeneration(), added, "lower-priority add is a no-op");
  searcher.RemoveDirectoryScope("unknown"); searcher.RemoveDirectoryScope("");
  EXPECT_EQ_MSG(searcher.GetResolutionGeneration(), added, "unknown and empty removal are no-ops");
  searcher.AddDirectory(L"/cache/stock", "*", false, 5, "stock"); const uint64_t raised = searcher.GetResolutionGeneration();
  EXPECT_GT_MSG(raised, added, "priority raise invalidates resolution");
  searcher.RemoveDirectoryScope("stock"); const uint64_t removed = searcher.GetResolutionGeneration();
  EXPECT_GT_MSG(removed, raised, "scope removal invalidates resolution");
  searcher.ClearDirectories(); EXPECT_EQ_MSG(searcher.GetResolutionGeneration(), removed, "clear empty is a no-op");
  searcher.AddDirectory(L"/cache/stock", "*", false); const uint64_t populated = searcher.GetResolutionGeneration(); searcher.ClearDirectories();
  EXPECT_GT_MSG(searcher.GetResolutionGeneration(), populated, "clear populated invalidates resolution");
  EXPECT_LT_MSG(initial, added, "first directory add invalidates resolution");
}
} // namespace

namespace hpl {
void cPlatform::FindFilesInDir(tWStringList &out, const tWString &dir, const tWString &, bool) {
  auto it = g_files.find(dir); if (it != g_files.end()) out.insert(out.end(), it->second.files.begin(), it->second.files.end());
}
void cPlatform::FindFoldersInDir(tWStringList &, const tWString &, bool, bool) {}
tWString cPlatform::GetFullFilePath(const tWString &path) { return path; }
unsigned long cPlatform::GetApplicationTime() { return 1; }
void Log(const char *, ...) {}
void Warning(const char *, ...) {}
} // namespace hpl

struct ResourceCacheFixture {};

UTEST_F_SETUP(ResourceCacheFixture) {
  (void)utest_result;
  SetUp();
}

UTEST_F_TEARDOWN(ResourceCacheFixture) {
  (void)utest_result;
  (void)utest_fixture;
}

UTEST_F(ResourceCacheFixture, CheckCacheGenerations) {
  CheckCacheGenerations(utest_result);
}

UTEST_F(ResourceCacheFixture, CheckMixedPolicies) {
  CheckMixedPolicies(utest_result);
}

UTEST_F(ResourceCacheFixture, CheckSearcherInvalidation) {
  CheckSearcherInvalidation(utest_result);
}

UTEST_MAIN();
