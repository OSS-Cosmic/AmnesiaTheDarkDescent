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

#include "impl/LegacyVertexBuffer.h"

#include "bgfx/bgfx.h"
#include "bgfx/defines.h"
#include "graphics/GraphicsContext.h"
#include "math/Math.h"
#include "system/LowLevelSystem.h"

#include "impl/LowLevelGraphicsSDL.h"

#include <SDL2/SDL_stdinc.h>
#include <algorithm>
#include <cstdint>
#include <vector>

#include "Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "tinyimageformat_query.h"
#include <FixPreprocessor.h>

namespace hpl {

    namespace detail {
        static ResourceMemoryUsage toMemoryUsage(eVertexBufferUsageType aUsageType) {
            switch (aUsageType) {
                case eVertexBufferUsageType_Static:
                    return RESOURCE_MEMORY_USAGE_GPU_ONLY;
                case eVertexBufferUsageType_Dynamic:
                case eVertexBufferUsageType_Stream:
                    return RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
            }
            return RESOURCE_MEMORY_USAGE_GPU_ONLY;
        };
        static bool IsDynamicMemory(eVertexBufferUsageType aUsageType) {
            return aUsageType == eVertexBufferUsageType_Dynamic || aUsageType == eVertexBufferUsageType_Stream;
        }
    }

    size_t LegacyVertexBuffer::GetSizeFromHPL(eVertexBufferElementFormat format) {
        switch (format) {
        case eVertexBufferElementFormat_Float:
            return sizeof(float);
        case eVertexBufferElementFormat_Int:
            return sizeof(uint32_t);
        case eVertexBufferElementFormat_Byte:
            return sizeof(char);
        default:
            break;
        }
        ASSERT(false && "Unknown vertex attribute type.");
        return 0;
    }

    bgfx::AttribType::Enum LegacyVertexBuffer::GetAttribTypeFromHPL(eVertexBufferElementFormat format) {
        switch (format) {
        case eVertexBufferElementFormat_Float:
            return bgfx::AttribType::Float;
        case eVertexBufferElementFormat_Int:
            return bgfx::AttribType::Float;
        case eVertexBufferElementFormat_Byte:
            return bgfx::AttribType::Uint8;
        default:
            break;

        }
        ASSERT(false && "Unknown vertex attribute type.");
        return bgfx::AttribType::Count;
    }

    bgfx::Attrib::Enum LegacyVertexBuffer::GetAttribFromHPL(eVertexBufferElement format) {
        switch (format) {
        case eVertexBufferElement_Normal:
            return bgfx::Attrib::Normal;
        case eVertexBufferElement_Position:
            return bgfx::Attrib::Position;
        case eVertexBufferElement_Color0:
            return bgfx::Attrib::Color0;
        case eVertexBufferElement_Color1:
            return bgfx::Attrib::Color1;
        case eVertexBufferElement_Texture1Tangent:
            return bgfx::Attrib::Tangent;
        case eVertexBufferElement_Texture0:
            return bgfx::Attrib::TexCoord0;
        case eVertexBufferElement_Texture1:
            return bgfx::Attrib::TexCoord1;
        case eVertexBufferElement_Texture2:
            return bgfx::Attrib::TexCoord2;
        case eVertexBufferElement_Texture3:
            return bgfx::Attrib::TexCoord3;
        case eVertexBufferElement_Texture4:
            return bgfx::Attrib::TexCoord4;
        case eVertexBufferElement_User0:
            return bgfx::Attrib::TexCoord5;
        case eVertexBufferElement_User1:
            return bgfx::Attrib::TexCoord6;
        case eVertexBufferElement_User2:
            return bgfx::Attrib::TexCoord7;
        case eVertexBufferElement_User3:
        case eVertexBufferElement_LastEnum:
            break;
        }
        ASSERT(false && "Unknown vertex attribute type.");
        return bgfx::Attrib::Count;
    }


    LegacyVertexBuffer::LegacyVertexBuffer(
        eVertexBufferDrawType aDrawType,
        eVertexBufferUsageType aUsageType,
        int alReserveVtxSize,
        int alReserveIdxSize)
        : iVertexBuffer(aDrawType, aUsageType, alReserveVtxSize, alReserveIdxSize) {
    }

    LegacyVertexBuffer::~LegacyVertexBuffer() {

    }

    void LegacyVertexBuffer::PushVertexElements(
        std::span<const float> values, eVertexBufferElement elementType, std::span<LegacyVertexBuffer::VertexElement> elements) {
        for (auto& element : elements) {
            if (element.m_type == elementType) {
                element.m_rebuild = true;
                auto& buffer = element.m_shadowData;
                switch (element.m_format) {
                case eVertexBufferElementFormat_Float:
                    {
                        for (size_t i = 0; i < element.m_num; i++) {
                            union {
                                float f;
                                unsigned char b[sizeof(float)];
                            } entry = { i < values.size() ? values[i] : 0 };
                            buffer.insert(buffer.end(), std::begin(entry.b), std::end(entry.b));
                        }
                    }
                    break;
                case eVertexBufferElementFormat_Int:
                    {
                        for (size_t i = 0; i < element.m_num; i++) {
                            union {
                                int f;
                                unsigned char b[sizeof(int)];
                            } entry = { i < values.size() ? static_cast<int>(values[i]) : 0 };
                            buffer.insert(buffer.end(), std::begin(entry.b), std::end(entry.b));
                        }
                    }
                    break;
                case eVertexBufferElementFormat_Byte:
                    {
                        for (size_t i = 0; i < element.m_num; i++) {
                            buffer.emplace_back(i < values.size() ? static_cast<char>(values[i]) : 0);
                        }
                    }
                    break;
                default:
                    break;
                }
                return;
            }
        }
    }

    size_t LegacyVertexBuffer::VertexElement::Stride() const {
        return GetSizeFromHPL(m_format) * m_num;
    }

    size_t LegacyVertexBuffer::VertexElement::NumElements() const {
        return m_shadowData.size() / Stride();
    }

    void LegacyVertexBuffer::AddVertexVec3f(eVertexBufferElement aElement, const cVector3f& avVtx) {
        PushVertexElements(
            std::span(std::begin(avVtx.v), std::end(avVtx.v)), aElement, std::span(m_vertexElements.begin(), m_vertexElements.end()));
    }

    void LegacyVertexBuffer::AddVertexVec4f(eVertexBufferElement aElement, const cVector3f& avVtx, float afW) {
        PushVertexElements(
            std::span(std::begin(avVtx.v), std::end(avVtx.v)), aElement, std::span(m_vertexElements.begin(), m_vertexElements.end()));
    }

    void LegacyVertexBuffer::AddVertexColor(eVertexBufferElement aElement, const cColor& aColor) {
        PushVertexElements(
            std::span(std::begin(aColor.v), std::end(aColor.v)), aElement, std::span(m_vertexElements.begin(), m_vertexElements.end()));
    }

    void LegacyVertexBuffer::AddIndex(unsigned int alIndex) {
        m_rebuildIndices = true;
        m_indices.push_back(alIndex);
    }

    void LegacyVertexBuffer::Transform(const cMatrixf& mtxTransform) {
        cMatrixf mtxRot = mtxTransform.GetRotation();
        cMatrixf mtxNormalRot = cMath::MatrixInverse(mtxRot).GetTranspose();

        ///////////////
        // Get position
        auto positionElement = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [](const auto& element) {
            return element.m_type == eVertexBufferElement_Position;
        });
        if (positionElement != m_vertexElements.end()) {
            ASSERT(positionElement->m_format == eVertexBufferElementFormat_Float && "Only float format supported");
            ASSERT(positionElement->m_num >= 3 && "Only 3 component format supported");
            struct PackedVec3 {
                float x;
                float y;
                float z;
            };
            for (size_t i = 0; i < positionElement->NumElements(); i++) {
                auto& position = positionElement->GetElement<PackedVec3>(i);
                cVector3f outputPos = cMath::MatrixMul(mtxTransform, cVector3f(position.x, position.y, position.z));
                position = { outputPos.x, outputPos.y, outputPos.z };
            }
        }

        auto normalElement = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [](const auto& element) {
            return element.m_type == eVertexBufferElement_Normal;
        });
        if (normalElement != m_vertexElements.end()) {
            ASSERT(normalElement->m_format == eVertexBufferElementFormat_Float && "Only float format supported");
            ASSERT(normalElement->m_num >= 3 && "Only 3 component format supported");
            struct PackedVec3 {
                float x;
                float y;
                float z;
            };
            for (size_t i = 0; i < normalElement->NumElements(); i++) {
                auto& normal = normalElement->GetElement<PackedVec3>(i);
                cVector3f outputNormal = cMath::MatrixMul(mtxNormalRot, cVector3f(normal.x, normal.y, normal.z));
                normal = { outputNormal.x, outputNormal.y, outputNormal.z };
            }
        }

        auto tangentElement = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [](const auto& element) {
            return element.m_type == eVertexBufferElement_Texture1Tangent;
        });
        if (tangentElement != m_vertexElements.end()) {
            ASSERT(tangentElement->m_format == eVertexBufferElementFormat_Float && "Only float format supported");
            ASSERT(tangentElement->m_num >= 3 && "Only 4 component format supported");
            struct PackedVec3 {
                float x;
                float y;
                float z;
            };
            for (size_t i = 0; i < normalElement->NumElements(); i++) {
                auto& tangent = tangentElement->GetElement<PackedVec3>(i);
                cVector3f outputTangent = cMath::MatrixMul(mtxRot, cVector3f(tangent.x, tangent.y, tangent.z));
                tangent = { outputTangent.x, outputTangent.y, outputTangent.z };
            }
        }

        // ////////////////////////////
        // //Update the data
        tVertexElementFlag vtxFlag = eVertexElementFlag_Position;
        if (normalElement != m_vertexElements.end()) {
            vtxFlag |= eVertexElementFlag_Normal;
        }
        if (tangentElement != m_vertexElements.end()) {
            vtxFlag |= eVertexElementFlag_Texture1;
        }

        UpdateData(vtxFlag, false);
    }

    int LegacyVertexBuffer::GetElementNum(eVertexBufferElement aElement) {
        auto element = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [aElement](const auto& element) {
            return element.m_type == aElement;
        });
        if (element != m_vertexElements.end()) {
            return element->m_num;
        }
        return 0;
    }

    eVertexBufferElementFormat LegacyVertexBuffer::GetElementFormat(eVertexBufferElement aElement) {
        auto element = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [aElement](const auto& element) {
            return element.m_type == aElement;
        });
        if (element != m_vertexElements.end()) {
            return element->m_format;
        }
        return eVertexBufferElementFormat_LastEnum;
    }

    int LegacyVertexBuffer::GetElementProgramVarIndex(eVertexBufferElement aElement) {
        auto element = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [aElement](const auto& element) {
            return element.m_type == aElement;
        });
        if (element != m_vertexElements.end()) {
            return element->m_programVarIndex;
        }
        return 0;
    }

    bool LegacyVertexBuffer::Compile(tVertexCompileFlag aFlags) {
        if (aFlags & eVertexCompileFlag_CreateTangents) {
            CreateElementArray(eVertexBufferElement_Texture1Tangent, eVertexBufferElementFormat_Float, 4);
            auto positionElement = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [](const auto& element) {
                return element.m_type == eVertexBufferElement_Position;
            });
            auto normalElement = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [](const auto& element) {
                return element.m_type == eVertexBufferElement_Normal;
            });
            auto textureElement = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [](const auto& element) {
                return element.m_type == eVertexBufferElement_Texture0;
            });
            auto tangentElement = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [](const auto& element) {
                return element.m_type == eVertexBufferElement_Texture1Tangent;
            });

            ASSERT(positionElement != m_vertexElements.end() && "No position element found");
            ASSERT(normalElement != m_vertexElements.end() && "No normal element found");
            ASSERT(textureElement != m_vertexElements.end() && "No texture element found");
            ASSERT(tangentElement != m_vertexElements.end() && "No tangent element found");
            ASSERT(positionElement->m_format == eVertexBufferElementFormat_Float && "Only float format supported");
            ASSERT(normalElement->m_format == eVertexBufferElementFormat_Float && "Only float format supported");
            ASSERT(textureElement->m_format == eVertexBufferElementFormat_Float && "Only float format supported");
            ASSERT(tangentElement->m_format == eVertexBufferElementFormat_Float && "Only float format supported");
            ASSERT(positionElement->m_num >= 3 && "Only 3 component format supported");
            ASSERT(normalElement->m_num >= 3 && "Only 3 component format supported");
            ASSERT(textureElement->m_num >= 2 && "Only 2 component format supported");
            ASSERT(tangentElement->m_num >= 4 && "Only 4 component format supported");

            ResizeArray(eVertexBufferElement_Texture1Tangent, GetVertexNum() * 4);

            cMath::CreateTriTangentVectors(
                reinterpret_cast<float*>(tangentElement->m_shadowData.data()),
                m_indices.data(),
                m_indices.size(),
                reinterpret_cast<float*>(positionElement->m_shadowData.data()),
                positionElement->m_num,
                reinterpret_cast<float*>(textureElement->m_shadowData.data()),
                reinterpret_cast<float*>(normalElement->m_shadowData.data()),
                positionElement->NumElements());
        }
        SyncToken token = {};

        
        for (auto& element : m_vertexElements) {
            element.m_buffer.TryFree();
            element.m_rebuild = false;
            BufferLoadDesc loadDesc = {};
            loadDesc.ppBuffer = &element.m_buffer.m_handle;
            loadDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
            loadDesc.mDesc.mMemoryUsage = detail::toMemoryUsage(mUsageType);

            if(detail::IsDynamicMemory(mUsageType)) {
                loadDesc.mDesc.mSize = element.m_shadowData.size() * ForgeRenderer::SwapChainLength;
            } else {
                loadDesc.mDesc.mSize = element.m_shadowData.size();
            }
            loadDesc.pData = element.m_shadowData.data();
            addResource(&loadDesc, &token);
            element.m_buffer.Initialize();
        }

        m_rebuildIndices = false;
        m_indexBuffer.TryFree();
        BufferLoadDesc loadDesc = {};
        loadDesc.ppBuffer = &m_indexBuffer.m_handle;
        loadDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_INDEX_BUFFER;
        loadDesc.mDesc.mMemoryUsage = detail::toMemoryUsage(mUsageType);
        if(detail::IsDynamicMemory(mUsageType)) {
            // we need to multiply by the swapchain length to account for the fact that we have a buffer per swapchain
            loadDesc.mDesc.mSize = m_indices.size() * sizeof(uint32_t) *  ForgeRenderer::SwapChainLength;
        } else {
            loadDesc.mDesc.mSize = m_indices.size() * sizeof(uint32_t);
        }
        loadDesc.pData = m_indices.data();
        addResource(&loadDesc, &token);
        m_indexBuffer.Initialize();
        waitForToken(&token);
        return true;
    }

    void LegacyVertexBuffer::UpdateData(tVertexElementFlag aTypes, bool abIndices) {
        m_updateFlags |= aTypes;
        m_updateIndices |= abIndices;
    }

    float* LegacyVertexBuffer::GetFloatArray(eVertexBufferElement aElement) {
        auto element = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [aElement](const auto& element) {
            return element.m_type == aElement;
        });
        if (element != m_vertexElements.end()) {
            return reinterpret_cast<float*>(element->m_shadowData.data());
        }
        return nullptr;
    }

    int* LegacyVertexBuffer::GetIntArray(eVertexBufferElement aElement) {
        auto element = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [aElement](const auto& element) {
            return element.m_type == aElement;
        });
        if (element != m_vertexElements.end()) {
            return reinterpret_cast<int*>(element->m_shadowData.data());
        }
        return nullptr;
    }

    unsigned char* LegacyVertexBuffer::GetByteArray(eVertexBufferElement aElement) {
        auto element = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [aElement](const auto& element) {
            return element.m_type == aElement;
        });
        if (element != m_vertexElements.end()) {
            return element->m_shadowData.data();
        }
        return nullptr;
    }

    unsigned int* LegacyVertexBuffer::GetIndices() {
        return m_indices.data();
    }

    void LegacyVertexBuffer::ResizeArray(eVertexBufferElement aElement, int alSize) {
        auto element = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [aElement](const auto& element) {
            return element.m_type == aElement;
        });
        if (element != m_vertexElements.end()) {
            element->m_rebuild = true;
            element->m_shadowData.resize(alSize * GetSizeFromHPL(element->m_format));
        }
    }

    void LegacyVertexBuffer::ResizeIndices(int alSize) {
        m_rebuildIndices = true;
        m_indices.resize(alSize);
    }

    const LegacyVertexBuffer::VertexElement* LegacyVertexBuffer::GetElement(eVertexBufferElement elementType) {
        auto element = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [elementType](const auto& element) {
            return element.m_type == elementType;
        });
        if (element != m_vertexElements.end()) {
            return element;
        }
        return nullptr;
    }

    void LegacyVertexBuffer::CreateElementArray(
        eVertexBufferElement aType, eVertexBufferElementFormat aFormat, int alElementNum, int alProgramVarIndex) {
        tVertexElementFlag elementFlag = GetVertexElementFlagFromEnum(aType);
        if (elementFlag & mVertexFlags) {
            Error("Vertex element of type %d already present in buffer %d!\n", aType, this);
            return;
        }
        mVertexFlags |= elementFlag;

        VertexElement element;
        element.m_type = aType;
        element.m_flag = elementFlag;
        element.m_format = aFormat;
        element.m_num = alElementNum;
        element.m_programVarIndex = alProgramVarIndex;
        m_vertexElements.push_back(std::move(element));
    }

    int LegacyVertexBuffer::GetVertexNum() {
        auto positionElement = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [](const auto& element) {
            return element.m_type == eVertexBufferElement_Position;
        });
        ASSERT(positionElement != m_vertexElements.end() && "No position element found");
        return positionElement->NumElements();
    }

    int LegacyVertexBuffer::GetIndexNum() {
        return m_indices.size();
    }

    cBoundingVolume LegacyVertexBuffer::CreateBoundingVolume() {
        cBoundingVolume bv;
        if ((mVertexFlags & eVertexElementFlag_Position) == 0) {
            Warning("Could not create bounding volume from buffer %d  because no position element was present!\n", this);
            return bv;
        }

        auto positionElement = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [](const auto& element) {
            return element.m_type == eVertexBufferElement_Position;
        });

        if (positionElement == m_vertexElements.end()) {
            return bv;
        }

        if (positionElement->m_format != eVertexBufferElementFormat_Float) {
            Warning("Could not breate bounding volume since postion was not for format float in buffer %d!\n", this);
            return bv;
        }

        bv.AddArrayPoints(reinterpret_cast<float*>(positionElement->m_shadowData.data()), GetVertexNum());
        bv.CreateFromPoints(positionElement->m_num);

        return bv;
    }

    void LegacyVertexBuffer::Draw(eVertexBufferDrawType aDrawType) {
    }
    void LegacyVertexBuffer::DrawIndices(unsigned int* apIndices, int alCount, eVertexBufferDrawType aDrawType) {
        ASSERT(false && "Not implemented");
    }

    void LegacyVertexBuffer::GetLayoutStream(GraphicsContext::LayoutStream& layoutStream, eVertexBufferDrawType aDrawType) {
        ASSERT(false && "need to reimplement");
        // layoutStream.m_drawType = aDrawType == eVertexBufferDrawType_LastEnum ? mDrawType : aDrawType;
        // for (auto& element : m_vertexElements) {
        //     layoutStream.m_vertexStreams.push_back({
        //         .m_handle = element.m_handle,
        //         .m_dynamicHandle = element.m_dynamicHandle,
        //     });
        // }
        // layoutStream.m_indexStream = {
        //     .m_handle = m_indexBufferHandle,
        //     .m_dynamicHandle = m_dynamicIndexBufferHandle,
        // };
    }

    void LegacyVertexBuffer::resolveGeometryBinding(
        uint32_t frameIndex, std::span<eVertexBufferElement> elements, GeometryBinding* binding) {
        if (m_updateIndices || m_updateFlags) {
            if (m_frameIndex != frameIndex) {
                m_bufferIndex = (m_bufferIndex + 1) % ForgeRenderer::SwapChainLength;
                m_frameIndex = frameIndex;
            }

            SyncToken token = {};
            const bool isDynamicAccess = detail::IsDynamicMemory(mUsageType);
            for (auto& element : m_vertexElements) {
                if (m_updateFlags & element.m_flag || element.m_rebuild) {
                    if (!isDynamicAccess || element.m_rebuild) {
                        element.m_rebuild = false;
                        element.m_buffer.TryFree();
                        BufferLoadDesc loadDesc = {};
                        loadDesc.ppBuffer = &element.m_buffer.m_handle;
                        loadDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
                        loadDesc.mDesc.mMemoryUsage = detail::toMemoryUsage(mUsageType);
                        if (detail::IsDynamicMemory(mUsageType)) {
                            loadDesc.mDesc.mSize = element.m_shadowData.size() * ForgeRenderer::SwapChainLength;
                        } else {
                            loadDesc.mDesc.mSize = element.m_shadowData.size();
                        }
                        loadDesc.pData = element.m_shadowData.data();
                        addResource(&loadDesc, &token);
                        element.m_buffer.Initialize();
                    }
                    if (isDynamicAccess) {
                        ASSERT(element.m_buffer.IsValid() && "Buffer not initialized");
                        BufferUpdateDesc updateDesc = { element.m_buffer.m_handle };
                        updateDesc.mSize = element.m_shadowData.size();
                        updateDesc.mDstOffset = m_bufferIndex * updateDesc.mSize;
                        beginUpdateResource(&updateDesc);
                        memcpy(updateDesc.pMappedData, element.m_shadowData.data(), element.m_shadowData.size());
                        endUpdateResource(&updateDesc, &token);
                    }
                }
            }
            if (m_updateIndices || m_rebuildIndices) {
                if (!isDynamicAccess || m_rebuildIndices) {
                    m_rebuildIndices = false;
                    m_indexBuffer.TryFree();
                    BufferLoadDesc loadDesc = {};
                    loadDesc.ppBuffer = &m_indexBuffer.m_handle;
                    loadDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_INDEX_BUFFER;
                    loadDesc.mDesc.mMemoryUsage = detail::toMemoryUsage(mUsageType);
                    if (isDynamicAccess) {
                        loadDesc.mDesc.mSize = m_indices.size() * ForgeRenderer::SwapChainLength * sizeof(uint32_t);
                    } else {
                        loadDesc.mDesc.mSize = m_indices.size() * sizeof(uint32_t);
                    }
                    loadDesc.pData = m_indices.data();
                    addResource(&loadDesc, &token);
                    m_indexBuffer.Initialize();
                }
                if (isDynamicAccess) {
                    ASSERT(m_indexBuffer.IsValid() && "Buffer not initialized");
                    BufferUpdateDesc updateDesc = { m_indexBuffer.m_handle };
                    updateDesc.mSize = m_indices.size() * sizeof(uint32_t);
                    updateDesc.mDstOffset = m_bufferIndex * m_indices.size() * sizeof(uint32_t);
                    beginUpdateResource(&updateDesc);
                    memcpy(updateDesc.pMappedData, m_indices.data(), m_indices.size() * sizeof(uint32_t));
                    endUpdateResource(&updateDesc, &token);
                }
            }
            m_updateIndices = false;
            m_updateFlags = 0;
            m_frameIndex = frameIndex;

            // waitForToken(&token);
        }

        // GeometryBinding binding = {};
        for (auto& targetEle : elements) {
            auto found = std::find_if(m_vertexElements.begin(), m_vertexElements.end(), [&](auto& element) {
                return element.m_type == targetEle;
            });
            if(found == m_vertexElements.end()) {
                ASSERT(false && "failed to bind vertex buffer");
                return;
            }
            binding->m_vertexElement.push_back({found, m_bufferIndex * found->m_shadowData.size() });
        }
        binding->m_indexBuffer = {
            &m_indexBuffer, m_bufferIndex * m_indices.size() * sizeof(uint32_t), static_cast<uint32_t>(m_indices.size())
        };
    }

    void LegacyVertexBuffer::UnBind() {
        return;
    }

    iVertexBuffer* LegacyVertexBuffer::CreateCopy(
        eVertexBufferType aType, eVertexBufferUsageType aUsageType, tVertexElementFlag alVtxToCopy) {
        auto* vertexBuffer =
            new LegacyVertexBuffer(mDrawType, aUsageType, GetIndexNum(), GetVertexNum());
        vertexBuffer->m_indices = m_indices;

        for (auto element : m_vertexElements) {
            if (element.m_flag & alVtxToCopy) {
                auto& vb = vertexBuffer->m_vertexElements.emplace_back(element);
                vb.m_buffer.TryFree();
            }
        }
        vertexBuffer->Compile(0); // actually create the buffers
        return vertexBuffer;
    }

} // namespace hpl
