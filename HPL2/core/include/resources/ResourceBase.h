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

#ifndef HPL_RESOURCEBASE_H
#define HPL_RESOURCEBASE_H

#include "system/LowLevelSystem.h"
#include "system/SystemTypes.h"
#include "resources/ResourceManager.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <time.h>
#include <utility>
#include <variant>

namespace hpl {
class iResourceManager;

class iResourceBase {
public:
  iResourceBase(const tString &asName, const tWString &asFullPath,
                unsigned long alPrio);

  virtual ~iResourceBase();

  /**
   * virtual bool Reload()=0;
   * \return true is reload was succesful, else false.
   */
  virtual bool Reload() = 0;

  /**
   * Free most the memory, save info to get started again.
   */
  virtual void Unload() = 0;

  /**
   * Free all memory.
   */
  virtual void Destroy() = 0;

  const tString &GetName() { return msName; }
  unsigned long GetHandle() { return mlHandle; }
  void SetHandle(unsigned long alHandle) { mlHandle = alHandle; }

  // Stable per-instance cookie (random, assigned in the constructor). Used
  // as a cache key in place of the resource pointer, which is unsafe across
  // free + realloc (a reused address would alias the old entry).
  uint64_t GetUniqueCookie() const { return mUniqueCookie; }

  void SetFullPath(const tWString &asPath);
  const tWString &GetFullPath() { return msFullPath; }

  // Back-pointer to the manager that owns this resource, or null when the
  // resource is standalone (created outside any manager). Set by
  // iResourceManager::AddResource on registration. A keep-alive minted from a
  // raw pointer (RetainResource) uses this to free through the right path:
  // the manager for managed resources, plain delete for standalone ones.
  iResourceManager *GetOwningManager() const { return mpOwningManager; }
  void SetOwningManager(iResourceManager *apManager) { mpOwningManager = apManager; }

  // Manager-assigned generation for dependent resources. It is valid only for
  // the current FileSearcher generation; independent resources remain 0.
  uint64_t GetResolutionGeneration() const { return mResolutionGeneration; }
  void SetResolutionGeneration(uint64_t alGeneration) { mResolutionGeneration = alGeneration; }
  bool IsResolutionDependent() const { return mbResolutionDependent; }
  void SetResolutionDependent(bool abDependent) { mbResolutionDependent = abDependent; }

  unsigned long GetTime() { return mlTime; }
  unsigned long GetPrio() { return mlPrio; }

  void SetLogDestruction(bool abX) { mbLogDestruction = abX; }

  unsigned int GetReferenceCount() { return mlUserCount.load(); }
  void AddReference();
  void DropReference() {
    // Dropping a reference that was never held is a refcount bug (e.g. a double
    // release). Catch it in debug; the guard still prevents underflow in release.
    assert(mlUserCount.load() > 0 && "DropReference on a resource with zero references");
    // Single-threaded acquire/release assumed; the atomic guards against torn
    // reads, the guarded decrement is not a lock-free CAS.
    if (mlUserCount.load() > 0)
      mlUserCount--;
  }
  bool HasReferences() { return mlUserCount.load() > 0; }

  static bool GetLogCreateAndDelete() { return mbLogCreateAndDelete; }
  static void SetLogCreateAndDelete(bool abX) { mbLogCreateAndDelete = abX; }

protected:
  static bool mbLogCreateAndDelete;

  tString msName;

  unsigned int mlPrio;  // dunno if this will be of any use.
  unsigned long mlTime; // Time for creation.
  unsigned long mlSize; // for completion. Not used yet.

  std::atomic<unsigned int> mlUserCount;
  unsigned long mlHandle;
  uint64_t mUniqueCookie;
  bool mbLogDestruction;
  iResourceManager *mpOwningManager = nullptr;
  uint64_t mResolutionGeneration = 0;
  bool mbResolutionDependent = false;

private:
  tWString msFullPath;
};


// Shared, reference-counted handle to a resource. The count IS the resource's own
// iResourceBase reference count: copying adds a reference, and when the last
// reference is dropped the handle frees the resource according to its cleanup
// policy. The policy is a variant so one (non-templated) handle type covers every
// ownership mode:
//   * std::monostate     -> call the destructor (delete) — standalone resource.
//   * Deleter            -> a custom free function — standalone with special cleanup.
//   * iResourceManager*  -> free via iResourceManager::FreeResource() — managed
//                           resource; the manager outlives every handle to it.
//
// There is no non-owning mode: a handle either owns exactly one reference or is
// empty. To share a pointer you only borrowed, take a real reference with
// RetainResource; to hand the reference off as a raw pointer, use Release().
// Single-threaded acquire/release is assumed.
template<class T>
class SharedResourceHandle {
public:
  using Deleter = void (*)(T*);
  // monostate => delete; Deleter => custom free fn; iResourceManager* => FreeResource.
  using CleanupPolicy = std::variant<std::monostate, Deleter, iResourceManager*>;

  SharedResourceHandle() = default;

  // Managed ctor: adopt one already-held reference (no increment); the owning
  // manager frees the resource when the last reference drops.
  SharedResourceHandle(iResourceManager* manager, T* handle)
      : m_handle(handle), m_policy(manager) {}

  // Standalone ctor: adopt one already-held reference; freed by the given policy
  // (default = delete) when the last reference drops.
  explicit SharedResourceHandle(T* handle, CleanupPolicy policy = std::monostate{})
      : m_handle(handle), m_policy(policy) {}

  SharedResourceHandle(const SharedResourceHandle& other)
      : m_handle(other.m_handle), m_policy(other.m_policy) {
    if (m_handle) m_handle->AddReference();
  }
  SharedResourceHandle(SharedResourceHandle&& other) noexcept
      : m_handle(other.m_handle), m_policy(other.m_policy) {
    other.m_handle = nullptr;
    other.m_policy = std::monostate{};
  }

  ~SharedResourceHandle() { reset(); }

  SharedResourceHandle& operator=(const SharedResourceHandle& other) {
    if (m_handle == other.m_handle) return *this; // same resource (incl. self) — alias safe
    reset();
    m_handle = other.m_handle;
    m_policy = other.m_policy;
    if (m_handle) m_handle->AddReference();
    return *this;
  }
  SharedResourceHandle& operator=(SharedResourceHandle&& other) noexcept {
    if (this == &other) return *this;
    reset();
    m_handle = other.m_handle;
    m_policy = other.m_policy;
    other.m_handle = nullptr;
    other.m_policy = std::monostate{};
    return *this;
  }

  // Detach without releasing: hands the held reference back to the caller as a raw
  // pointer (the caller becomes responsible for it). Like std::unique_ptr::release.
  T* Release() {
    T* h = m_handle;
    m_handle = nullptr;
    m_policy = std::monostate{};
    return h;
  }

  T* Get() const { return m_handle; }
  T* operator->() const { return m_handle; }
  T& operator*() const { return *m_handle; }
  explicit operator bool() const { return m_handle != nullptr; }
  bool IsValid() const { return m_handle != nullptr; }

private:
  // Drop this handle's reference; free per the cleanup policy once the count hits zero.
  void reset() {
    if (!m_handle) return;
    m_handle->DropReference();
    if (!m_handle->HasReferences()) {
      if (auto* mgr = std::get_if<iResourceManager*>(&m_policy)) {
        if (*mgr) (*mgr)->FreeResource(m_handle);
      } else if (auto* del = std::get_if<Deleter>(&m_policy)) {
        if (*del) (*del)(m_handle);
      } else {
        delete m_handle; // monostate: call the destructor
      }
    }
    m_handle = nullptr;
    m_policy = std::monostate{};
  }

  T* m_handle = nullptr;
  CleanupPolicy m_policy = std::monostate{};
};

// Mint a handle to a manager resource, taking the one reference this handle owns.
// This is the single bridge from a raw manager pointer to a shared handle: the
// manager's Create*/Get* does NOT touch the count, the handle does it here. Works
// for both a freshly loaded resource (count 0 -> 1) and a cache hit (n -> n+1).
template<class T>
inline SharedResourceHandle<T> AcquireResource(iResourceManager* manager, iResourceBase* base) {
  if (base) base->AddReference();
  return SharedResourceHandle<T>(manager, static_cast<T*>(base));
}

// Alias of AcquireResource for call sites that read better as "share this live
// resource I only have a borrowed pointer to".
template<class T>
inline SharedResourceHandle<T> RetainResource(iResourceManager* manager, T* handle) {
  return AcquireResource<T>(manager, handle);
}

// Share a live resource given only a raw pointer: takes a reference and rebuilds
// the cleanup policy from the resource's owning manager (managed) or falls back to
// delete (standalone). Used by the per-frame keep-alive, which holds an Image* but
// not its manager.
template<class T>
inline SharedResourceHandle<T> RetainResource(T* handle) {
  if (!handle) return {};
  handle->AddReference();
  if (iResourceManager* mgr = handle->GetOwningManager())
    return SharedResourceHandle<T>(mgr, handle);
  return SharedResourceHandle<T>(handle, std::monostate{});
}

// Take ownership of a freshly created, manager-less resource (count 0 -> 1). The
// policy (default = delete) frees it when the last handle drops.
template<class T>
inline SharedResourceHandle<T> AdoptResource(
    T* handle, typename SharedResourceHandle<T>::CleanupPolicy policy = std::monostate{}) {
  if (handle) handle->AddReference();
  return SharedResourceHandle<T>(handle, policy);
}

// Lightweight, NON-templated keep-alive "pin" for a reference-counted resource.
// Where SharedResourceHandle<T> is the typed owner, a pin is the type-erased
// keep-alive parked per frame (in the frame's resourceLink) so a mid-frame
// destroy of the real owner can't free the GPU resource before the submit that
// uses it retires. The concrete type is erased to iResourceBase* — no per-type
// template instantiation and no need for the resource's full definition at the
// park site — and the last-reference cleanup uses the same policy variant as
// SharedResourceHandle. A pin must agree with its owner's cleanup: managed
// resources free through the manager, standalone ones via plain delete (so
// standalone resources adopt the monostate/delete policy on both sides).
class SharedResourcePin {
public:
  using Deleter = void (*)(iResourceBase*);
  using CleanupPolicy = std::variant<std::monostate, Deleter, iResourceManager*>;

  SharedResourcePin() = default;
  SharedResourcePin(iResourceBase* resource, CleanupPolicy policy)
      : m_resource(resource), m_policy(policy) {
    if (m_resource) m_resource->AddReference();
  }

  SharedResourcePin(const SharedResourcePin& other)
      : m_resource(other.m_resource), m_policy(other.m_policy) {
    if (m_resource) m_resource->AddReference();
  }
  SharedResourcePin(SharedResourcePin&& other) noexcept
      : m_resource(other.m_resource), m_policy(other.m_policy) {
    other.m_resource = nullptr;
    other.m_policy = std::monostate{};
  }
  SharedResourcePin& operator=(SharedResourcePin other) noexcept { // copy-and-swap
    std::swap(m_resource, other.m_resource);
    std::swap(m_policy, other.m_policy);
    return *this;
  }
  ~SharedResourcePin() { reset(); }

private:
  void reset() {
    if (!m_resource) return;
    m_resource->DropReference();
    if (!m_resource->HasReferences()) {
      if (auto* mgr = std::get_if<iResourceManager*>(&m_policy)) {
        if (*mgr) (*mgr)->FreeResource(m_resource);
      } else if (auto* del = std::get_if<Deleter>(&m_policy)) {
        if (*del) (*del)(m_resource);
      } else {
        delete m_resource; // monostate: virtual ~iResourceBase
      }
    }
    m_resource = nullptr;
    m_policy = std::monostate{};
  }

  iResourceBase* m_resource = nullptr;
  CleanupPolicy m_policy = std::monostate{};
};

// Pin a live resource for the frame: takes a reference and rebuilds the cleanup
// policy from its owning manager (managed) or falls back to delete (standalone).
inline SharedResourcePin PinResource(iResourceBase* resource) {
  if (!resource) return {};
  if (iResourceManager* mgr = resource->GetOwningManager())
    return SharedResourcePin(resource, mgr);
  return SharedResourcePin(resource, std::monostate{});
}

}; // namespace hpl
#endif // HPL_RESOURCEBASE_H
