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

#include "graphics/RendererSimple.h"

#include "math/Frustum.h"
#include "math/Math.h"

#include "system/Hasher.h"

#include "graphics/Graphics.h"
#include "graphics/GraphicsTypes.h"
#include "graphics/GraphicUtils.h"
#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/Renderable.h"

#include "graphics/DebugDraw.h"
#include "graphics/Graphics.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "graphics/VertexBuffer.h"

#include "resources/Resources.h"

#include "graphics/Texture.h"

#include "scene/ParticleEmitter.h"
#include "scene/Viewport.h"
#include "scene/World.h"
#include "scene/RenderableSet.h"

#include <cstring>

namespace hpl {

	namespace {
		// Mirrors SimplePass in Simple.{vert,frag}.slang (binding 0, set 0).
		struct SimplePass {
			float mvp[16];
			float uvMtx[16];
			float color[4];
			float alphaReject;
			float pad[3];
		};

		// Pipeline variants, folded into the cache hash. OPAQUE is the diffuse
		// list (blend off, depth write on); the rest are the decal/translucent
		// hardware blend modes (depth read-only ≤), same eMaterialBlendMode
		// remap as the particle/decal passes in cHybridRenderer.
		enum SimpleBlendVariant : uint32_t {
			SIMPLE_OPAQUE = 0,
			SIMPLE_BLEND_ADD,
			SIMPLE_BLEND_MUL,
			SIMPLE_BLEND_MULX2,
			SIMPLE_BLEND_ALPHA,
			SIMPLE_BLEND_PREMUL_ALPHA,
		};

		SimpleBlendVariant remapBlend(eMaterialBlendMode aMode) {
			switch(aMode) {
			case eMaterialBlendMode_Add:         return SIMPLE_BLEND_ADD;
			case eMaterialBlendMode_Mul:         return SIMPLE_BLEND_MUL;
			case eMaterialBlendMode_MulX2:       return SIMPLE_BLEND_MULX2;
			case eMaterialBlendMode_PremulAlpha: return SIMPLE_BLEND_PREMUL_ALPHA;
			case eMaterialBlendMode_Alpha:
			default:                             return SIMPLE_BLEND_ALPHA;
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cRendererSimple::cRendererSimple(cGraphics *apGraphics,cResources* apResources)
		: iRenderer("Simple",apGraphics, apResources),
		  m_simple(std::make_shared<RIProgram>())
	{
		LoadData();
	}

	//-----------------------------------------------------------------------

	bool cRendererSimple::LoadData()
	{
		// Standalone program (no bindless set) — mirrors cRendererWireFrame;
		// per-draw state arrives via a frame-scratch UBO ("pass") plus a
		// per-draw diffuse texture + sampler (DebugDraw-style named bindings).
		auto vert_stage = RIProgram::loadShaderStage(mpResources->GetFileSearcher(), "Simple.vert");
		auto frag_stage = RIProgram::loadShaderStage(mpResources->GetFileSearcher(), "Simple.frag");
		std::array<RIProgram::ModuleStage, 2> stages = {
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
		};
		m_simple->initialize(&mpGraphics->device, stages, {}, "RendererSimple");
		return true;
	}

	//-----------------------------------------------------------------------

	void cRendererSimple::DestroyData()
	{
		// Keep the old program alive until every command buffer that may have
		// bound one of its pipelines has retired. This also covers the public
		// ReloadRendererData path, which can run while a frame is active.
		auto oldProgram = std::move(m_simple);
		m_simple = std::make_shared<RIProgram>();
		mpGraphics->graphicsDefer.push(std::function<void()>(
			[oldProgram = std::move(oldProgram), device = &mpGraphics->device]() mutable {
				if (oldProgram)
					oldProgram->dispose(device);
			}));
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------


	//-----------------------------------------------------------------------

	// cViewport::SimpleViewportState — the viewport holds the state, this
	// backend refines it (shared by cRendererWireFrame + cRendererSimple): a
	// 1:1 color render target the renderer draws into plus a matching depth
	// attachment. cScene feeds the finished render target (GetBackBuffer)
	// into the viewport pogo in the post-processing step. Replacement/resize
	// hand the old targets to the frame freelist (drained once the pipeline
	// is done with them) — no stall.
	void cViewport::SimpleViewportState::Update(cGraphics::FrameContext *cntx, cVector2l size)
	{
		cGraphics* pGraphics = Interface<cGraphics>::Get();
		if(size.x <= 0 || size.y <= 0)
		{
			return;
		}
		const uint32_t w = (uint32_t)size.x;
		const uint32_t h = (uint32_t)size.y;
		if(width == w && height == h)
		{
			return;
		}

		*this = {}; // defer the old resources, reset to empty (see operator=)

		width = w;
		height = h;
		for(uint32_t i = 0; i < pGraphics->swapchain->imageCount; i++)
		{
			// 1:1 color target: drawn by the renderer, sampled by cScene's
			// pogo feed (TRANSFER_SRC backs the feed blit).
			CreateViewportColorTexture(
				&pGraphics->device, w, h, cGraphics::PogoColorFormat,
				RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE |
					RI_USAGE_TRANSFER_SRC,
				&renderTarget[i], &renderTargetView[i],
				"SimpleViewportState.renderTarget");

			CreateViewportAttachmentTexture(
				&pGraphics->device, w, h, cGraphics::DepthFormat,
				RI_USAGE_DEPTH_STENCIL_ATTACHMENT,
				RI_VIEWTYPE_DEPTH_STENCIL_ATTACHMENT, &depthTextures[i], &depthView[i],
				"SimpleViewportState.depth");
		}
	}

	cViewport::SimpleViewportState::~SimpleViewportState()
	{
		cGraphics* pGraphics = Interface<cGraphics>::Get();
		for(uint32_t i = 0; i < RI_MAX_SWAPCHAIN_IMAGES; i++)
		{
    	pGraphics->graphicsDefer.push(renderTarget[i]);
    	pGraphics->graphicsDefer.push(renderTargetView[i]);
    	
    	pGraphics->graphicsDefer.push(depthTextures[i]);
    	pGraphics->graphicsDefer.push(depthView[i]);
		}
	}

	// See HybridViewportState::operator=: defer our resources (the dtor pushes each
	// shared handle to the freelist), then move-construct rhs's shared handles into
	// us (pointer steal — no dispose). Drives `*this = {}` on resize.
	cViewport::SimpleViewportState &
	cViewport::SimpleViewportState::operator=(SimpleViewportState &&rhs) noexcept
	{
		if(this != &rhs)
		{
			this->~SimpleViewportState();
			new (this) SimpleViewportState(std::move(rhs));
		}
		return *this;
	}

	//-----------------------------------------------------------------------

	void cRendererSimple::Draw(cGraphics::FrameContext* cntx,
							   cViewport* viewport,
							   float afFrameTime,
							   cFrustum* apFrustum,
							   cWorld* apWorld,
							   cRenderSettings* apSettings,
							   bool abSendFrameBufferToPostEffects)
	{
		////////////////////////////////////////////
		// Build the render list (same walk as cHybridRenderer::Draw, minus the
		// BLAS parking — nothing here is ray traced).
		m_rendererList.BeginAndReset(afFrameTime, apFrustum);
		auto *dynamicContainer = apWorld->GetRenderableSet(eWorldContainerType_Dynamic);
		auto *staticContainer = apWorld->GetRenderableSet(eWorldContainerType_Static);
		dynamicContainer->UpdateBeforeRendering();
		staticContainer->UpdateBeforeRendering();

		auto prepareObjectHandler = [&](iRenderable *pObject) {
			if(!rendering::IsObjectIsVisible(pObject, eRenderableFlag_VisibleInNonReflection, {})) {
				return;
			}
			m_rendererList.AddObject(pObject);
		};
		rendering::WalkAndPrepareRenderList(dynamicContainer, apFrustum, prepareObjectHandler,
											eRenderableFlag_VisibleInNonReflection);
		rendering::WalkAndPrepareRenderList(staticContainer, apFrustum, prepareObjectHandler,
											eRenderableFlag_VisibleInNonReflection);
		m_rendererList.End(eRenderListCompileFlag_Diffuse |
						   eRenderListCompileFlag_Decal |
						   eRenderListCompileFlag_Translucent);

		const eRenderListType lists[] = {
			eRenderListType_Diffuse,
			eRenderListType_Decal,
			eRenderListType_Translucent
		};

		////////////////////////////////////////////
		// Upload vertex/index streams. Must run BEFORE beginRendering — the
		// uploader records barriers that can't live inside a dynamic-rendering
		// scope. No BuildBlas: simple never traces.
		for(eRenderListType listType : lists)
		{
			for(iRenderable *pObject : m_rendererList.GetRenderableItems(listType))
			{
				if(pObject == NULL) continue;

				if(listType == eRenderListType_Translucent)
				{
					// Particle emitters take the per-viewport scratch path in
					// the draw loop (BuildViewportVertices straight into
					// Interface<cGraphics>::Get()->translucentVtx/Idx segments) — panes must NOT write
					// the persistent VB: its uploader copies coalesce in the
					// fenced pre-pass, so the last pane would win for EVERY
					// pane (and poison the hybrid view).
					if(pObject->GetRenderType() == eRenderableType_ParticleEmitter)
						continue;

					// Dynamic translucent geometry (billboards/beams) still
					// rebuilds its vertex data per viewport before upload.
					// TODO: same shared-buffer disease as particles — migrate
					// to the scratch path.
					pObject->UpdateGraphicsForFrame(afFrameTime);
					pObject->UpdateGraphicsForViewport(apFrustum, afFrameTime);
				}

				cVertexBuffer *pVB = pObject->GetVertexBuffer();
				if(pVB == NULL) continue;

				auto *vbri = static_cast<cVertexBuffer*>(pVB);
				vbri->SubmitToGPU(&mpGraphics->device);
			}
		}

		const cVector2l displayExtent = viewport->GetDisplayExtent();
		if(displayExtent.x <= 0 || displayExtent.y <= 0) {
			return;
		}

		////////////////////////////////////////////
		// Viewport-owned targets: PrepareToRender configures this backend's
		// state (1:1 color render target + depth) for the negotiated render
		// extent. cScene feeds the finished render target into the viewport pogo
		// afterwards.
		cViewport::SimpleViewportState *pState =
			viewport->PrepareToRender<cViewport::SimpleViewportState>(cntx);
		if(pState == nullptr || pState->width == 0 || pState->height == 0) {
			return;
		}
		cViewport::SimpleViewportState &state = *pState;
		const uint32_t renderWidth = state.width;
		const uint32_t renderHeight = state.height;

		////////////////////////////////////////////
		// Pre-render transitions: render target UNDEFINED -> COLOR (contents
		// discarded, we clear) and depth UNDEFINED -> DEPTH_ATTACHMENT
		// (cleared via loadOp).
		{
			RITextureBarrier barriers[2] = {};
			barriers[0] = RI_PogoAttachmentBarrier(
				state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/true);

			barriers[1].texture = state.depthTextures[mpGraphics->swapchainIndex].Get();
			barriers[1].before = RI_RESOURCE_STATE_UNDEFINED;
			barriers[1].after = RI_RESOURCE_STATE_DEPTH_WRITE;
			barriers[1].aspect = RI_BARRIER_ASPECT_DEPTH;
			barriers[1].mipCount = 1;
			barriers[1].layerCount = 1;

			mpGraphics->primary.cmds[0].vk_d3d12_textureBarriers<2>(2, barriers);
		}

		////////////////////////////////////////////
		// Begin rendering into the state's render target at its 1:1 extent —
		// no overscan; cScene's pogo feed consumes it afterwards.
		{
			RITextureView colorView = *state.renderTargetView[mpGraphics->swapchainIndex];

			RIRenderingAttachment color = {};
			color.view = colorView;
			color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
			color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
			// Honor the viewport's clear color (e.g. the thumbnail builder's
			// gray backdrop) like the other renderers do.
			color.clearValue.color[0] = apSettings->mClearColor.r;
			color.clearValue.color[1] = apSettings->mClearColor.g;
			color.clearValue.color[2] = apSettings->mClearColor.b;
			color.clearValue.color[3] = apSettings->mClearColor.a;

			RIRenderingAttachment depth = {};
			depth.view = *state.depthView[mpGraphics->swapchainIndex];
			depth.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
			depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
			depth.readOnly = false;
			depth.hasStencil = false;
			depth.clearValue.depth = 1.0f;

			RIBeginRenderingDesc beginDesc = {};
			beginDesc.renderArea.x = 0;
			beginDesc.renderArea.y = 0;
			beginDesc.renderArea.width = renderWidth;
			beginDesc.renderArea.height = renderHeight;
			beginDesc.colorCount = 1;
			beginDesc.colors = &color;
			beginDesc.depthStencil = &depth;
			mpGraphics->primary.cmds[0].vk_d3d12_beginRendering(&mpGraphics->device, beginDesc);
		}

		// Y-flipped viewport — same convention as the forward passes, so the
		// unmodified projection matrix lands the right way up (negative height).
		RIViewport viewportRi = {};
		viewportRi.x = 0.0f;
		viewportRi.y = (float)renderHeight;
		viewportRi.width = (float)renderWidth;
		viewportRi.height = -(float)renderHeight;
		viewportRi.depthMin = 0.0f;
		viewportRi.depthMax = 1.0f;
		RIRect scissor = {};
		scissor.x = 0;
		scissor.y = 0;
		scissor.width = renderWidth;
		scissor.height = renderHeight;
		mpGraphics->primary.cmds[0].setViewport(&mpGraphics->device, viewportRi);
		mpGraphics->primary.cmds[0].setScissor(&mpGraphics->device, scissor);

		////////////////////////////////////////////
		// Pipeline builder: 3 fixed-function streams (position always present;
		// color / uv fall back to the single-vertex defaults with a zeroed
		// binding stride, same trick as TranslucentMeshPipelineDesc). Variant
		// (blend/depth-write) + stream-present mask fold into the cache hash so
		// RIProgram keeps one pipeline per combination.
		auto bindSimplePipeline = [&](SimpleBlendVariant aVariant, uint32_t alVtxMask)
		{
			RIGraphicsPipelineDesc desc = {};

			desc.vertexInput.bindingCount = 3;
			desc.vertexInput.bindings[0] = { 0, 16, RI_VERTEX_INPUT_RATE_VERTEX }; // position (engine stream is float4, stride 16)
			desc.vertexInput.bindings[1] = { 1, (alVtxMask & eVertexElementFlag_Color0)   ? 16u : 0u, RI_VERTEX_INPUT_RATE_VERTEX }; // color
			desc.vertexInput.bindings[2] = { 2, (alVtxMask & eVertexElementFlag_Texture0) ? 12u : 0u, RI_VERTEX_INPUT_RATE_VERTEX }; // uv (float3 stream, .xy consumed)
			desc.vertexInput.attributeCount = 3;
			desc.vertexInput.attributes[0] = { 0, 0, RI_FORMAT_RGB32_SFLOAT,  0 }; // position
			desc.vertexInput.attributes[1] = { 1, 1, RI_FORMAT_RGBA32_SFLOAT, 0 }; // color
			desc.vertexInput.attributes[2] = { 2, 2, RI_FORMAT_RG32_SFLOAT,   0 }; // uv

			desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;

			// CLOCKWISE front face to match the opaque/translucent raster
			// passes — all run under the same Y-flipped viewport.
			desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
			desc.raster.cullMode = RI_CULL_MODE_BACK;
			desc.raster.frontFace = RI_FRONT_FACE_CLOCKWISE;
			desc.raster.lineWidth = 1.0f;

			desc.renderTarget.colorCount = 1;
			desc.renderTarget.colorFormats[0] = cGraphics::PogoColorFormat;
			desc.renderTarget.depthFormat = cGraphics::DepthFormat;

			// Opaque writes depth; decals/translucents test against it only.
			desc.depthStencil.depthTest = true;
			desc.depthStencil.depthWrite = (aVariant == SIMPLE_OPAQUE);
			desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;

			desc.blendCount = 1;
			desc.blend[0].writeMask = RI_COLOR_WRITE_RGBA;
			desc.blend[0].colorOp = RI_BLEND_OP_ADD;
			desc.blend[0].alphaOp = RI_BLEND_OP_ADD;
			switch(aVariant)
			{
			case SIMPLE_OPAQUE:
				desc.blend[0].blendEnable = false;
				desc.blend[0].srcColor = RI_BLEND_ONE;
				desc.blend[0].dstColor = RI_BLEND_ZERO;
				desc.blend[0].srcAlpha = RI_BLEND_ONE;
				desc.blend[0].dstAlpha = RI_BLEND_ZERO;
				break;
			case SIMPLE_BLEND_ADD:
				desc.blend[0].blendEnable = true;
				desc.blend[0].srcColor = RI_BLEND_ONE;
				desc.blend[0].dstColor = RI_BLEND_ONE;
				desc.blend[0].srcAlpha = RI_BLEND_ONE;
				desc.blend[0].dstAlpha = RI_BLEND_ONE;
				break;
			case SIMPLE_BLEND_MUL:
				desc.blend[0].blendEnable = true;
				desc.blend[0].srcColor = RI_BLEND_ZERO;
				desc.blend[0].dstColor = RI_BLEND_SRC_COLOR;
				desc.blend[0].srcAlpha = RI_BLEND_ZERO;
				desc.blend[0].dstAlpha = RI_BLEND_SRC_ALPHA;
				break;
			case SIMPLE_BLEND_MULX2:
				desc.blend[0].blendEnable = true;
				desc.blend[0].srcColor = RI_BLEND_DST_COLOR;
				desc.blend[0].dstColor = RI_BLEND_SRC_COLOR;
				desc.blend[0].srcAlpha = RI_BLEND_DST_ALPHA;
				desc.blend[0].dstAlpha = RI_BLEND_SRC_ALPHA;
				break;
			case SIMPLE_BLEND_ALPHA:
				desc.blend[0].blendEnable = true;
				desc.blend[0].srcColor = RI_BLEND_SRC_ALPHA;
				desc.blend[0].dstColor = RI_BLEND_ONE_MINUS_SRC_ALPHA;
				desc.blend[0].srcAlpha = RI_BLEND_SRC_ALPHA;
				desc.blend[0].dstAlpha = RI_BLEND_ONE_MINUS_SRC_ALPHA;
				break;
			case SIMPLE_BLEND_PREMUL_ALPHA:
				desc.blend[0].blendEnable = true;
				desc.blend[0].srcColor = RI_BLEND_ONE;
				desc.blend[0].dstColor = RI_BLEND_ONE_MINUS_SRC_ALPHA;
				desc.blend[0].srcAlpha = RI_BLEND_ONE;
				desc.blend[0].dstAlpha = RI_BLEND_ONE_MINUS_SRC_ALPHA;
				break;
			}

			// Blend variant and stream-present mask are both structural now
			// (blend factors / binding strides), so no variant salt is needed.
			m_simple->bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0], HASH_INITIAL_VALUE, "simple", desc);
		};

		////////////////////////////////////////////
		// Draw the lists in legacy order: diffuse (depth write, no blend) →
		// decals → translucents (depth read-only, hardware blend). Per-object
		// state flows through a frame-scratch UBO (Interface<cGraphics>::Get()->UpdateFrameUBO) plus a
		// per-draw diffuse texture + sampler, DebugDraw-style.
		const ml::float4x4 viewProjMtx = apFrustum->GetProjectionMat() * apFrustum->GetViewMat();
		for(eRenderListType listType : lists)
		{
			for(iRenderable *pObject : m_rendererList.GetRenderableItems(listType))
			{
				if(pObject == NULL) continue;

				// Draw sources — persistent VB by default; particles use the
				// per-viewport scratch path below.
				RIBuffer *posBuffer = nullptr;
				RIBuffer *colBuffer = nullptr;
				RIBuffer *uvBuffer = nullptr;
				RIBuffer *idxBuffer = nullptr;
				VkDeviceSize posOffset = 0, colOffset = 0, uvOffset = 0, idxOffset = 0;
				int indexCount = 0;

				if(listType == eRenderListType_Translucent &&
				   pObject->GetRenderType() == eRenderableType_ParticleEmitter)
				{
					////////////////////////////////////////////
					// Particles: build this pane's camera-facing quads straight
					// into per-frame segments of Interface<cGraphics>::Get()->translucentVtx/IdxBuffer —
					// each viewport gets its own correctly-billboarded geometry
					// (the persistent VB stays the hybrid renderer's; its
					// uploader copies coalesce in the fenced pre-pass, so panes
					// writing it would let the last pane win for EVERY pane).
					auto *pEmitter = static_cast<iParticleEmitter*>(pObject);
					auto geom = pEmitter->BuildScratchGeometry(apFrustum, afFrameTime, /*withUv=*/true);
					if(!geom.valid) continue;

					posBuffer = mpGraphics->translucentVtxBuffer.Get();
					colBuffer = mpGraphics->translucentVtxBuffer.Get();
					uvBuffer  = mpGraphics->translucentVtxBuffer.Get();
					idxBuffer = mpGraphics->translucentIdxBuffer.Get();
					posOffset = (VkDeviceSize)geom.posByteOffset;
					colOffset = (VkDeviceSize)geom.colByteOffset;
					uvOffset  = (VkDeviceSize)geom.uvByteOffset;
					idxOffset = (VkDeviceSize)geom.idxByteOffset;
					indexCount = (int)geom.indexCount;
				}
				else
				{
					cVertexBuffer *pVB = pObject->GetVertexBuffer();
					if(pVB == NULL) continue;

					auto *vbri = static_cast<cVertexBuffer*>(pVB);
					const auto *posElement = vbri->GetElement(eVertexBufferElement_Position);
					const auto *colElement = vbri->GetElement(eVertexBufferElement_Color0);
					const auto *uvElement = vbri->GetElement(eVertexBufferElement_Texture0);
					const auto &indexBufferPtr = vbri->GetIndexRIBuffer();
					indexCount = vbri->GetIndexNum();
					if(posElement == NULL || !posElement->GetBuffer() || !indexBufferPtr || indexCount <= 0) {
						continue;
					}
					posBuffer = posElement->GetBuffer();
					idxBuffer = indexBufferPtr;
					colBuffer = colElement ? colElement->GetBuffer() : nullptr;
					uvBuffer = uvElement ? uvElement->GetBuffer() : nullptr;
				}

				cMaterial *pMat = pObject->GetMaterial();

				const SimpleBlendVariant variant = (listType == eRenderListType_Diffuse)
					? SIMPLE_OPAQUE
					: remapBlend(pMat ? pMat->GetBlendMode() : eMaterialBlendMode_Alpha);
				uint32_t vtxMask = 0;
				if(colBuffer) vtxMask |= eVertexElementFlag_Color0;
				if(uvBuffer) vtxMask |= eVertexElementFlag_Texture0;
				bindSimplePipeline(variant, vtxMask);

				cMatrixf *pModelMtx = pObject->GetModelMatrix(apFrustum);
				const ml::float4x4 mvp = viewProjMtx *
					cMath::ToFloatTranspose4x4(pModelMtx ? *pModelMtx : cMatrixf::Identity);
				const ml::float4x4 uvMtx =
					cMath::ToFloatTranspose4x4(pMat ? pMat->GetUvMatrix() : cMatrixf::Identity);

				SimplePass uniformBlock = {};
				std::memcpy(uniformBlock.mvp, mvp.a, sizeof(uniformBlock.mvp));
				std::memcpy(uniformBlock.uvMtx, uvMtx.a, sizeof(uniformBlock.uvMtx));
				uniformBlock.color[0] = 1.0f;
				uniformBlock.color[1] = 1.0f;
				uniformBlock.color[2] = 1.0f;
				uniformBlock.color[3] = 1.0f;
				// Cutout solids (alpha-tested grates/foliage) reject in the
				// fragment stage; blended lists keep their hardware blend.
				uniformBlock.alphaReject =
					(listType == eRenderListType_Diffuse && pMat &&
					 pMat->GetAlphaMode() == eMaterialAlphaMode_Trans) ? 0.5f : 0.0f;

				// Per-draw diffuse: material texture when present and uploaded,
				// else the 1x1 white default. Pin real textures so a mid-frame
				// destroy can't free the VkImage before this submit retires.
				Image *pDiffuseImage = pMat ? pMat->GetImage(eMaterialTexture_Diffuse) : nullptr;
				cTexture *texture = pDiffuseImage ? pDiffuseImage->GetTexture() : nullptr;
				RIDescriptor textureDescriptor = mpGraphics->whiteTexture2DDescriptor();
				if(texture && !texture->view.isEmpty()) {
					mpGraphics->graphicsDefer.push(PinResource(pDiffuseImage));
					textureDescriptor = texture->descriptor();
				}

				auto samplerDesc = mpGraphics->resolve_filter_descriptor(
					pMat ? pMat->GetTextureWrap() : eTextureWrap_Repeat,
					pMat ? pMat->GetTextureWrap() : eTextureWrap_Repeat,
					pMat ? pMat->GetTextureWrap() : eTextureWrap_Repeat,
					pMat ? pMat->GetTextureFilter() : eTextureFilter_Bilinear);
				assert(samplerDesc);

				RIProgram::DescriptorBinding bindings[3] = {};
				mpGraphics->UpdateFrameUBO(&bindings[0].descriptor, (void*)&uniformBlock, sizeof(uniformBlock));
				bindings[0].handle = DescriptorBindingID::Create("pass");
				bindings[1].descriptor = *samplerDesc;
				bindings[1].handle = DescriptorBindingID::Create("diffuseSampler");
				bindings[2].descriptor = textureDescriptor;
				bindings[2].handle = DescriptorBindingID::Create("diffuseMap");
				m_simple->bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex, bindings, 3);

				RIBuffer *vertBufs[3] = {
					posBuffer,
					colBuffer ? colBuffer : &mpGraphics->fallbackColorVertex,
					uvBuffer ? uvBuffer : &mpGraphics->fallbackUv0Vertex,
				};
				const VkDeviceSize vertOffsets[3] = { posOffset, colOffset, uvOffset };
				mpGraphics->primary.cmds[0].bindVertexBuffers<3>(0, 3, vertBufs, vertOffsets);
				mpGraphics->primary.cmds[0].bindIndexBuffer(&mpGraphics->device, idxBuffer, idxOffset, RI_INDEX_TYPE_32);
				mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, (uint32_t)indexCount, 1, 0, 0, 0);
			}
		}

		////////////////////////////////////////////
		// Offscreen panes: overlay the queued DebugDraw requests (grid, axes,
		// gizmos — enqueued by the editor's OnPreWorldDraw callbacks) into the
		// same rendering scope before it closes. Same color/depth formats, so
		// the batcher's pipelines match.
		DebugDraw *debugDraw = mpGraphics->GetDebugDraw();
		if(debugDraw && debugDraw->HasRequests())
		{
			debugDraw->flush(cntx, &mpGraphics->primary.cmds[0], apFrustum, renderWidth,
							 renderHeight, cGraphics::PogoColorFormat);
		}

		mpGraphics->primary.cmds[0].vk_d3d12_endRendering(&mpGraphics->device);

		////////////////////////////////////////////
		// Hand off: render target COLOR -> SHADER_READ — the finished frame
		// cScene feeds into the viewport pogo (post processing) and delivers
		// to the Target.
		{
			mpGraphics->primary.cmds[0].vk_d3d12_textureBarrier(RI_PogoShaderBarrier(
				state.renderTarget[mpGraphics->swapchainIndex].Get(), /*initial=*/false));
		}
	}


	//-----------------------------------------------------------------------

}
