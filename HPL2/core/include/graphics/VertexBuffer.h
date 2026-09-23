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

#ifndef HPL_VERTEXBUFFER_H
#define HPL_VERTEXBUFFER_H

#include "graphics/GraphicsTypes.h"
#include "math/MathTypes.h"
#include "system/SystemTypes.h"
#include "math/BoundingVolume.h"

#include "graphics/RITypes.h"
#include "graphics/Graphics.h"
#include "system/Event.h"

#include <cassert>
#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace hpl {

class cBoundingVolume;

// The single concrete vertex buffer. CPU-side geometry building + shadow data
// plus the RI (Vulkan) GPU stream / BLAS management. Formerly split across the
// abstract iVertexBuffer interface and its sole implementation VertexBuffer_RI;
// collapsed into one type now that there is only one backend.
class cVertexBuffer {
public:
  static size_t GetSizeFromHPL(eVertexBufferElementFormat format);

  struct VertexElement {
  public:
    // GPU buffer, shared-owned; the owning cVertexBuffer defers its disposal to
    // Interface<cGraphics>::Get()->graphicsDefer on regrow / teardown. Borrow the raw handle via GetBuffer().
    RISharedPointer<RIBuffer> buffer;
    RIBuffer *GetBuffer() const;
    eVertexBufferElementFormat format =
        eVertexBufferElementFormat::eVertexBufferElementFormat_Float;
    eVertexBufferElement type =
        eVertexBufferElement::eVertexBufferElement_Position;
    tVertexElementFlag flag = 0;
    size_t num = 0;
    int programVarIndex = 0; // for legacy behavior

    size_t Stride() const;
    size_t NumElements() const;

    template <typename TData> std::span<TData> GetElements() {
      assert(sizeof(TData) == Stride() && "Data must be same size as stride");
      return std::span<TData *>(reinterpret_cast<TData *>(m_shadowData.data()),
                                m_shadowData.size() / Stride());
    }

    std::span<uint8_t> Data() const { return m_shadowData; }

    template <typename TData> TData &GetElement(size_t index) {
      assert(sizeof(TData) <= Stride() &&
             "Date must be less than or equal to stride");
      return *reinterpret_cast<TData *>(m_shadowData.data() + index * Stride());
    }

    template <typename TData> const TData &GetElement(size_t index) const {
      assert(sizeof(TData) <= Stride() &&
             "Date must be less than or equal to stride");
      return *reinterpret_cast<TData *>(m_shadowData.data() + index * Stride());
    }

  private:
    mutable size_t m_activeCopy = 0;         // the active copy of the data
    mutable size_t m_internalBufferSize = 0; // the size of the internal buffer
    mutable std::vector<uint8_t> m_shadowData = {};
    friend class cVertexBuffer;
  };

  cVertexBuffer(eVertexBufferType aType,
			eVertexBufferDrawType aDrawType,eVertexBufferUsageType aUsageType,
			int alReserveVtxSize = 0,int alReserveIdxSize = 0);
  ~cVertexBuffer();

  inline eVertexBufferType GetType() const { return mType; }

  void CreateShadowDouble(bool abUpdateData);

  void CreateElementArray(eVertexBufferElement aType,
                          eVertexBufferElementFormat aFormat,
                          int alElementNum,
                          int alProgramVarIndex = 0);

  void AddVertexVec3f(eVertexBufferElement aElement, const cVector3f &avVtx);
  void AddVertexVec4f(eVertexBufferElement aElement, const cVector3f &avVtx,
                      float afW);
  void AddVertexColor(eVertexBufferElement aElement, const cColor &aColor);
  void AddIndex(unsigned int alIndex);

  bool Compile(tVertexCompileFlag aFlags);
  void UpdateData(tVertexElementFlag aTypes, bool abIndices);

  void Transform(const cMatrixf &mtxTransform);

  void Draw(eVertexBufferDrawType aDrawType = eVertexBufferDrawType_LastEnum);
  void Bind();
  void UnBind();
  void DrawIndices(unsigned int *apIndices, int alCount,
                   eVertexBufferDrawType aDrawType);

  struct GeometryBinding {
    struct VertexGeometryEntry {
      VertexElement *element;
      uint64_t offset;
    };
    struct VertexIndexEntry {
      std::shared_ptr<RIBuffer> m_buffer;
      uint64_t offset;
      uint32_t numIndicies;
    };
    std::array<VertexGeometryEntry, eVertexBufferElement_LastEnum>
        m_vertexElement; // elements are in the order they are requested
    VertexIndexEntry m_indexBuffer;
  };

  // Hand every owned GPU buffer/AS to Interface<cGraphics>::Get()->graphicsDefer (deferred dispose past
  // the next submit). Called from the destructor.
  void DeferOwnedResources();
  // Uploads dirty vertex/index streams (allocates GPU buffers on first submit /
  // shadow-data growth). No-op when nothing changed since the last submit.
  void SubmitToGPU(RIDevice *device);
  // Ensures streams are uploaded (calls SubmitToGPU), then (re)builds the BLAS
  // if it is missing or was built from an older geometry generation. Only call
  // for renderables that are TLAS instances; particles/billboards/beams/ropes/
  // decals just use SubmitToGPU. The build is recorded into `cmd`; the caller
  // must ensure it completes before the BLAS is read (e.g. via the accel-build
  // barrier before the TLAS build).
  void BuildBlas(RICmd *cmd, RIDevice *device,
                 cGraphics::FrameContext *cntx);

  cVertexBuffer *CreateCopy(eVertexBufferType aType,
                            eVertexBufferUsageType aUsageType,
                            tVertexElementFlag alVtxToCopy);

  cBoundingVolume CreateBoundingVolume();

  int GetVertexNum();
  int GetIndexNum();

  int GetElementNum(eVertexBufferElement aElement);
  eVertexBufferElementFormat GetElementFormat(eVertexBufferElement aElement);
  int GetElementProgramVarIndex(eVertexBufferElement aElement);

  float *GetFloatArray(eVertexBufferElement aElement);
  int *GetIntArray(eVertexBufferElement aElement);
  unsigned char *GetByteArray(eVertexBufferElement aElement);

  unsigned int *GetIndices();

  void ResizeArray(eVertexBufferElement aElement, int alSize);
  void ResizeIndices(int alSize);

  // Set the number of elements to draw. If < 0, draw all indices.
  void SetElementNum(int alNum) { mlElementNum = alNum; }
  int GetElementNum() { return mlElementNum; }

  tVertexElementFlag GetVertexElementFlags() { return mVertexFlags; }

  const cVertexBuffer::VertexElement *GetElement(eVertexBufferElement elementType);
  // Borrow the BLAS / index buffer; null when none has been built/allocated.
  RIAccelStructure *accelStructure() { return m_blas.isEmpty() ? nullptr : m_blas.Get(); }
  RIBuffer *GetIndexRIBuffer() const;

  // Bindless-slot cleanup hook. GlobalManagedSets::submitObject binds an
  // EventHandler (stored in its object-slot cache) to this event; on destruction
  // the VB Signals it so the set bumps this VB's object-slot reuse generation,
  // invalidating anything holding a cached reference to the slot before the
  // now-freed vertex buffer-device-address is dereferenced -> GPUVM fault.
  Event<> &OnDestroyed() { return m_onDestroyed; }

  // Geometry-invalidation hook. Signaled whenever this VB's GPU geometry is
  // rebuilt in a way that can move triangles around — Compile() (incl. the
  // CreateCopy finalize) and the SubmitToGPU realloc path (first submit,
  // shadow-data growth, CreateCopy sentinel). A consumer that caches a
  // primitiveIndex / BDA per object slot can bind a handler here to bump that
  // slot's reuse generation, so the cached reference self-invalidates before the
  // now-stale vertex/index BDA is dereferenced -> OOB read -> GPUVM fault. Plain
  // UpdateData() re-uploads (same buffer, same triangle count) do NOT signal, so
  // animated meshes are not over-invalidated. Currently no subscriber:
  // GlobalManagedSets detects the same condition itself by comparing the VB's
  // index count per submit.
  Event<> &OnGeometryChanged() { return m_onGeometryChanged; }

protected:
  static void
  PushVertexElements(std::span<const float> values,
                     eVertexBufferElement elementType,
                     std::span<cVertexBuffer::VertexElement> elements);

  eVertexBufferType mType;
  tVertexElementFlag mVertexFlags;
  eVertexBufferDrawType mDrawType;
  eVertexBufferUsageType mUsageType;

  int mlReservedVtxSize;
  int mlReservedIdxSize;

  int mlElementNum;

  bool mbTangents;

  std::vector<VertexElement> m_vertexElements = {};
  RISharedPointer<RIBuffer> m_indexBuffer;
  std::vector<uint32_t> m_indices = {};

  uint32_t m_generation = 0;
  uint32_t m_lastSubmitted = 0;
  uint32_t m_blasGeneration = 0; // m_generation at the last successful BLAS build

  size_t m_indexBufferActiveCopy = 0;
  size_t m_indexBufferCapacity = 0; // bytes allocated in m_indexBuffer
  tVertexElementFlag m_updateFlags = 0; // update no need to rebuild buffers
  bool m_updateIndices = false;

  // Cached BLAS built on first BuildBlas call. Shared-owned; the old value is
  // deferred to Interface<cGraphics>::Get()->graphicsDefer on rebuild / teardown so it outlives any
  // in-flight build.
  RISharedPointer<RIBuffer> m_blasStorage;
  RISharedPointer<RIAccelStructure> m_blas;

  // Signaled in the destructor; GlobalManagedSets connects a per-slot destroy
  // handler (owned by its object-slot cache) to it.
  Event<> m_onDestroyed;

  // Signaled on geometry rebuild (Compile / SubmitToGPU realloc). See
  // OnGeometryChanged() above.
  Event<> m_onGeometryChanged;

  friend struct VertexElement;
};

} // namespace hpl

#endif // HPL_VERTEXBUFFER_H
