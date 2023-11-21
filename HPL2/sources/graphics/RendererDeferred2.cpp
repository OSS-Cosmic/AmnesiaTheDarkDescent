#include "graphics/RendererDeferred2.h"
#include "Common_3/Graphics/Interfaces/IGraphics.h"
#include "Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "Common_3/Utilities/ThirdParty/OpenSource/ModifiedSonyMath/common.hpp"
#include "graphics/DrawPacket.h"
#include "graphics/ForgeRenderer.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/SceneResource.h"
#include "graphics/TextureDescriptorPool.h"
#include "scene/RenderableContainer.h"
#include "tinyimageformat_base.h"
#include <memory>

namespace hpl {
    struct RangeSubsetAlloc {
    public:
        struct RangeSubset {
            uint32_t m_start;
            uint32_t m_end;
            inline uint32_t size() { return m_end - m_start; }

        };
        uint32_t& m_index;
        uint32_t m_start;
        RangeSubsetAlloc(uint32_t& index): m_index(index), m_start(index) {
        }
        uint32_t Increment() {
            return (m_index++);
        }
        RangeSubset End() {
            uint32_t start = m_start;
            m_start = m_index;
            return RangeSubset{start, m_index};
        }
    };

    cRendererDeferred2::cRendererDeferred2(cGraphics* apGraphics, cResources* apResources, std::shared_ptr<DebugDraw> debug)
        : iRenderer("Deferred2", apGraphics, apResources) {
        m_sceneTexture2DPool = TextureDescriptorPool(ForgeRenderer::SwapChainLength, resource::MaxSceneTextureCount);
        // Indirect Argument signature
        auto* forgeRenderer = Interface<ForgeRenderer>::Get();
        {
            std::array<IndirectArgumentDescriptor, 1> indirectArgs = {};
            {
                IndirectArgumentDescriptor arg{};
                arg.mType = INDIRECT_DRAW_INDEX;
                indirectArgs[0] = arg;
            }
            CommandSignatureDesc vbPassDesc = { m_sceneRootSignature.m_handle, indirectArgs.data(), indirectArgs.size() };
            vbPassDesc.mPacked = true;
            addIndirectCommandSignature(forgeRenderer->Rend(), &vbPassDesc, &m_cmdSignatureVBPass);
        }
        m_nearEdgeClamp.Load(forgeRenderer->Rend(), [&](Sampler** sampler) {
            SamplerDesc bilinearClampDesc = { FILTER_NEAREST,
                                              FILTER_NEAREST,
                                              MIPMAP_MODE_NEAREST,
                                              ADDRESS_MODE_CLAMP_TO_EDGE,
                                              ADDRESS_MODE_CLAMP_TO_EDGE,
                                              ADDRESS_MODE_CLAMP_TO_EDGE };
            addSampler(forgeRenderer->Rend(), &bilinearClampDesc, sampler);
            return true;
        });

        m_emptyTexture.Load([&](Texture** texture) {
            TextureDesc textureDesc = {};
            textureDesc.mArraySize = 1;
            textureDesc.mMipLevels = 1;
            textureDesc.mDepth = 1;
            textureDesc.mFormat = TinyImageFormat_R8G8B8A8_UNORM;
            textureDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
            textureDesc.mWidth = 16;
            textureDesc.mHeight = 16;
            textureDesc.mSampleCount = SAMPLE_COUNT_1;
            textureDesc.mStartState = RESOURCE_STATE_COMMON;
            textureDesc.pName = "empty texture";
            TextureLoadDesc loadDesc = {};
            loadDesc.ppTexture = texture;
            loadDesc.pDesc = &textureDesc;
            addResource(&loadDesc, nullptr);
            return true;
        });
        m_materialSampler = hpl::resource::createSceneSamplers(forgeRenderer->Rend());
        m_visibilityBufferPassShader.Load(forgeRenderer->Rend(), [&](Shader** shader) {
            ShaderLoadDesc loadDesc = {};
            loadDesc.mStages[0].pFileName = "visibilityBuffer_pass.vert";
            loadDesc.mStages[1].pFileName = "visibilityBuffer_pass.frag";
            addShader(forgeRenderer->Rend(), &loadDesc, shader);
            return true;
        });

        m_visibilityBufferAlphaPassShader.Load(forgeRenderer->Rend(), [&](Shader** shader) {
            ShaderLoadDesc loadDesc = {};
            loadDesc.mStages[0].pFileName = "visibilityBuffer_alpha_pass.vert";
            loadDesc.mStages[1].pFileName = "visibilityBuffer_alpha_pass.frag";
            addShader(forgeRenderer->Rend(), &loadDesc, shader);
            return true;
        });
        m_visibilityShadePassShader.Load(forgeRenderer->Rend(), [&](Shader** shader) {
            ShaderLoadDesc loadDesc = {};
            loadDesc.mStages[0].pFileName = "visibility_shade_pass.vert";
            loadDesc.mStages[1].pFileName = "visibility_shade_pass.frag";
            addShader(forgeRenderer->Rend(), &loadDesc, shader);

            return true;
        });
        m_sceneRootSignature.Load(forgeRenderer->Rend(), [&](RootSignature** signature) {
            std::array shaders = { m_visibilityBufferAlphaPassShader.m_handle ,m_visibilityBufferPassShader.m_handle, m_visibilityShadePassShader.m_handle };
            Sampler* vbShadeSceneSamplers[] = { m_nearEdgeClamp.m_handle };
            const char* vbShadeSceneSamplersNames[] = { "linearBorderClamp" };
            RootSignatureDesc rootSignatureDesc = {};
            rootSignatureDesc.ppShaders = shaders.data();
            rootSignatureDesc.mShaderCount = shaders.size();
            rootSignatureDesc.mStaticSamplerCount = std::size(vbShadeSceneSamplersNames);
            rootSignatureDesc.ppStaticSamplers = vbShadeSceneSamplers;
            rootSignatureDesc.ppStaticSamplerNames = vbShadeSceneSamplersNames;
            addRootSignature(forgeRenderer->Rend(), &rootSignatureDesc, signature);
            return true;
        });
        m_visiblityShadePass.Load(forgeRenderer->Rend(), [&](Pipeline** pipeline) {
            std::array colorFormats = {
                ColorBufferFormat,
                TinyImageFormat_R16G16B16A16_SFLOAT
            };
            RasterizerStateDesc rasterizerStateDesc = {};
            rasterizerStateDesc.mCullMode = CULL_MODE_FRONT;
            rasterizerStateDesc.mFrontFace = FRONT_FACE_CCW;

            DepthStateDesc depthStateDisabledDesc = {};
            depthStateDisabledDesc.mDepthWrite = false;
            depthStateDisabledDesc.mDepthTest = false;

            PipelineDesc pipelineDesc = {};
            pipelineDesc.mType = PIPELINE_TYPE_GRAPHICS;
            auto& pipelineSettings = pipelineDesc.mGraphicsDesc;
            pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
            pipelineSettings.mRenderTargetCount = colorFormats.size();
            pipelineSettings.pColorFormats = colorFormats.data();
            pipelineSettings.mSampleCount = SAMPLE_COUNT_1;
            pipelineSettings.mSampleQuality = 0;
            pipelineSettings.pRootSignature = m_sceneRootSignature.m_handle;
            pipelineSettings.pShaderProgram = m_visibilityShadePassShader.m_handle;
            pipelineSettings.pRasterizerState = &rasterizerStateDesc;
            pipelineSettings.pDepthState = &depthStateDisabledDesc;
            pipelineSettings.pVertexLayout = nullptr;
            addPipeline(forgeRenderer->Rend(), &pipelineDesc, pipeline);
            return true;
        });
        m_visbilityAlphaBufferPass.Load(forgeRenderer->Rend(), [&](Pipeline** pipeline) {
            VertexLayout vertexLayout = {};
            vertexLayout.mBindingCount = 4;
            vertexLayout.mBindings[0] = {.mStride = sizeof(float3)};
            vertexLayout.mBindings[1] = {.mStride = sizeof(float2)};
            vertexLayout.mBindings[2] = {.mStride = sizeof(float3)};
            vertexLayout.mBindings[3] = {.mStride = sizeof(float3)};
            vertexLayout.mAttribCount = 4;
            vertexLayout.mAttribs[0] = VertexAttrib {
                .mSemantic = SEMANTIC_POSITION,
                .mFormat = TinyImageFormat_R32G32B32_SFLOAT,
                .mBinding = 0,
                .mLocation = 0,
                .mOffset = 0
            };
            vertexLayout.mAttribs[1] = VertexAttrib {
                .mSemantic = SEMANTIC_TEXCOORD0,
                .mFormat = TinyImageFormat_R32G32_SFLOAT,
                .mBinding = 1,
                .mLocation = 1,
                .mOffset = 0
            };
            vertexLayout.mAttribs[2] = VertexAttrib {
                .mSemantic = SEMANTIC_NORMAL,
                .mFormat = TinyImageFormat_R32G32B32_SFLOAT,
                .mBinding = 2,
                .mLocation = 2,
                .mOffset = 0
            };
            vertexLayout.mAttribs[3] = VertexAttrib {
                .mSemantic = SEMANTIC_TANGENT,
                .mFormat = TinyImageFormat_R32G32B32_SFLOAT,
                .mBinding = 3,
                .mLocation = 3,
                .mOffset = 0
            };
            std::array colorFormats = { ColorBufferFormat, TinyImageFormat_R8G8_UNORM };
            DepthStateDesc depthStateDesc = {
                .mDepthTest = true,
                .mDepthWrite = true,
                .mDepthFunc = CMP_LEQUAL
            };

            RasterizerStateDesc rasterizerStateDesc = {};
            rasterizerStateDesc.mCullMode = CULL_MODE_FRONT;
            rasterizerStateDesc.mFrontFace = FRONT_FACE_CCW;

            PipelineDesc pipelineDesc = {};
            pipelineDesc.mType = PIPELINE_TYPE_GRAPHICS;
            auto& pipelineSettings = pipelineDesc.mGraphicsDesc;
            pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
            pipelineSettings.mRenderTargetCount = colorFormats.size();
            pipelineSettings.pColorFormats = colorFormats.data();
            pipelineSettings.pDepthState = &depthStateDesc;
            pipelineSettings.mSampleCount = SAMPLE_COUNT_1;
            pipelineSettings.mDepthStencilFormat = DepthBufferFormat;
            pipelineSettings.mSampleQuality = 0;
            pipelineSettings.pRootSignature = m_sceneRootSignature.m_handle;
            pipelineSettings.pShaderProgram = m_visibilityBufferAlphaPassShader.m_handle;
            pipelineSettings.pRasterizerState = &rasterizerStateDesc;
            pipelineSettings.pVertexLayout = &vertexLayout;
            pipelineSettings.mSupportIndirectCommandBuffer = true;
            addPipeline(forgeRenderer->Rend(), &pipelineDesc, pipeline);
            return true;
        });
        m_visibilityBufferPass.Load(forgeRenderer->Rend(), [&](Pipeline** pipeline) {
            VertexLayout vertexLayout = {};
            vertexLayout.mBindingCount = 1;
            vertexLayout.mBindings[0] = {
                .mStride = sizeof(float3)
            };
            vertexLayout.mAttribCount = 1;
            vertexLayout.mAttribs[0] = VertexAttrib {
                .mSemantic = SEMANTIC_POSITION,
                .mFormat = TinyImageFormat_R32G32B32_SFLOAT,
                .mBinding = 0,
                .mLocation = 0,
                .mOffset = 0
            };
            std::array colorFormats = { ColorBufferFormat };
            DepthStateDesc depthStateDesc = {
                .mDepthTest = true,
                .mDepthWrite = true,
                .mDepthFunc = CMP_LEQUAL
            };

            RasterizerStateDesc rasterizerStateDesc = {};
            rasterizerStateDesc.mCullMode = CULL_MODE_FRONT;
            rasterizerStateDesc.mFrontFace = FRONT_FACE_CCW;

            PipelineDesc pipelineDesc = {};
            pipelineDesc.mType = PIPELINE_TYPE_GRAPHICS;
            auto& pipelineSettings = pipelineDesc.mGraphicsDesc;
            pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
            pipelineSettings.mRenderTargetCount = colorFormats.size();
            pipelineSettings.pColorFormats = colorFormats.data();
            pipelineSettings.pDepthState = &depthStateDesc;
            pipelineSettings.mSampleCount = SAMPLE_COUNT_1;
            pipelineSettings.mDepthStencilFormat = DepthBufferFormat;
            pipelineSettings.mSampleQuality = 0;
            pipelineSettings.pRootSignature = m_sceneRootSignature.m_handle;
            pipelineSettings.pShaderProgram = m_visibilityBufferPassShader.m_handle;
            pipelineSettings.pRasterizerState = &rasterizerStateDesc;
            pipelineSettings.pVertexLayout = &vertexLayout;
            pipelineSettings.mSupportIndirectCommandBuffer = true;
            addPipeline(forgeRenderer->Rend(), &pipelineDesc, pipeline);
            return true;
        });


        for (size_t i = 0; i < ForgeRenderer::SwapChainLength; i++) {
            ASSERT(m_objectUniformBuffer.size() == ForgeRenderer::SwapChainLength);
            ASSERT(m_perSceneInfoBuffer.size() == ForgeRenderer::SwapChainLength);
            ASSERT(m_indirectDrawArgsBuffer.size() == ForgeRenderer::SwapChainLength);

            m_objectUniformBuffer[i].Load([&](Buffer** buffer) {
                BufferLoadDesc desc = {};
                desc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER;
                desc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
                desc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
                desc.mDesc.mFirstElement = 0;
                desc.mDesc.mElementCount = MaxObjectUniforms;
                desc.mDesc.mStructStride = sizeof(hpl::resource::SceneObject);
                desc.mDesc.mSize = desc.mDesc.mElementCount * desc.mDesc.mStructStride;
                desc.ppBuffer = buffer;
                addResource(&desc, nullptr);
                return true;
            });
            m_perSceneInfoBuffer[i].Load([&](Buffer** buffer) {
                BufferLoadDesc desc = {};
                desc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                desc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
                desc.mDesc.mElementCount = 1;
                desc.mDesc.mStructStride = sizeof(hpl::resource::SceneInfoResource);
                desc.mDesc.mSize = sizeof(hpl::resource::SceneInfoResource);
                desc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
                desc.pData = nullptr;
                desc.ppBuffer = buffer;
                addResource(&desc, nullptr);
                return true;
            });

            m_indirectDrawArgsBuffer[i].Load([&](Buffer** buffer) {
                BufferLoadDesc indirectBufferDesc = {};
                indirectBufferDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_INDIRECT_BUFFER | DESCRIPTOR_TYPE_BUFFER;
                indirectBufferDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_TO_CPU;
                indirectBufferDesc.mDesc.mElementCount = MaxIndirectDrawArgs;
                indirectBufferDesc.mDesc.mStructStride = sizeof(IndirectDrawIndexArguments);
                indirectBufferDesc.mDesc.mStartState = RESOURCE_STATE_SHADER_RESOURCE | RESOURCE_STATE_INDIRECT_ARGUMENT;
                indirectBufferDesc.mDesc.mSize = indirectBufferDesc.mDesc.mElementCount * indirectBufferDesc.mDesc.mStructStride;
                indirectBufferDesc.mDesc.pName = "Indirect Buffer";
                indirectBufferDesc.ppBuffer = buffer;
                addResource(&indirectBufferDesc, NULL);
                return true;
            });

            m_diffuseSolidMaterialUniformBuffer.Load([&](Buffer** buffer) {
                BufferLoadDesc desc = {};
                desc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER;
                desc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
                desc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
                desc.mDesc.mFirstElement = 0;
                desc.mDesc.mElementCount = MaxSolidDiffuseMaterials;
                desc.mDesc.mStructStride = sizeof(resource::DiffuseMaterial);
                desc.mDesc.mSize = desc.mDesc.mElementCount * desc.mDesc.mStructStride;
                desc.ppBuffer = buffer;
                addResource(&desc, nullptr);
                return true;
            });
        }
        m_diffuseIndexPool = IndexPool(MaxSolidDiffuseMaterials);

        // create Descriptor sets
        m_sceneDescriptorConstSet.Load(forgeRenderer->Rend(), [&](DescriptorSet** descSet) {
            DescriptorSetDesc descriptorSetDesc{ m_sceneRootSignature.m_handle, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
            addDescriptorSet(forgeRenderer->Rend(), &descriptorSetDesc, descSet);
            return true;
        });
        for (size_t i = 0; i < ForgeRenderer::SwapChainLength; i++) {
            m_sceneDescriptorPerFrameSet[i].Load(forgeRenderer->Rend(), [&](DescriptorSet** descSet) {
                DescriptorSetDesc descriptorSetDesc{ m_sceneRootSignature.m_handle, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, 1 };
                addDescriptorSet(forgeRenderer->Rend(), &descriptorSetDesc, descSet);
                return true;
            });
        }

        // update descriptorSet
        for (size_t swapchainIndex = 0; swapchainIndex < ForgeRenderer::SwapChainLength; swapchainIndex++) {
            std::array params = {
                DescriptorData{ .pName = "sceneObjects", .ppBuffers = &m_objectUniformBuffer[swapchainIndex].m_handle },
                DescriptorData{ .pName = "sceneInfo", .ppBuffers = &m_perSceneInfoBuffer[swapchainIndex].m_handle },
                DescriptorData{ .pName = "indirectDrawArgs", .ppBuffers = &m_indirectDrawArgsBuffer[swapchainIndex].m_handle },
            };
            updateDescriptorSet(forgeRenderer->Rend(), 0,
                               m_sceneDescriptorPerFrameSet[swapchainIndex].m_handle, params.size(), params.data());
        }
        {
            std::array<Sampler*, resource::MaterailSamplerNonAntistropyCount> samplers;
            for (size_t wrapIdx = 0; wrapIdx < eTextureWrap_LastEnum; wrapIdx++) {
                for (size_t filterIdx = 0; filterIdx < eTextureFilter_LastEnum; filterIdx++) {
                    samplers[resource::textureFilterNonAnistropyIdx(
                        static_cast<eTextureWrap>(wrapIdx), static_cast<eTextureFilter>(filterIdx))] =
                        m_materialSampler[resource::textureFilterIdx(
                                              TextureAntistropy::Antistropy_None,
                                              static_cast<eTextureWrap>(wrapIdx),
                                              static_cast<eTextureFilter>(filterIdx))]
                            .m_handle;
                }
            }
            auto* graphicsAllocator = Interface<GraphicsAllocator>::Get();
            auto& opaqueSet = graphicsAllocator->resolveSet(GraphicsAllocator::AllocationSet::OpaqueSet);

            std::array params = {
                DescriptorData{ .pName = "sceneDiffuseMat", .ppBuffers = &m_diffuseSolidMaterialUniformBuffer.m_handle },
                DescriptorData{ .pName = "sceneFilters", .mCount = samplers.size(), .ppSamplers = samplers.data()},
                DescriptorData{ .pName = "vtxOpaqueIndex", .ppBuffers = &opaqueSet.indexBuffer().m_handle },
                DescriptorData{ .pName = "vtxOpaquePosition", .ppBuffers = &opaqueSet.getStreamBySemantic(ShaderSemantic::SEMANTIC_POSITION)->buffer().m_handle },
                DescriptorData{ .pName = "vtxOpaqueTangnet", .ppBuffers  = &opaqueSet.getStreamBySemantic(ShaderSemantic::SEMANTIC_TANGENT)->buffer().m_handle },
                DescriptorData{ .pName = "vtxOpaqueNormal", .ppBuffers  = &opaqueSet.getStreamBySemantic(ShaderSemantic::SEMANTIC_NORMAL)->buffer().m_handle },
                DescriptorData{ .pName = "vtxOpaqueUv", .ppBuffers = &opaqueSet.getStreamBySemantic(ShaderSemantic::SEMANTIC_TEXCOORD0)->buffer().m_handle  },
                //DescriptorData{ .pName = "vtxOpaqueColor", .ppBuffers = &opaqueSet.getStreamBySemantic(ShaderSemantic::SEMANTIC_COLOR)->buffer().m_handle },
            };
            updateDescriptorSet(forgeRenderer->Rend(), 0, m_sceneDescriptorConstSet.m_handle, params.size(), params.data());
        }
    }

    cRendererDeferred2::~cRendererDeferred2() {
    }

    cRendererDeferred2::SceneMaterial& cRendererDeferred2::resolveMaterial(const ForgeRenderer::Frame& frame, cMaterial* material) {
        auto* forgeRenderer = Interface<ForgeRenderer>::Get();
        auto& sceneMaterial = m_sceneMaterial[material->Index()];
        if (sceneMaterial.m_material != material || sceneMaterial.m_version != material->Generation()) {
            auto& descriptor = material->Descriptor();
            sceneMaterial.m_version = material->Generation();
            sceneMaterial.m_material = material;

            resource::visitTextures(sceneMaterial.m_resource, [&](eMaterialTexture texture, uint32_t slot) {
                m_sceneTexture2DPool.dispose(slot);
            });
            sceneMaterial.m_resource = hpl::resource::createMaterial(m_sceneTexture2DPool, material);
            if (resource::DiffuseMaterial* mat = std::get_if<resource::DiffuseMaterial>(&sceneMaterial.m_resource)) {
                sceneMaterial.m_slot = IndexPoolHandle(&m_diffuseIndexPool);
                BufferUpdateDesc updateDesc = { m_diffuseSolidMaterialUniformBuffer.m_handle,
                                                sizeof(resource::DiffuseMaterial) * sceneMaterial.m_slot.get() };
                beginUpdateResource(&updateDesc);
                (*reinterpret_cast<resource::DiffuseMaterial*>(updateDesc.pMappedData)) = *mat;
                endUpdateResource(&updateDesc);
            }
        }
        return sceneMaterial;
    }

    uint32_t cRendererDeferred2::resolveObjectIndex(
        const ForgeRenderer::Frame& frame, iRenderable* apObject, uint32_t drawArgOffset, std::optional<Matrix4> modelMatrix) {
        auto* forgeRenderer = Interface<ForgeRenderer>::Get();
        cMaterial* material =  apObject->GetMaterial();

        ASSERT(material);

        uint32_t id = folly::hash::fnv32_buf(
            reinterpret_cast<const char*>(apObject),
            sizeof(apObject),
            modelMatrix.has_value() ? folly::hash::fnv32_buf(&modelMatrix, sizeof(modelMatrix)) : folly::hash::fnv32_hash_start);

        auto objectLookup = m_objectDescriptorLookup.find(apObject);
        const bool isFound = objectLookup != m_objectDescriptorLookup.end();
        uint32_t index = isFound ? objectLookup->second : m_objectIndex++;
        if (!isFound) {
            Matrix4 modelMat = modelMatrix.value_or(apObject->GetModelMatrixPtr() ? cMath::ToForgeMatrix4(*apObject->GetModelMatrixPtr()) : Matrix4::identity());

            auto& sceneMaterial = resolveMaterial(frame, material);
            BufferUpdateDesc updateDesc = { m_objectUniformBuffer[frame.m_frameIndex].m_handle,
                                            sizeof(resource::SceneObject) * index };
            beginUpdateResource(&updateDesc);
            auto& uniformObjectData = (*reinterpret_cast<resource::SceneObject*>(updateDesc.pMappedData));
            uniformObjectData.m_dissolveAmount = apObject->GetCoverageAmount();
            uniformObjectData.m_modelMat = modelMat;
            uniformObjectData.m_invModelMat = inverse(modelMat);
            uniformObjectData.m_lightLevel = 1.0f;
            uniformObjectData.m_illuminationAmount = apObject->GetIlluminationAmount();
            uniformObjectData.m_materialId = resource::encodeMaterialID(material->Descriptor().m_id, sceneMaterial.m_slot.get());
            uniformObjectData.m_indirectOffset = drawArgOffset; // we only care about the first offset
            if(material) {
                uniformObjectData.m_uvMat = cMath::ToForgeMatrix4(material->GetUvMatrix().GetTranspose());
            } else {
                uniformObjectData.m_uvMat = Matrix4::identity();
            }
            endUpdateResource(&updateDesc);
            m_objectDescriptorLookup[apObject] = index;
        }
        return index;
    }

    SharedRenderTarget cRendererDeferred2::GetOutputImage(uint32_t frameIndex, cViewport& viewport) {
        auto sharedData = m_boundViewportData.resolve(viewport);
        if (!sharedData) {
            return SharedRenderTarget();
        }
        return sharedData->m_outputBuffer[frameIndex];

    }

    void cRendererDeferred2::Draw(
        Cmd* cmd,
        const ForgeRenderer::Frame& frame,
        cViewport& viewport,
        float frameTime,
        cFrustum* apFrustum,
        cWorld* apWorld,
        cRenderSettings* apSettings,
        bool abSendFrameBufferToPostEffects) {
        auto* forgeRenderer = Interface<ForgeRenderer>::Get();
        auto* graphicsAllocator = Interface<GraphicsAllocator>::Get();
        if (frame.m_currentFrame != m_activeFrame) {
            m_objectIndex = 0;
            m_sceneTexture2DPool.reset([&](TextureDescriptorPool::Action action, uint32_t slot, SharedTexture& texture) {
                std::array<DescriptorData, 1> params = {
                    DescriptorData {.pName = "sceneTextures", .mCount = 1, .mArrayOffset = slot, .ppTextures = (action == TextureDescriptorPool::Action::UpdateSlot ? &texture.m_handle: &m_emptyTexture.m_handle ) }
                };
                updateDescriptorSet(
                    forgeRenderer->Rend(),
                    0,
                    m_sceneDescriptorPerFrameSet[frame.m_frameIndex].m_handle,
                    params.size(),
                    params.data());
            });
            m_objectDescriptorLookup.clear();
            m_indirectDrawIndex = 0;
            m_objectIndex = 0;
            m_activeFrame = frame.m_currentFrame;
        }

        auto viewportDatum = m_boundViewportData.resolve(viewport);
        if(!viewportDatum || viewportDatum->m_size != viewport.GetSizeU2()) {
            auto updateDatum = std::make_unique<ViewportData>();
            updateDatum->m_size = viewport.GetSizeU2();
            for(size_t i = 0; i < ForgeRenderer::SwapChainLength; i++) {
                updateDatum->m_outputBuffer[i].Load(forgeRenderer->Rend(), [&](RenderTarget** target) {
                    ClearValue optimizedColorClearBlack = { { 0.0f, 0.0f, 0.0f, 0.0f } };
                    RenderTargetDesc renderTargetDesc = {};
                    renderTargetDesc.mArraySize = 1;
                    renderTargetDesc.mClearValue = optimizedColorClearBlack;
                    renderTargetDesc.mDepth = 1;
                    renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
                    renderTargetDesc.mWidth = updateDatum->m_size.x;
                    renderTargetDesc.mHeight = updateDatum->m_size.y;
                    renderTargetDesc.mSampleCount = SAMPLE_COUNT_1;
                    renderTargetDesc.mSampleQuality = 0;
                    renderTargetDesc.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
                    renderTargetDesc.mFormat = TinyImageFormat_R8G8B8A8_UNORM;
                    renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
                    renderTargetDesc.pName = "output RT";
                    addRenderTarget(forgeRenderer->Rend(), &renderTargetDesc, target);
                    return true;
                });

                updateDatum->m_testBuffer[i].Load(forgeRenderer->Rend(), [&](RenderTarget** target) {
                    RenderTargetDesc renderTargetDesc = {};
                    renderTargetDesc.mArraySize = 1;
                    renderTargetDesc.mClearValue = { .depth = 1.0f, .stencil = 0 };
                    renderTargetDesc.mDepth = 1;
                    renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
                    renderTargetDesc.mWidth = updateDatum->m_size.x;
                    renderTargetDesc.mHeight = updateDatum->m_size.y;
                    renderTargetDesc.mSampleCount = SAMPLE_COUNT_1;
                    renderTargetDesc.mSampleQuality = 0;
                    renderTargetDesc.mFormat = TinyImageFormat_R16G16B16A16_SFLOAT;
                    renderTargetDesc.mStartState = RESOURCE_STATE_RENDER_TARGET;
                    renderTargetDesc.pName = "parallax RT";
                    addRenderTarget(forgeRenderer->Rend(), &renderTargetDesc, target);
                    return true;
                });
                updateDatum->m_depthBuffer[i].Load(forgeRenderer->Rend(), [&](RenderTarget** target) {
                    RenderTargetDesc renderTargetDesc = {};
                    renderTargetDesc.mArraySize = 1;
                    renderTargetDesc.mClearValue = { .depth = 1.0f, .stencil = 0 };
                    renderTargetDesc.mDepth = 1;
                    renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
                    renderTargetDesc.mWidth = updateDatum->m_size.x;
                    renderTargetDesc.mHeight = updateDatum->m_size.y;
                    renderTargetDesc.mSampleCount = SAMPLE_COUNT_1;
                    renderTargetDesc.mSampleQuality = 0;
                    renderTargetDesc.mFormat = DepthBufferFormat;
                    renderTargetDesc.mStartState = RESOURCE_STATE_DEPTH_WRITE;
                    renderTargetDesc.pName = "Depth RT";
                    addRenderTarget(forgeRenderer->Rend(), &renderTargetDesc, target);
                    return true;
                });

                updateDatum->m_visiblityBuffer[i].Load(forgeRenderer->Rend(), [&](RenderTarget** target) {
                    ClearValue optimizedColorClearBlack = { { 0.0f, 0.0f, 0.0f, 0.0f } };
                    RenderTargetDesc renderTargetDesc = {};
                    renderTargetDesc.mArraySize = 1;
                    renderTargetDesc.mClearValue = optimizedColorClearBlack;
                    renderTargetDesc.mDepth = 1;
                    renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
                    renderTargetDesc.mWidth = updateDatum->m_size.x;
                    renderTargetDesc.mHeight = updateDatum->m_size.y;
                    renderTargetDesc.mSampleCount = SAMPLE_COUNT_1;
                    renderTargetDesc.mSampleQuality = 0;
                    renderTargetDesc.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
                    renderTargetDesc.mFormat = TinyImageFormat_R8G8B8A8_UNORM;
                    renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
                    addRenderTarget(forgeRenderer->Rend(), &renderTargetDesc, target);
                    return true;
                });
            }
            viewportDatum = m_boundViewportData.update(viewport, std::move(updateDatum));
        }
        {
            std::array params = {
                DescriptorData{ .pName = "visibilityTexture",
                                .ppTextures = &viewportDatum->m_visiblityBuffer[frame.m_frameIndex].m_handle->pTexture },
            };
            updateDescriptorSet(
                forgeRenderer->Rend(), 0, m_sceneDescriptorPerFrameSet[frame.m_frameIndex].m_handle, params.size(), params.data());
        }
        {
            BufferUpdateDesc updateDesc = {m_perSceneInfoBuffer[frame.m_frameIndex].m_handle, 0,
                                           sizeof(resource::SceneInfoResource)};
            beginUpdateResource(&updateDesc);
            resource::SceneInfoResource* sceneInfo = reinterpret_cast<resource::SceneInfoResource*>(updateDesc.pMappedData);
            const auto fogColor = apWorld->GetFogColor();
            sceneInfo->m_worldInfo.m_fogColor = float4(fogColor.r, fogColor.g, fogColor.b, fogColor.a);
            sceneInfo->m_worldInfo.m_worldFogStart = apWorld->GetFogStart();
            sceneInfo->m_worldInfo.m_worldFogLength = apWorld->GetFogEnd() - apWorld->GetFogStart();
            sceneInfo->m_worldInfo.m_oneMinusFogAlpha = 1.0f - apWorld->GetFogColor().a;
            sceneInfo->m_worldInfo.m_fogFalloffExp = apWorld->GetFogFalloffExp();

            auto& primaryViewport = sceneInfo->m_viewports[resource::ViewportInfo::PrmaryViewportIndex];
            primaryViewport.m_viewMat = apFrustum->GetViewMat();
            primaryViewport.m_projMat = apFrustum->GetProjectionMat();
            primaryViewport.m_invViewMat = inverse(apFrustum->GetViewMat());
            primaryViewport.m_invViewProj= inverse(primaryViewport.m_projMat * primaryViewport.m_viewMat);
            primaryViewport.m_zFar = apFrustum->GetFarPlane();
            primaryViewport.m_zNear = apFrustum->GetNearPlane();
            primaryViewport.m_rect = float4(0.0f,0.0f, static_cast<float>(viewportDatum->m_size.x), static_cast<float>(viewportDatum->m_size.y));
            primaryViewport.m_cameraPosition = v3ToF3(cMath::ToForgeVec3(apFrustum->GetOrigin()));
            endUpdateResource(&updateDesc);
        }

        {
            m_rendererList.BeginAndReset(frameTime, apFrustum);
            auto* dynamicContainer = apWorld->GetRenderableContainer(eWorldContainerType_Dynamic);
            auto* staticContainer = apWorld->GetRenderableContainer(eWorldContainerType_Static);
            dynamicContainer->UpdateBeforeRendering();
            staticContainer->UpdateBeforeRendering();

            auto prepareObjectHandler = [&](iRenderable* pObject) {
                if (!iRenderable::IsObjectIsVisible(*pObject, eRenderableFlag_VisibleInNonReflection, {})) {
                    return;
                }
                m_rendererList.AddObject(pObject);
            };
            iRenderableContainer::WalkRenderableContainer(*dynamicContainer, apFrustum, prepareObjectHandler, eRenderableFlag_VisibleInNonReflection);
            iRenderableContainer::WalkRenderableContainer(*staticContainer, apFrustum, prepareObjectHandler, eRenderableFlag_VisibleInNonReflection);
            m_rendererList.End(
                eRenderListCompileFlag_Diffuse | eRenderListCompileFlag_Translucent | eRenderListCompileFlag_Decal |
                eRenderListCompileFlag_Illumination | eRenderListCompileFlag_FogArea);
        }

        std::vector<iRenderable*> translucenctRenderables;
        RangeSubsetAlloc indirectAllocSession(m_indirectDrawIndex);

        for(auto& renderable: m_rendererList.GetSolidObjects()) {
            cMaterial* material = renderable->GetMaterial();
            if(!material ||
                renderable->GetCoverageAmount() < 1.0 ||
                cMaterial::IsTranslucent(material->Descriptor().m_id) ||
                material->GetImage(eMaterialTexture_Alpha)) {
                translucenctRenderables.push_back(renderable);
                continue;
            }

            DrawPacket packet = renderable->ResolveDrawPacket(frame);
            if(packet.m_type == DrawPacket::Unknown) {
                continue;
            }

            ASSERT(material->Descriptor().m_id == MaterialID::SolidDiffuse && "Invalid material type");
            ASSERT(packet.m_type == DrawPacket::DrawPacketType::DrawGeometryset);
            ASSERT(packet.m_unified.m_set == GraphicsAllocator::AllocationSet::OpaqueSet);
            BufferUpdateDesc updateDesc = {m_indirectDrawArgsBuffer[frame.m_frameIndex].m_handle,  indirectAllocSession.Increment() * sizeof(IndirectDrawIndexArguments),
                                           sizeof(IndirectDrawIndexArguments)  };
            beginUpdateResource(&updateDesc);
            auto* indirectDrawArgs = reinterpret_cast<IndirectDrawIndexArguments*>(updateDesc.pMappedData);
            indirectDrawArgs->mIndexCount = packet.m_unified.m_numIndices;
            indirectDrawArgs->mStartIndex = packet.m_unified.m_subAllocation->indexOffset();
            indirectDrawArgs->mVertexOffset = packet.m_unified.m_subAllocation->vertextOffset();
            indirectDrawArgs->mStartInstance = resolveObjectIndex(frame, renderable, updateDesc.mDstOffset / sizeof(uint32_t), {}); // for DX12 this won't work
            indirectDrawArgs->mInstanceCount = 1;
            endUpdateResource(&updateDesc);
        }
        RangeSubsetAlloc::RangeSubset opaqueIndirectArgs = indirectAllocSession.End();

        for(auto& renderable: translucenctRenderables) {
            cMaterial* material = renderable->GetMaterial();
            if(!material) {
                continue;
            }
            DrawPacket packet = renderable->ResolveDrawPacket(frame);
            if(packet.m_type == DrawPacket::Unknown) {
                continue;
            }

            ASSERT(material->Descriptor().m_id == MaterialID::SolidDiffuse && "Invalid material type");
            ASSERT(packet.m_type == DrawPacket::DrawPacketType::DrawGeometryset);
            ASSERT(packet.m_unified.m_set == GraphicsAllocator::AllocationSet::OpaqueSet);
            BufferUpdateDesc updateDesc = {m_indirectDrawArgsBuffer[frame.m_frameIndex].m_handle,  indirectAllocSession.Increment() * sizeof(IndirectDrawIndexArguments),
                                           sizeof(IndirectDrawIndexArguments)  };
            beginUpdateResource(&updateDesc);
            auto* indirectDrawArgs = reinterpret_cast<IndirectDrawIndexArguments*>(updateDesc.pMappedData);
            indirectDrawArgs->mIndexCount = packet.m_unified.m_numIndices;
            indirectDrawArgs->mStartIndex = packet.m_unified.m_subAllocation->indexOffset();
            indirectDrawArgs->mVertexOffset = packet.m_unified.m_subAllocation->vertextOffset();
            indirectDrawArgs->mStartInstance = resolveObjectIndex(frame, renderable, updateDesc.mDstOffset / sizeof(uint32_t), {}); // for DX12 this won't work
            indirectDrawArgs->mInstanceCount = 1;
            endUpdateResource(&updateDesc);

        }
        RangeSubsetAlloc::RangeSubset translucentIndirectArgs = indirectAllocSession.End();

        {
            cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
            std::array rtBarriers = {
                RenderTargetBarrier{ viewportDatum->m_visiblityBuffer[frame.m_frameIndex].m_handle, RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
            };
            cmdResourceBarrier(
                cmd, 0, nullptr, 0, nullptr, rtBarriers.size(), rtBarriers.data());
        }
        {
            auto& opaqueSet = graphicsAllocator->resolveSet(GraphicsAllocator::AllocationSet::OpaqueSet);
            cmdBeginDebugMarker(cmd, 0, 1, 0, "Visibility Buffer Pass");
            {
                std::array semantics = { ShaderSemantic::SEMANTIC_POSITION };
                LoadActionsDesc loadActions = {};
                loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
                loadActions.mLoadActionsColor[1] = LOAD_ACTION_CLEAR;
                loadActions.mClearColorValues[0] = { .r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f };
                loadActions.mClearDepth = { .depth = 1.0f, .stencil = 0 };
                loadActions.mLoadActionDepth = LOAD_ACTION_CLEAR;

                std::array targets = {
                    viewportDatum->m_visiblityBuffer[frame.m_frameIndex].m_handle
                };
                cmdBindRenderTargets(cmd, targets.size(), targets.data(), viewportDatum->m_depthBuffer[frame.m_frameIndex].m_handle, &loadActions, NULL, NULL, -1, -1);
                cmdSetViewport(cmd, 0.0f, 0.0f, viewportDatum->m_size.x, viewportDatum->m_size.y, 0.0f, 1.0f);
                cmdSetScissor(cmd, 0, 0, viewportDatum->m_size.x, viewportDatum->m_size.y);

                cmdBindPipeline(cmd, m_visibilityBufferPass.m_handle);
                opaqueSet.cmdBindGeometrySet(cmd, semantics);
                cmdBindIndexBuffer(cmd, opaqueSet.indexBuffer().m_handle, INDEX_TYPE_UINT32, 0);
                cmdBindDescriptorSet(cmd, 0, m_sceneDescriptorConstSet.m_handle);
                cmdBindDescriptorSet(cmd, 0, m_sceneDescriptorPerFrameSet[frame.m_frameIndex].m_handle);
		        cmdExecuteIndirect(cmd, m_cmdSignatureVBPass,  opaqueIndirectArgs.size(), m_indirectDrawArgsBuffer[frame.m_frameIndex].m_handle, opaqueIndirectArgs.m_start * sizeof(IndirectDrawIndexArguments) , nullptr, 0);
            }
            {
                std::array semantics = {
                    ShaderSemantic::SEMANTIC_POSITION,
                    ShaderSemantic::SEMANTIC_TEXCOORD0,
                    ShaderSemantic::SEMANTIC_NORMAL,
                    ShaderSemantic::SEMANTIC_TANGENT
                };
                LoadActionsDesc loadActions = {};
                loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
                loadActions.mLoadActionsColor[1] = LOAD_ACTION_CLEAR;
                loadActions.mClearColorValues[0] = { .r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f };
                loadActions.mClearColorValues[1] = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f };
                loadActions.mClearDepth = { .depth = 1.0f, .stencil = 0 };
                loadActions.mLoadActionDepth = LOAD_ACTION_LOAD;

                std::array targets = {
                    viewportDatum->m_visiblityBuffer[frame.m_frameIndex].m_handle,
                    viewportDatum->m_testBuffer[frame.m_frameIndex].m_handle
                };
                cmdBindRenderTargets(cmd, targets.size(), targets.data(), viewportDatum->m_depthBuffer[frame.m_frameIndex].m_handle, &loadActions, NULL, NULL, -1, -1);
                cmdSetViewport(cmd, 0.0f, 0.0f, viewportDatum->m_size.x, viewportDatum->m_size.y, 0.0f, 1.0f);
                cmdSetScissor(cmd, 0, 0, viewportDatum->m_size.x, viewportDatum->m_size.y);

                cmdBindPipeline(cmd, m_visbilityAlphaBufferPass.m_handle);
                opaqueSet.cmdBindGeometrySet(cmd, semantics);
                cmdBindIndexBuffer(cmd, opaqueSet.indexBuffer().m_handle, INDEX_TYPE_UINT32, 0);
                cmdBindDescriptorSet(cmd, 0, m_sceneDescriptorConstSet.m_handle);
                cmdBindDescriptorSet(cmd, 0, m_sceneDescriptorPerFrameSet[frame.m_frameIndex].m_handle);
		        cmdExecuteIndirect(cmd, m_cmdSignatureVBPass,  translucentIndirectArgs.size(), m_indirectDrawArgsBuffer[frame.m_frameIndex].m_handle, translucentIndirectArgs.m_start * sizeof(IndirectDrawIndexArguments) , nullptr, 0);
            }
            cmdEndDebugMarker(cmd);
        }
        {

            cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
            std::array rtBarriers = {
                RenderTargetBarrier{ viewportDatum->m_visiblityBuffer[frame.m_frameIndex].m_handle, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_SHADER_RESOURCE },
                RenderTargetBarrier{ viewportDatum->m_outputBuffer[frame.m_frameIndex].m_handle, RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
            };
            cmdResourceBarrier(
                cmd, 0, nullptr, 0, nullptr, rtBarriers.size(), rtBarriers.data());
        }
        {
            cmdBeginDebugMarker(cmd, 0, 1, 0, "Visibility Output Buffer Pass");
            LoadActionsDesc loadActions = {};
            loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
            loadActions.mClearColorValues[0] = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f };
            loadActions.mClearDepth = { .depth = 1.0f, .stencil = 0 };
            loadActions.mLoadActionDepth = LOAD_ACTION_CLEAR;
            std::array targets = {
                viewportDatum->m_outputBuffer[frame.m_frameIndex].m_handle,
                viewportDatum->m_testBuffer[frame.m_frameIndex].m_handle
            };
            cmdBindRenderTargets(cmd, targets.size(), targets.data(), viewportDatum->m_depthBuffer[frame.m_frameIndex].m_handle, &loadActions, NULL, NULL, -1, -1);
            cmdSetViewport(cmd, 0.0f, 0.0f, viewportDatum->m_size.x, viewportDatum->m_size.y, 0.0f, 1.0f);
            cmdSetScissor(cmd, 0, 0, viewportDatum->m_size.x, viewportDatum->m_size.y);
            cmdBindPipeline(cmd, m_visiblityShadePass.m_handle);
            cmdBindDescriptorSet(cmd, 0, m_sceneDescriptorConstSet.m_handle);
            cmdBindDescriptorSet(cmd, 0, m_sceneDescriptorPerFrameSet[frame.m_frameIndex].m_handle);
		    cmdDraw(cmd, 3, 0);

            cmdEndDebugMarker(cmd);
        }

    }

} // namespace hpl