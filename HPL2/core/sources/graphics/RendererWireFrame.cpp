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

#include "graphics/RendererWireFrame.h"

#include "math/Frustum.h"
#include "math/Math.h"

#include "system/Hasher.h"

#include "graphics/Graphics.h"
#include "graphics/GraphicUtils.h"
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
		// Mirrors WireFramePass in wireframe.{vert,frag}.slang (binding 0, set 0).
		struct WireFramePass {
			float mvp[16];
			float color[4];
		};
	}

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cRendererWireFrame::cRendererWireFrame(cGraphics *apGraphics,cResources* apResources)
		: iRenderer("WireFrame",apGraphics, apResources),
		  m_wireframe(std::make_shared<RIProgram>())
	{
		LoadData();
	}

	//-----------------------------------------------------------------------

	bool cRendererWireFrame::LoadData()
	{
		// Standalone program (no bindless set) — mirrors the Interface<cGraphics>::Get()->gui load in
		// cGraphics::Init; per-draw state arrives via a frame-scratch UBO
		// ("pass", see Draw below).
		auto vert_stage = RIProgram::loadShaderStage(mpResources->GetFileSearcher(), "wireframe.vert");
		auto frag_stage = RIProgram::loadShaderStage(mpResources->GetFileSearcher(), "wireframe.frag");
		std::array<RIProgram::ModuleStage, 2> stages = {
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vert_stage, "vsMain"},
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, frag_stage, "psMain"}
		};
		m_wireframe->initialize(&mpGraphics->device, stages, {}, "RendererWireFrame");
		return true;
	}

	//-----------------------------------------------------------------------

	void cRendererWireFrame::DestroyData()
	{
		// Keep the old program alive until every command buffer that may have
		// bound one of its pipelines has retired. This also covers the public
		// ReloadRendererData path, which can run while a frame is active.
		auto oldProgram = std::move(m_wireframe);
		m_wireframe = std::make_shared<RIProgram>();
		mpGraphics->graphicsDefer.push(std::function<void()>(
			[oldProgram = std::move(oldProgram), device = &mpGraphics->device]() mutable {
				if (oldProgram)
					oldProgram->dispose(device);
			}));
	}

	//-----------------------------------------------------------------------

	// cViewport::SimpleViewportState (shared with this renderer) is
	// implemented in RendererSimple.cpp.

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cRendererWireFrame::Draw(cGraphics::FrameContext* cntx,
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
		// Upload vertex/index streams. Must run BEFORE vkCmdBeginRendering — the
		// uploader records barriers that can't live inside a dynamic-rendering
		// scope. No BuildBlas: wireframe never traces.
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

			RIRenderingAttachment color = {};
			color.view =
				*state.renderTargetAttachmentView[mpGraphics->swapchainIndex];
			color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
			color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
			color.clearValue.color[0] = 0.0f;
			color.clearValue.color[1] = 0.0f;
			color.clearValue.color[2] = 0.0f;
			color.clearValue.color[3] = 1.0f;

			RIRenderingAttachment depth = {};
			depth.view = *state.depthView[mpGraphics->swapchainIndex];
			depth.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
			depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
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
		// unmodified projection matrix lands the right way up.
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
		// Pipeline: position-only fetch, triangle list rasterised as lines.
		// Formats are constexpr so a single cached variant suffices.
		{
			RIGraphicsPipelineDesc desc = {};

			desc.vertexInput.bindingCount = 1;
			desc.vertexInput.bindings[0] = { 0, 16, RI_VERTEX_INPUT_RATE_VERTEX };
			desc.vertexInput.attributeCount = 1;
			desc.vertexInput.attributes[0] = { 0, 0, RI_FORMAT_RGB32_SFLOAT, 0 }; // position (engine stream is float4, stride 16)

			desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;

			desc.raster.polygonMode = RI_POLYGON_MODE_LINE;
			desc.raster.cullMode = RI_CULL_MODE_NONE;
			// The Vk create-info never assigned frontFace, i.e. it was
			// COUNTER_CLOCKWISE; the RI default is CLOCKWISE, so spell it out.
			desc.raster.frontFace = RI_FRONT_FACE_COUNTER_CLOCKWISE;
			desc.raster.lineWidth = 1.0f;

			desc.renderTarget.colorCount = 1;
			desc.renderTarget.colorFormats[0] = cGraphics::PogoColorFormat;
			desc.renderTarget.depthFormat = cGraphics::DepthFormat;

			desc.depthStencil.depthTest = true;
			desc.depthStencil.depthWrite = true;
			desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;

			desc.blendCount = 1;
			desc.blend[0].blendEnable = false;
			desc.blend[0].srcColor = RI_BLEND_ONE;
			desc.blend[0].dstColor = RI_BLEND_ZERO;
			desc.blend[0].colorOp = RI_BLEND_OP_ADD;
			desc.blend[0].srcAlpha = RI_BLEND_ONE;
			desc.blend[0].dstAlpha = RI_BLEND_ZERO;
			desc.blend[0].alphaOp = RI_BLEND_OP_ADD;
			desc.blend[0].writeMask = RI_COLOR_WRITE_RGBA;

			m_wireframe->bindPipeline(&mpGraphics->device, &mpGraphics->primary.cmds[0], HASH_INITIAL_VALUE, "wireframe", desc);
		}

		////////////////////////////////////////////
		// Draw every list with the same flat pipeline. Per-object MVP flows
		// through a frame-scratch UBO slice (Interface<cGraphics>::Get()->UpdateFrameUBO), GUI-style.
		const ml::float4x4 viewProjMtx = apFrustum->GetProjectionMat() * apFrustum->GetViewMat();
		for(eRenderListType listType : lists)
		{
			for(iRenderable *pObject : m_rendererList.GetRenderableItems(listType))
			{
				if(pObject == NULL) continue;

				////////////////////////////////////////////
				// Particles: per-viewport scratch path. Build this pane's
				// camera-facing quads straight into per-frame segments of
				// Interface<cGraphics>::Get()->translucentVtx/IdxBuffer and bind them at byte offsets —
				// each pane gets its own correctly-billboarded geometry (the
				// persistent VB stays the hybrid renderer's).
				if(listType == eRenderListType_Translucent &&
				   pObject->GetRenderType() == eRenderableType_ParticleEmitter)
				{
					// Build this pane's camera-facing quads into the shared
					// per-frame scratch (one copy per viewport, billboarded to
					// its own camera). withUv=false — this pipeline reads only
					// position.
					auto *pEmitter = static_cast<iParticleEmitter*>(pObject);
					auto geom = pEmitter->BuildScratchGeometry(apFrustum, afFrameTime, /*withUv=*/false);
					if(!geom.valid) continue;

					// Particle verts are in VIEW space; GetModelMatrix returns
					// the inverse view so MVP collapses to the projection.
					cMatrixf *pModelMtx = pObject->GetModelMatrix(apFrustum);
					const ml::float4x4 mvp = viewProjMtx *
						cMath::ToFloatTranspose4x4(pModelMtx ? *pModelMtx : cMatrixf::Identity);

					WireFramePass uniformBlock = {};
					std::memcpy(uniformBlock.mvp, mvp.a, sizeof(uniformBlock.mvp));
					uniformBlock.color[0] = 1.0f;
					uniformBlock.color[1] = 1.0f;
					uniformBlock.color[2] = 1.0f;
					uniformBlock.color[3] = 1.0f;

					RIProgram::DescriptorBinding binding = {};
					mpGraphics->UpdateFrameUBO(&binding.descriptor, (void*)&uniformBlock, sizeof(uniformBlock));
					binding.handle = DescriptorBindingID::Create("pass");
					m_wireframe->bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex, &binding, 1);

					RIBuffer *vertBufs[1] = { mpGraphics->translucentVtxBuffer.Get() };
					const VkDeviceSize vertOffsets[1] = { (VkDeviceSize)geom.posByteOffset };
					mpGraphics->primary.cmds[0].bindVertexBuffers<1>(0, 1, vertBufs, vertOffsets);
					mpGraphics->primary.cmds[0].bindIndexBuffer(&mpGraphics->device, mpGraphics->translucentIdxBuffer.Get(),
													   (VkDeviceSize)geom.idxByteOffset,
													   RI_INDEX_TYPE_32);
					mpGraphics->primary.cmds[0].drawIndexed(&mpGraphics->device, geom.indexCount, 1, 0, 0, 0);
					continue;
				}

				cVertexBuffer *pVB = pObject->GetVertexBuffer();
				if(pVB == NULL) continue;

				auto *vbri = static_cast<cVertexBuffer*>(pVB);
				const auto *posElement = vbri->GetElement(eVertexBufferElement_Position);
				const auto &indexBuffer = vbri->GetIndexRIBuffer();
				const int indexCount = vbri->GetIndexNum();
				if(posElement == NULL || !posElement->GetBuffer() || !indexBuffer || indexCount <= 0) {
					continue;
				}

				cMatrixf *pModelMtx = pObject->GetModelMatrix(apFrustum);
				const ml::float4x4 mvp = viewProjMtx *
					cMath::ToFloatTranspose4x4(pModelMtx ? *pModelMtx : cMatrixf::Identity);

				WireFramePass uniformBlock = {};
				std::memcpy(uniformBlock.mvp, mvp.a, sizeof(uniformBlock.mvp));
				uniformBlock.color[0] = 1.0f;
				uniformBlock.color[1] = 1.0f;
				uniformBlock.color[2] = 1.0f;
				uniformBlock.color[3] = 1.0f;

				RIProgram::DescriptorBinding binding = {};
				mpGraphics->UpdateFrameUBO(&binding.descriptor, (void*)&uniformBlock, sizeof(uniformBlock));
				binding.handle = DescriptorBindingID::Create("pass");
				m_wireframe->bindDescriptors(&mpGraphics->device, &mpGraphics->primary.cmds[0], mpGraphics->frameIndex, &binding, 1);

				RIBuffer *vertBufs[1] = { posElement->GetBuffer() };
				const VkDeviceSize vertOffsets[1] = { 0 };
				mpGraphics->primary.cmds[0].bindVertexBuffers<1>(0, 1, vertBufs, vertOffsets);
				mpGraphics->primary.cmds[0].bindIndexBuffer(&mpGraphics->device, indexBuffer, 0, RI_INDEX_TYPE_32);
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
