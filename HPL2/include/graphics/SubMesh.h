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

#pragma once

#include "Common_3/Graphics/Interfaces/IGraphics.h"
#include "graphics/ForgeHandles.h"
#include "graphics/GraphicsBuffer.h"
#include "graphics/GraphicsTypes.h"
#include "math/MathTypes.h"
#include "math/MeshTypes.h"
#include "physics/PhysicsTypes.h"
#include "system/SystemTypes.h"

#include <optional>
#include <span>

#include <folly/small_vector.h>
namespace hpl {

    class cMaterial;
    class iVertexBuffer;

    class cMesh;
    class iPhysicsWorld;
    class iCollideShape;

    class cMaterialManager;

    class cSubMesh final {
        friend class cMesh;
        friend class cSubMeshEntity;

    public:
        struct NotifyMesh {
            uint32_t m_semanticSize;
            ShaderSemantic m_semantic[12];
            uint32_t changeIndexData: 1;
        };
        using NotifySubMeshChanged = hpl::Event<NotifyMesh>;
        NotifySubMeshChanged m_notify;


        // preset traits that are expected throughout the engine
        struct PostionTrait {
            using Type = float3;
            static constexpr TinyImageFormat format = TinyImageFormat_R32G32B32_SFLOAT;
            static constexpr uint32_t Stride = sizeof(float3);
            static constexpr ShaderSemantic Semantic = ShaderSemantic::SEMANTIC_POSITION;
        };
        struct NormalTrait {
            using Type = float3;
            static constexpr TinyImageFormat format = TinyImageFormat_R32G32B32_SFLOAT;
            static constexpr uint32_t Stride = sizeof(float3);
            static constexpr ShaderSemantic Semantic = ShaderSemantic::SEMANTIC_NORMAL;
        };
        struct ColorTrait {
            using Type = float4;
            static constexpr TinyImageFormat format = TinyImageFormat_R32G32B32A32_SFLOAT;
            static constexpr uint32_t Stride = sizeof(float4);
            static constexpr ShaderSemantic Semantic = ShaderSemantic::SEMANTIC_COLOR;
        };
        struct TangentTrait {
            using Type = float3;
            static constexpr TinyImageFormat format = TinyImageFormat_R32G32B32_SFLOAT;
            static constexpr uint32_t Stride = sizeof(float3);
            static constexpr ShaderSemantic Semantic = ShaderSemantic::SEMANTIC_TANGENT;
        };
        struct TextureTrait {
            using Type = float2;
            static constexpr TinyImageFormat format = TinyImageFormat_R32G32_SFLOAT;
            static constexpr uint32_t Stride = sizeof(float2);
            static constexpr ShaderSemantic Semantic = ShaderSemantic::SEMANTIC_TEXCOORD0;
        };

        struct StreamBufferInfo {
        public:
            StreamBufferInfo() {
            }

            StreamBufferInfo(const StreamBufferInfo& other):
                m_buffer(other.m_buffer),
                m_semantic(other.m_semantic),
                m_stride(other.m_stride),
                m_numberElements(other.m_numberElements){
            }
            StreamBufferInfo(StreamBufferInfo&& other):
                m_buffer(std::move(other.m_buffer)),
                m_semantic(other.m_semantic),
                m_stride(other.m_stride),
                m_numberElements(other.m_numberElements){
            }

            void operator=(const StreamBufferInfo& other) {
                m_buffer = other.m_buffer;
                m_semantic = other.m_semantic;
                m_stride = other.m_stride;
                m_numberElements = other.m_numberElements;
            }
            void operator=(StreamBufferInfo&& other) {
                m_buffer = std::move(other.m_buffer);
                m_semantic = other.m_semantic;
                m_stride = other.m_stride;
                m_numberElements = other.m_numberElements;
            }

            // utility function to create buffers allows for consistancy for these types of buffers in the engine
            template<typename Trait>
            static void InitializeBuffer(StreamBufferInfo* info, GraphicsBuffer::BufferStructuredView<typename Trait::Type>* view = nullptr) {
                info->m_stride = Trait::Stride;
                info->m_semantic = Trait::Semantic;
                if(view) {
                    (*view) = info->GetStructuredView<typename Trait::Type>();
                }
            }

            template<typename T>
            constexpr GraphicsBuffer::BufferStructuredView<T> GetStructuredView(uint32_t byteOffset = 0) {
                return m_buffer.CreateStructuredView<T>(byteOffset, m_stride);
            }

            GraphicsBuffer m_buffer;
            uint32_t m_stride = 0;
            uint32_t m_numberElements = 0;
            ShaderSemantic m_semantic = ShaderSemantic::SEMANTIC_UNDEFINED;
        };

        struct IndexBufferInfo {
        public:
            IndexBufferInfo() {
            }

            IndexBufferInfo (const IndexBufferInfo& other):
                m_buffer(other.m_buffer),
                m_numberElements(other.m_numberElements){
            }
            IndexBufferInfo(IndexBufferInfo&& other):
                m_buffer(std::move(other.m_buffer)),
                m_numberElements(other.m_numberElements){
            }
            void operator=(const IndexBufferInfo& other) {
                m_buffer = other.m_buffer;
                m_numberElements = other.m_numberElements;
            }
            void operator=(IndexBufferInfo&& other) {
                m_buffer = std::move(other.m_buffer);
                m_numberElements = other.m_numberElements;
            }
            GraphicsBuffer::BufferIndexView GetView() {
                return m_buffer.CreateIndexView();
            }

            uint32_t m_numberElements = 0;
            GraphicsBuffer m_buffer;
        };

        class MeshCollisionResource {
        public:
            eCollideShapeType mType;
            cVector3f mvSize;
            cMatrixf m_mtxOffset;
            bool mbCharCollider;
        };

        static void WriteToVertexBuffer(std::vector<StreamBufferInfo>& vertexBuffers, IndexBufferInfo& indexBuffer, iVertexBuffer* ouput);
        static void ReadFromVertexBuffer(iVertexBuffer* input, std::vector<StreamBufferInfo>& vertexBuffers, IndexBufferInfo& indexBuffer);
        static iCollideShape* CreateCollideShapeFromCollider(
            const MeshCollisionResource& pCollider,
            iPhysicsWorld* apWorld,
            const cVector3f& avSizeMul,
            const std::optional<cMatrixf> apMtxOffset = std::nullopt);
        static iCollideShape* CreateCollideShape(iPhysicsWorld* apWorld, std::span<MeshCollisionResource> resource);

        cSubMesh(const tString& asName, cMaterialManager* apMaterialManager);
        ~cSubMesh();

        void SetMaterial(cMaterial* apMaterial);
        void SetVertexBuffer(iVertexBuffer* apVtxBuffer);

        // Renderable implementation.
        cMaterial* GetMaterial();
        iVertexBuffer* GetVertexBuffer();

        const tString& GetName() {
            return m_name;
        }

        // Vertex-Bone pairs
        void ResizeVertexBonePairs(int alSize);
        int GetVertexBonePairNum();
        cVertexBonePair& GetVertexBonePair(int alNum);

        void AddVertexBonePair(const cVertexBonePair& aPair);
        void ClearVertexBonePairs();

        void AddCollider(const MeshCollisionResource& def);
        std::span<MeshCollisionResource> GetColliders();
        inline std::span<StreamBufferInfo> streamBuffers() { return m_vertexStreams; }
        inline std::span<StreamBufferInfo>::iterator getStreamBySemantic(ShaderSemantic semantic) {
            return std::find_if(streamBuffers().begin(), streamBuffers().end(), [&](auto& stream) {
                return stream.m_semantic == semantic;
            });
        }


        inline IndexBufferInfo& IndexStream() {return m_indexStream; }
        void SetIsCollideShape(bool abX) {
            m_collideShape = abX;
        }
        bool IsCollideShape() {
            return m_collideShape;
        }

        void SetDoubleSided(bool abX) {
            m_doubleSided = abX;
        }
        bool GetDoubleSided() {
            return m_doubleSided;
        }

        inline void SetModelScale(const cVector3f& avScale) {
            m_modelScale = avScale;
        }
        inline cVector3f GetModelScale() {
            return m_modelScale;
        }

        const cMatrixf& GetLocalTransform() {
            return m_mtxLocalTransform;
        }
        void SetLocalTransform(const cMatrixf& a_mtxTrans) {
            m_mtxLocalTransform = a_mtxTrans;
        }

        bool GetIsOneSided() {
            return m_isOneSided;
        }
        const cVector3f& GetOneSidedNormal() {
            return m_oneSidedNormal;
        }
        const cVector3f& GetOneSidedPoint() {
            return m_oneSidedPoint;
        }
        void SetMaterialName(const tString& asName) {
            m_materialName = asName;
        }
        const tString& GetMaterialName() {
            return m_materialName;
        }

        bool hasMesh();
        void SetStreamBuffers(iVertexBuffer* buffer, std::vector<StreamBufferInfo>&& vertexStreams, IndexBufferInfo&& indexStream);
        void Compile();
    private:

        cMaterialManager* m_materialManager = nullptr;
        tString m_name;

        tString m_materialName;
        cMaterial* m_material = nullptr;
        iVertexBuffer* m_vtxBuffer = nullptr;

        cMatrixf m_mtxLocalTransform = cMatrixf::Identity;

        std::vector<cVertexBonePair> m_vtxBonePairs;
        folly::small_vector<MeshCollisionResource, 3> m_colliders;

        std::vector<StreamBufferInfo> m_vertexStreams;
        IndexBufferInfo m_indexStream;

        std::vector<float> m_vertexWeights;
        std::vector<uint8_t> m_vertexBones;

        cVector3f m_modelScale;
        bool m_doubleSided = false;
        bool m_collideShape = false;
        bool m_isOneSided = false;
        cVector3f m_oneSidedNormal = 0;
        cVector3f m_oneSidedPoint = 0;

        cMesh* m_parent;
    };

}; // namespace hpl
