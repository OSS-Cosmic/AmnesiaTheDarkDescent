/*
 * Copyright © 2009-2020 Frictional Games
 * Copyright 2026 Michael Pollind
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

#include "LuxEffectRenderer.h"

#include "graphics/Color.h"
#include "graphics/Texture.h"
#include "graphics/Image.h"
#include "graphics/Material.h"
#include "graphics/Graphics.h"
#include "graphics/RIBarrier.h"
#include "graphics/RIFormat.h"
#include "graphics/RIPogoBuffer.h"
#include "graphics/RIProgramHelpers.h"
#include "graphics/VertexBuffer.h"
#include "scene/Viewport.h"
#include "system/Hasher.h"

#include <algorithm>
#include <cstring>

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// LOCAL TYPES / HELPERS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

namespace {

// Per-frame UBO ("pass", set 0 binding 0) — view/viewProj for the geometry
// passes. Matches OutlinePass in outline_geom.vert.slang.
struct OutlinePassUBO {
	float viewProj[16];
	float view[16];
};

// Push constant — per-object model matrix + glow color. Matches OutlinePC
// (outline_geom).
struct OutlinePushConstants {
	float model[16];
	float color[4];
};

// Push constant for the alpha-tested outline variant — adds params.x (alpha
// texture single-channel flag). Matches OutlineAlphaPC (outline_alpha).
struct OutlineAlphaPushConstants {
	float model[16];
	float color[4];
	float params[4];
};

struct BlurPushConstants {
	float blurDir[2];
	float _pad[2];
};

// Outline glow colour is authored in DISPLAY (sRGB) space and converted with
// hpl::sRGBToLinear before the composite (the pogo target is linear HDR).
const float kOutlineGlow[3] = {0.0f, 0.0f, 0.5f};
const float kOutlineScaleAdd = 0.02f;

// Flash / enemy glow TINTS — applied as LINEAR multipliers into the HDR scene,
// matching the original dds_flash / dds_enemy_glow fragment maths (which tint
// in-shader with no sRGB decode). The glow is modulated by the object's own
// diffuse texture and a view rim, so these are gentle bluish tints, not a flat
// fill. Enemy folds the original's extra 0.7 scalar into the tint.
const float kFlashGlow[3] = {0.5f, 0.5f, 1.0f};
const float kEnemyGlow[3] = {0.6f * 0.7f, 0.6f * 0.7f, 1.0f * 0.7f};

enum eGeomPassMode {
	eGeomPassMode_OutlineMark = 0, // stencil ALWAYS -> REPLACE 1, no color
	eGeomPassMode_OutlineRim,      // stencil NOT_EQUAL 1, write glow color
	eGeomPassMode_Glow,            // flash / enemy: diffuse-modulated interior
	                               // fill, additive RGB, cull BACK, pos/uv
};

void EmitImageBarrier(RICmd *apCmd, RITexture *apTex,
					  RIResourceState_e aBefore, uint32_t alBeforeStages,
					  RIResourceState_e aAfter, uint32_t alAfterStages) {
	RITextureBarrier barrier = {};
	barrier.texture = apTex;
	barrier.before = aBefore;
	barrier.beforeStages = alBeforeStages;
	barrier.after = aAfter;
	barrier.afterStages = alAfterStages;
	apCmd->vk_d3d12_textureBarrier(barrier);
}

// Build (and cache on the program) a geometry pipeline for the given pass.
// abUvLayout switches binding 1 from normal (float3) to texcoord (float2) —
// used by the alpha-tested cutout outline AND the Glow pass (both sample a UV
// instead of a normal). abNormalPresent only applies to the normal layout.
void BindGeomPipeline(RIProgram &aProgram, RICmd *apCmd, eGeomPassMode aMode,
					  bool abNormalPresent, bool abUvLayout, const char *asDebugName) {
	const bool bGlow = (aMode == eGeomPassMode_Glow);

	RIGraphicsPipelineDesc desc = {};

	desc.vertexInput.bindingCount = 2;
	desc.vertexInput.bindings[0] = {0, 16, RI_VERTEX_INPUT_RATE_VERTEX}; // position (float4 stride)
	desc.vertexInput.bindings[1] = {1, abUvLayout ? 12u : (abNormalPresent ? 12u : 0u),
									RI_VERTEX_INPUT_RATE_VERTEX};        // texcoord (12) or normal (12)
	desc.vertexInput.attributeCount = 2;
	desc.vertexInput.attributes[0] = {0, 0, RI_FORMAT_RGB32_SFLOAT, 0};  // position
	desc.vertexInput.attributes[1] = {1, 1,
									  abUvLayout ? RI_FORMAT_RG32_SFLOAT : RI_FORMAT_RGB32_SFLOAT,
									  0};                                // uv / normal

	desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;

	desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
	// Cull BACK everywhere — the glow draws the visible FRONT faces so the mesh
	// interior fills with the additive glow (no silhouette-only border).
	desc.raster.cullMode = RI_CULL_MODE_BACK;
	desc.raster.frontFace = RI_FRONT_FACE_CLOCKWISE; // engine winding
	desc.raster.lineWidth = 1.0f;
	// Static negative depth bias, so the overlay geometry wins the LESS_EQUAL
	// test against the scene depth it was rasterised from. (The Vk create-info
	// set these fields in the middle of the depth-stencil block; they are
	// rasterizer state.)
	desc.raster.depthBiasEnable = true;
	desc.raster.depthBiasConstant = -1.0f;
	desc.raster.depthBiasSlope = -1.0f;
	desc.raster.depthBiasClamp = 0.0f;

	const bool bHasStencil = (aMode == eGeomPassMode_OutlineMark ||
							  aMode == eGeomPassMode_OutlineRim);

	desc.renderTarget.colorCount = 1;
	desc.renderTarget.colorFormats[0] = cGraphics::PogoColorFormat;
	desc.renderTarget.depthFormat = cGraphics::DepthFormat;
	desc.renderTarget.stencilFormat =
		bHasStencil ? cGraphics::DepthFormat : RI_FORMAT_UNKNOWN;

	// Read-only depth test against the scene depth (never writes).
	desc.depthStencil.depthTest = true;
	desc.depthStencil.depthWrite = false;
	desc.depthStencil.depthCompare = RI_COMPARE_LESS_EQUAL;
	if (bHasStencil) {
		desc.depthStencil.stencilTest = true;
		// The reference is shared by both faces in the RI desc (D3D12 has one
		// OMSetStencilRef); it is not part of RIStencilFaceDesc.
		desc.depthStencil.stencilReference = 1;
		RIStencilFaceDesc op = {};
		op.compareMask = 0xFF;
		if (aMode == eGeomPassMode_OutlineMark) {
			op.failOp = RI_STENCIL_OP_KEEP;
			op.passOp = RI_STENCIL_OP_REPLACE;
			op.depthFailOp = RI_STENCIL_OP_KEEP;
			op.compareFunc = RI_COMPARE_ALWAYS;
			op.writeMask = 0xFF;
		} else { // rim
			op.failOp = RI_STENCIL_OP_KEEP;
			op.passOp = RI_STENCIL_OP_KEEP;
			op.depthFailOp = RI_STENCIL_OP_KEEP;
			op.compareFunc = RI_COMPARE_NOT_EQUAL;
			op.writeMask = 0x00;
		}
		desc.depthStencil.front = op;
		desc.depthStencil.back = op;
	}

	desc.blendCount = 1; // must match renderTarget.colorCount
	if (bGlow) {
		// TRUE additive (ONE/ONE), RGB only. Flash/enemy glow draw into the
		// linear-HDR backbuffer BEFORE the post chain (OnPostTranslucenceDraw),
		// so bloom + tonemap compress the over-range result back into a soft
		// halo — matching the original cLuxEffectRenderer. (Drawing post-tonemap
		// with a SCREEN blend instead collapsed a white flash to a flat white
		// cutout, since dst + src*(1-dst) trends to 1.0 for any bright source.)
		desc.blend[0].blendEnable = true;
		desc.blend[0].colorOp = RI_BLEND_OP_ADD;
		desc.blend[0].alphaOp = RI_BLEND_OP_ADD;
		desc.blend[0].srcColor = RI_BLEND_ONE;
		desc.blend[0].dstColor = RI_BLEND_ONE;
		desc.blend[0].srcAlpha = RI_BLEND_ONE;
		desc.blend[0].dstAlpha = RI_BLEND_ONE;
		desc.blend[0].writeMask = RI_COLOR_WRITE_RGB; // RI default is RGBA
	} else if (aMode == eGeomPassMode_OutlineRim) {
		// Overwrite the glow color into the cleared offscreen target.
		desc.blend[0].blendEnable = false;
		desc.blend[0].writeMask = RI_COLOR_WRITE_RGBA;
	} else { // mark — no color
		desc.blend[0].blendEnable = false;
		// Writes NOTHING: the RI default is RGBA, so leaving this unset would
		// silently turn a depth/stencil-only pass into a colour write.
		desc.blend[0].writeMask = RI_COLOR_WRITE_NONE;
	}

	// aMode, abNormalPresent and abUvLayout are all structural now (blend +
	// stencil state, binding stride, attribute format), so no variant salt.
	aProgram.bindPipeline(&Interface<cGraphics>::Get()->device, apCmd, HASH_INITIAL_VALUE,
						  asDebugName, desc);
}

// Fullscreen-triangle pipeline (no vertex input, no depth, cull NONE). When
// abAdditive, blends ONE/ONE RGB-only into the bound color attachment.
void BindFullscreenPipeline(RIProgram &aProgram, RICmd *apCmd, bool abAdditive,
							hash_t aSeed, const char *asDebugName) {
	// No vertex input, no depth attachment, cull NONE.
	RIGraphicsPipelineDesc desc = {};
	desc.topology = RI_TOPOLOGY_TRIANGLE_LIST;
	desc.raster.polygonMode = RI_POLYGON_MODE_FILL;
	desc.raster.cullMode = RI_CULL_MODE_NONE;
	// The Vk create-info never assigned frontFace (zero == COUNTER_CLOCKWISE);
	// the RI default is CLOCKWISE, so spell it out.
	desc.raster.frontFace = RI_FRONT_FACE_COUNTER_CLOCKWISE;
	desc.raster.lineWidth = 1.0f;

	desc.renderTarget.colorCount = 1;
	desc.renderTarget.colorFormats[0] = cGraphics::PogoColorFormat;

	desc.blendCount = 1;
	if (abAdditive) {
		// SCREEN, not straight additive: this composites the glow onto the
		// pogo read half AFTER the tone-map / post chain has run, so the image
		// is already in display range. A plain ADD would push any bright pixel
		// past 1.0 and clip to white (the original cLuxEffectRenderer composited
		// pre-tone-map, so its additive glow got compressed back into range).
		// Screen — dst + src*(1-dst) — brightens toward white but never past it.
		//
		// The alpha slots take the ALPHA analogue of that equation, never the
		// _COLOR factor the colour slots use: D3D12 rejects a _COLOR blend
		// factor in SrcBlendAlpha / DestBlendAlpha outright (the PSO fails with
		// E_INVALIDARG), where Vulkan accepts it. Nothing observable rides on
		// the choice — writeMask is RGB and outline_composite.frag returns
		// alpha 0, so the alpha equation's result is discarded either way.
		desc.blend[0].blendEnable = true;
		desc.blend[0].colorOp = RI_BLEND_OP_ADD;
		desc.blend[0].alphaOp = RI_BLEND_OP_ADD;
		desc.blend[0].srcColor = RI_BLEND_ONE_MINUS_DST_COLOR;
		desc.blend[0].dstColor = RI_BLEND_ONE;
		desc.blend[0].srcAlpha = RI_BLEND_ONE_MINUS_DST_ALPHA;
		desc.blend[0].dstAlpha = RI_BLEND_ONE;
		desc.blend[0].writeMask = RI_COLOR_WRITE_RGB; // RI default is RGBA
	} else {
		desc.blend[0].blendEnable = false;
		desc.blend[0].writeMask = RI_COLOR_WRITE_RGBA;
	}

	aProgram.bindPipeline(&Interface<cGraphics>::Get()->device, apCmd, aSeed, asDebugName, desc);
}

// Bind a renderable's position + normal streams (bindings 0/1). Substitutes
// the global single-vertex fallback normal when the mesh lacks one; the caller
// zeroes that binding's stride in the pipeline (abNormalPresent=false).
bool BindGeomStreams(RICmd *apCmd, cVertexBuffer *apVB, bool *abNormalPresent) {
	auto *vbri = static_cast<cVertexBuffer *>(apVB);
	auto bufOf = [&](eVertexBufferElement type) -> RIBuffer * {
		const auto *element = vbri->GetElement(type);
		return element ? element->GetBuffer() : nullptr;
	};
	RIBuffer *pos = bufOf(eVertexBufferElement_Position);
	RIBuffer *idx = vbri->GetIndexRIBuffer();
	if (!pos || !idx)
		return false;
	RIBuffer *nrm = bufOf(eVertexBufferElement_Normal);
	if (abNormalPresent)
		*abNormalPresent = (nrm != nullptr);
	RIBuffer *vertBufs[2] = {pos, nrm ? nrm : &Interface<cGraphics>::Get()->fallbackNormalVertex};
	apCmd->bindVertexBuffers<2>(0, 2, vertBufs);
	apCmd->bindIndexBuffer(&Interface<cGraphics>::Get()->device, idx, 0, RI_INDEX_TYPE_32);
	return true;
}

// Bind position (binding 0) + texcoord (binding 1) for the alpha-tested
// cutout outline. Returns false if the mesh has no UVs (caller falls back to
// the solid path).
bool BindGeomStreamsUv(RICmd *apCmd, cVertexBuffer *apVB) {
	auto *vbri = static_cast<cVertexBuffer *>(apVB);
	auto bufOf = [&](eVertexBufferElement type) -> RIBuffer * {
		const auto *element = vbri->GetElement(type);
		return element ? element->GetBuffer() : nullptr;
	};
	RIBuffer *pos = bufOf(eVertexBufferElement_Position);
	RIBuffer *uv = bufOf(eVertexBufferElement_Texture0);
	RIBuffer *idx = vbri->GetIndexRIBuffer();
	if (!pos || !uv || !idx)
		return false;
	RIBuffer *vertBufs[2] = {pos, uv};
	apCmd->bindVertexBuffers<2>(0, 2, vertBufs);
	apCmd->bindIndexBuffer(&Interface<cGraphics>::Get()->device, idx, 0, RI_INDEX_TYPE_32);
	return true;
}

} // namespace

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

cLuxEffectRenderer::cLuxEffectRenderer() : iLuxUpdateable("LuxEffectRenderer")
{
	mpViewport = NULL;
	mbProgramsLoaded = false;
	mbOutlineColorInit = false;
	mbBlurInit = false;

	mFlashOscill.SetUp(0, 1, 0, 1, 1);

	Reset();
}

//-----------------------------------------------------------------------

cLuxEffectRenderer::~cLuxEffectRenderer()
{
	if(mbProgramsLoaded)
	{
		RIDevice *pDevice = &Interface<cGraphics>::Get()->device;
		mGeomProgram.dispose(pDevice);
		mAlphaProgram.dispose(pDevice);
		mGlowProgram.dispose(pDevice);
		mBlurProgram.dispose(pDevice);
		mCompositeProgram.dispose(pDevice);
	}
	DestroyPostEffectColorTarget(m_outlineColor);
	DestroyPostEffectColorTarget(m_blur[0]);
	DestroyPostEffectColorTarget(m_blur[1]);
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxEffectRenderer::Reset()
{
	mvOutlineObjects.clear();
	ClearRenderLists();
}

void cLuxEffectRenderer::ClearRenderLists()
{
	mvFlashObjects.clear();
	mvEnemyGlowObjects.clear();
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::Update(float afTimeStep)
{
	mFlashOscill.Update(afTimeStep);
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::AttachViewport(cViewport* apViewport)
{
	mpViewport = apViewport;
	mTranslucenceDrawHandler = EventHandler<const PostTranslucenceDrawCtx&>(
		[this](const PostTranslucenceDrawCtx& ctx){ OnPostTranslucenceDraw(ctx); });
	mPostWorldDrawHandler = EventHandler<const PostWorldDrawCtx&>(
		[this](const PostWorldDrawCtx& ctx){ OnPostWorldDraw(ctx); });
	if (mpViewport) {
		mTranslucenceDrawHandler.Connect(mpViewport->OnPostTranslucenceDraw());
		mPostWorldDrawHandler.Connect(mpViewport->OnPostWorldDraw());
	}
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::AddOutlineObject(iRenderable *apObject)
{
	mvOutlineObjects.push_back(apObject);
}

void cLuxEffectRenderer::ClearOutlineObjects()
{
	mvOutlineObjects.clear();
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::AddFlashObject(iRenderable *apObject, float afAlpha)
{
	mvFlashObjects.push_back(cGlowObject(apObject, afAlpha) );
}

void cLuxEffectRenderer::AddEnemyGlow(iRenderable *apObject, float afAlpha)
{
	mvEnemyGlowObjects.push_back(cGlowObject(apObject, afAlpha) );
}

//-----------------------------------------------------------------------

//////////////////////////////////////////////////////////////////////////
// PRIVATE METHODS
//////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------

void cLuxEffectRenderer::EnsurePrograms()
{
	if (mbProgramsLoaded) return;
	cResources *pResources = gpBase->mpEngine->GetResources();

	LoadSlangGraphics(&Interface<cGraphics>::Get()->device, mGeomProgram, pResources,
					  "outline_geom.vert", "outline_geom.frag");
	LoadSlangGraphics(&Interface<cGraphics>::Get()->device, mAlphaProgram, pResources,
					  "outline_alpha.vert", "outline_alpha.frag");
	LoadSlangGraphics(&Interface<cGraphics>::Get()->device, mGlowProgram, pResources,
					  "glow_object.vert", "glow_object.frag");
	LoadSlangGraphics(&Interface<cGraphics>::Get()->device, mBlurProgram, pResources,
					  "posteffect_fullscreen.vert",
					  "posteffect_bloom_blur.frag");
	LoadSlangGraphics(&Interface<cGraphics>::Get()->device, mCompositeProgram, pResources,
					  "posteffect_fullscreen.vert",
					  "outline_composite.frag");
	mbProgramsLoaded = true;
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::EnsureTargets(uint32_t alWidth, uint32_t alHeight)
{
	if (!m_outlineColor.valid || m_outlineColor.width != alWidth ||
		m_outlineColor.height != alHeight) {
		DestroyPostEffectColorTarget(m_outlineColor);
		CreatePostEffectColorTarget(m_outlineColor, alWidth, alHeight,
									cGraphics::PogoColorFormat, 0u,
									"LuxOutline.color");
		mbOutlineColorInit = true;
	}

	const uint32_t blurW = std::max(alWidth / 4u, 1u);
	const uint32_t blurH = std::max(alHeight / 4u, 1u);
	if (!m_blur[0].valid || m_blur[0].width != blurW ||
		m_blur[0].height != blurH) {
		DestroyPostEffectColorTarget(m_blur[0]);
		DestroyPostEffectColorTarget(m_blur[1]);
		CreatePostEffectColorTarget(m_blur[0], blurW, blurH,
									cGraphics::PogoColorFormat, 0u,
									"LuxOutline.blur0");
		CreatePostEffectColorTarget(m_blur[1], blurW, blurH,
									cGraphics::PogoColorFormat, 0u,
									"LuxOutline.blur1");
		mbBlurInit = true;
	}
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::OnPostWorldDraw(const PostWorldDrawCtx &ctx)
{
	// Only the hover outline composites over the FINAL tone-mapped image here;
	// flash + enemy glow draw additively pre-tonemap (OnPostTranslucenceDraw).
	if (mvOutlineObjects.empty()) return;

	// Everything per-viewport / per-frame arrives through the context — no
	// captured mpViewport, no reach into the RI global for the command buffer.
	RI_PogoBuffer *pPogo = ctx.pogo;
	if (pPogo == NULL) return;

	cFrustum *pFrustum = ctx.frustum;
	if (pFrustum == NULL) return;

	RITextureView *pDepthView = ctx.depthView;
	if (pDepthView == NULL) return;

	const uint32_t w = ctx.width;
	const uint32_t h = ctx.height;
	if (w == 0 || h == 0) return;

	EnsurePrograms();
	EnsureTargets(w, h);

	RICmd *pCmd = ctx.cmd;

	/////////////////////////////
	// Per-frame view/viewProj UBO
	OutlinePassUBO ubo = {};
	{
		const ml::float4x4 viewProj =
			pFrustum->GetProjectionMat() * pFrustum->GetViewMat();
		const ml::float4x4 view = pFrustum->GetViewMat();
		std::memcpy(ubo.viewProj, viewProj.a, sizeof(ubo.viewProj));
		std::memcpy(ubo.view, view.a, sizeof(ubo.view));
	}
	RIDescriptor passDesc = {};
	Interface<cGraphics>::Get()->UpdateFrameUBO(&passDesc, (void *)&ubo, sizeof(ubo));

	/////////////////////////////
	// Outline silhouette + blur into the offscreen scratch (untouched pogo)
	RenderOutline(ctx, passDesc);

	/////////////////////////////
	// Composite the blurred outline into the pogo read half (post-tonemap).
	const uint32_t readIdx = (pPogo->attachmentIndex + 1u) % 2u;
	RITexture *pReadTex = pPogo->textures[readIdx].Get();

	// Read half: SHADER_RESOURCE -> RENDER_TARGET (composite appends).
	pCmd->vk_d3d12_textureBarrier(RI_PogoAttachmentBarrier(pReadTex, /*initial=*/false));

	RIRect scissor = {};
	scissor.width = w;
	scissor.height = h;

	// (a) Composite the blurred outline — fullscreen, no depth.
	{
		RIRenderingAttachment color = {};
		color.view = *pPogo->attachmentView[readIdx];
		color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
		color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

		RIBeginRenderingDesc beginDesc = {};
		beginDesc.renderArea.width = w;
		beginDesc.renderArea.height = h;
		beginDesc.colorCount = 1;
		beginDesc.colors = &color;
		pCmd->vk_d3d12_beginRendering(&Interface<cGraphics>::Get()->device, beginDesc);

		// Negative height: the engine convention shared with posteffect_fullscreen.vert.
		RIViewport fsViewport = {};
		fsViewport.y = (float)h;
		fsViewport.width = (float)w;
		fsViewport.height = -(float)h;
		fsViewport.depthMax = 1.0f;
		pCmd->setViewport(&Interface<cGraphics>::Get()->device, fsViewport);
		pCmd->setScissor(&Interface<cGraphics>::Get()->device, scissor);

		BindFullscreenPipeline(mCompositeProgram, pCmd, /*additive=*/true,
							   hash_u32(HASH_INITIAL_VALUE, 0u),
							   "LuxOutline.composite");
		auto pSampler = Interface<cGraphics>::Get()->resolve_filter_descriptor(
			eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
			eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].descriptor = *pSampler;
		bindings[0].handle = DescriptorBindingID::Create("inputSampler");
		bindings[1].descriptor = m_blur[1].descriptor();
		bindings[1].handle = DescriptorBindingID::Create("blurInput");
		mCompositeProgram.bindDescriptors(ctx.device, pCmd, ctx.frameIndex, bindings, 2);
		pCmd->draw(&Interface<cGraphics>::Get()->device, 3, 1, 0, 0);

		pCmd->vk_d3d12_endRendering(&Interface<cGraphics>::Get()->device);

		// Restore blur[1]'s rest state (RENDER_TARGET) for the next frame's
		// ping-pong — matches the Bloom blur convention.
		EmitImageBarrier(pCmd, &m_blur[1].texture, RI_RESOURCE_STATE_SHADER_RESOURCE,
						 RI_STAGE_FRAGMENT, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
	}

	// Read half back to SHADER_RESOURCE for delivery.
	pCmd->vk_d3d12_textureBarrier(RI_PogoShaderBarrier(pReadTex, /*initial=*/false));
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::OnPostTranslucenceDraw(const PostTranslucenceDrawCtx &ctx)
{
	const bool bHasFlash = !mvFlashObjects.empty();
	const bool bHasEnemy = !mvEnemyGlowObjects.empty();
	if (!bHasFlash && !bHasEnemy) return;

	cFrustum *pFrustum = ctx.frustum;
	if (pFrustum == NULL || ctx.viewport == NULL) return;

	RITextureView *pDepthView = ctx.depthView;
	if (pDepthView == NULL) return;

	const uint32_t w = ctx.width;
	const uint32_t h = ctx.height;
	if (w == 0 || h == 0) return;

	EnsurePrograms();

	RICmd *pCmd = ctx.cmd;

	// The BackBuffer is the scene image at the negotiated RENDER extent;
	// ctx.width/ctx.height are that render extent, and ctx.viewMat/ctx.projMat
	// are the renderer's ACTUAL jittered raster matrices delivered on this
	// context. They therefore keep these draws aligned with the rasterized
	// scene at any render extent and any jitter. The guard band remains disabled,
	// so the BackBuffer's valid rectangle is the whole image.
	cViewport::BackBuffer bb = ctx.viewport->GetBackBuffer();
	if (bb.renderTarget.isEmpty()) return;

	/////////////////////////////
	// Per-frame view/viewProj UBO (HDR-space, actual jittered raster matrices)
	OutlinePassUBO ubo = {};
	{
		ml::float4x4 view = {};
		ml::float4x4 proj = {};
		std::memcpy(view.a, ctx.viewMat, sizeof(view.a));
		std::memcpy(proj.a, ctx.projMat, sizeof(proj.a));
		const ml::float4x4 viewProj = proj * view;
		std::memcpy(ubo.viewProj, viewProj.a, sizeof(ubo.viewProj));
		std::memcpy(ubo.view, view.a, sizeof(ubo.view));
	}
	RIDescriptor passDesc = {};
	Interface<cGraphics>::Get()->UpdateFrameUBO(&passDesc, (void *)&ubo, sizeof(ubo));

	// Borrow the BackBuffer: SHADER_RESOURCE -> RENDER_TARGET for the additive
	// draws, then back to SHADER_RESOURCE so the feed blit finds it as the
	// renderer left it.
	EmitImageBarrier(pCmd, &bb.renderTarget, RI_RESOURCE_STATE_SHADER_RESOURCE,
					 RI_STAGE_FRAGMENT, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);

	RIViewport flippedViewport = {};
	flippedViewport.y = (float)h;
	flippedViewport.width = (float)w;
	flippedViewport.height = -(float)h;
	flippedViewport.depthMax = 1.0f;
	RIRect scissor = {};
	scissor.width = w;
	scissor.height = h;

	{
		RIRenderingAttachment color = {};
		color.view = bb.renderTargetAttachmentView;
		color.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
		color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

		// Scene depth, read-only (left in DEPTH_ATTACHMENT_OPTIMAL by Draw): the
		// glow depth-tests but the pipeline disables depth writes.
		RIRenderingAttachment depth = {};
		depth.view = *pDepthView;
		depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
		depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

		RIBeginRenderingDesc beginDesc = {};
		beginDesc.renderArea.width = w;
		beginDesc.renderArea.height = h;
		beginDesc.colorCount = 1;
		beginDesc.colors = &color;
		beginDesc.depthStencil = &depth;
		pCmd->vk_d3d12_beginRendering(&Interface<cGraphics>::Get()->device, beginDesc);

		pCmd->setViewport(&Interface<cGraphics>::Get()->device, flippedViewport);
		pCmd->setScissor(&Interface<cGraphics>::Get()->device, scissor);

		auto pDiffSampler = Interface<cGraphics>::Get()->resolve_filter_descriptor(
			eTextureWrap_Repeat, eTextureWrap_Repeat, eTextureWrap_Repeat,
			eTextureFilter_Bilinear);

		// Port of dds_flash / dds_enemy_glow: an additive glow that FILLS the
		// object's visible surface, modulated by its own diffuse texture and a
		// bluish tint. The tint is a LINEAR multiplier into the HDR scene (no sRGB
		// decode — matches the original in-shader maths). Flash alpha-tests and
		// draws twice; enemy draws once. Both pulse with the shared oscillation.
		auto drawObjects = [&](std::vector<cGlowObject> &avObjects, const float aTint[3],
							   float afGlobalAlpha, bool abAlphaTest, int alDrawCount) {
			for (size_t i = 0; i < avObjects.size(); ++i) {
				iRenderable *pObject = avObjects[i].mpObject;
				if (pObject == NULL) continue;
				if (pObject->CollidesWithFrustum(pFrustum) == false) continue;
				cVertexBuffer *pVB = pObject->GetVertexBuffer();
				if (pVB == NULL) continue;

				// The glow samples the object's diffuse texture, so it needs both
				// UVs and a diffuse image; skip the object if either is missing.
				cMaterial *pMat = pObject->GetMaterial();
				Image *pDiffImage = pMat ? pMat->GetImage(eMaterialTexture_Diffuse) : NULL;
				cTexture *diffTex =
					pDiffImage ? pDiffImage->GetTexture() : nullptr;
				if (!diffTex || diffTex->view.isEmpty()) continue;

				if (!BindGeomStreamsUv(pCmd, pVB)) continue;
				BindGeomPipeline(mGlowProgram, pCmd, eGeomPassMode_Glow,
								 /*normalPresent=*/false, /*uvLayout=*/true, "LuxEffect.glow");

				OutlineAlphaPushConstants pc = {};
				cMatrixf *pMtx = pObject->GetModelMatrix(pFrustum);
				const ml::float4x4 model =
					cMath::ToFloatTranspose4x4(pMtx ? *pMtx : cMatrixf::Identity);
				std::memcpy(pc.model, model.a, sizeof(pc.model));
				pc.color[0] = hpl::sRGBToLinear(aTint[0]);
				pc.color[1] = hpl::sRGBToLinear(aTint[1]);
				pc.color[2] = hpl::sRGBToLinear(aTint[2]);
				pc.color[3] = avObjects[i].mfAlpha * afGlobalAlpha;
				pc.params[0] = abAlphaTest ? 1.0f : 0.0f;
				pc.params[1] =
					(RIFormatChannelCount(diffTex->format) == 1) ? 1.0f : 0.0f;

				RIProgram::DescriptorBinding bindings[3] = {};
				bindings[0].descriptor = passDesc;
				bindings[0].handle = DescriptorBindingID::Create("pass");
				bindings[1].descriptor = *pDiffSampler;
				bindings[1].handle = DescriptorBindingID::Create("diffuseSampler");
				bindings[2].descriptor = diffTex->descriptor();
				bindings[2].handle = DescriptorBindingID::Create("diffuseMap");
				mGlowProgram.bindDescriptors(ctx.device, pCmd, ctx.frameIndex, bindings, 3);

				// Both draws below share these constants, so this stays outside the
				// loop -- nothing between them invalidates root arguments.
				pCmd->vk_d3d12_setPushConstants(&Interface<cGraphics>::Get()->device,
												mGlowProgram, 0, sizeof(pc), &pc);

				for (int d = 0; d < alDrawCount; ++d)
					pCmd->drawIndexed(&Interface<cGraphics>::Get()->device, (uint32_t)pVB->GetIndexNum(), 1, 0, 0, 0);
			}
		};

		// Shared pulse (0.5..1.0), matching the original's single fGlobalAlpha.
		const float fGlobalAlpha = (0.5f + mFlashOscill.val * 0.5f) * 0.1f;
		if (bHasFlash)
			drawObjects(mvFlashObjects, kFlashGlow, fGlobalAlpha,
						/*alphaTest=*/true, /*drawCount=*/2);
		if (bHasEnemy)
			drawObjects(mvEnemyGlowObjects, kEnemyGlow, fGlobalAlpha,
						/*alphaTest=*/false, /*drawCount=*/1);

		pCmd->vk_d3d12_endRendering(&Interface<cGraphics>::Get()->device);
	}

	// Return the BackBuffer to SHADER_RESOURCE for the feed blit.
	EmitImageBarrier(pCmd, &bb.renderTarget, RI_RESOURCE_STATE_RENDER_TARGET,
					 RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::RenderOutline(const PostWorldDrawCtx &ctx,
									   const RIDescriptor &aPassDesc)
{
	RICmd *apCmd = ctx.cmd;
	cFrustum *apFrustum = ctx.frustum;
	const uint32_t alWidth = ctx.width;
	const uint32_t alHeight = ctx.height;

	// Depth view comes from the context; the depth *texture* (for the stencil-
	// aspect barrier) is reached through the context's viewport — both belong to
	// the firing viewport, delivered rather than captured.
	RITexture *pDepthTex = ctx.viewport ? ctx.viewport->GetDepthTexture() : NULL;
	RITextureView *pDepthView = ctx.depthView;
	if (pDepthTex == NULL || pDepthView == NULL) return;

	// Offscreen color: (UNDEFINED on first use) -> RENDER_TARGET.
	EmitImageBarrier(apCmd, &m_outlineColor.texture,
					 mbOutlineColorInit ? RI_RESOURCE_STATE_UNDEFINED
										: RI_RESOURCE_STATE_SHADER_RESOURCE,
					 mbOutlineColorInit ? RI_STAGE_NONE : RI_STAGE_FRAGMENT,
					 RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
	mbOutlineColorInit = false;

	// Stencil aspect UNDEFINED -> STENCIL_ATTACHMENT_OPTIMAL (the depth aspect
	// is already DEPTH_ATTACHMENT_OPTIMAL, left by HybridRenderer::Draw).
	// RI_BARRIER_ASPECT_STENCIL addresses the single aspect: it selects Vulkan's
	// separate stencil layout and D3D12 plane 1. Zero stage hints derive the
	// early/late fragment-test stages from DEPTH_WRITE. Built inline rather than
	// through EmitImageBarrier, which has no aspect parameter.
	apCmd->vk_d3d12_textureBarrier(
		RITextureBarrier(pDepthTex, RI_RESOURCE_STATE_UNDEFINED,
						 RI_RESOURCE_STATE_DEPTH_WRITE, RI_STAGE_NONE,
						 RI_STAGE_NONE, RI_BARRIER_ASPECT_STENCIL));

	// Cull list once.
	std::vector<iRenderable *> lstObjects;
	lstObjects.reserve(mvOutlineObjects.size());
	for (size_t i = 0; i < mvOutlineObjects.size(); ++i) {
		iRenderable *pObject = mvOutlineObjects[i];
		if (pObject && pObject->CollidesWithFrustum(apFrustum))
			lstObjects.push_back(pObject);
	}

	{
		RIRenderingAttachment color = {};
		color.view = m_outlineColor.attachmentView;
		color.loadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
		color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
		// clearValue.color defaults to {0,0,0,0}.

		// Depth LOADs (DEPTH_ATTACHMENT_OPTIMAL); stencil CLEARs to 0 and is not
		// stored. hasStencil binds the depth aspect as DEPTH_ATTACHMENT_OPTIMAL
		// and the stencil aspect as STENCIL_ATTACHMENT_OPTIMAL, matching the
		// original split attachments.
		RIRenderingAttachment depth = {};
		depth.view = *pDepthView;
		depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
		depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
		depth.hasStencil = true;
		depth.stencilLoadOp = RI_ATTACHMENT_LOAD_OP_CLEAR;
		depth.stencilStoreOp = RI_ATTACHMENT_STORE_OP_DONT_CARE;
		depth.clearValue.stencil = 0;

		RIBeginRenderingDesc beginDesc = {};
		beginDesc.renderArea.width = alWidth;
		beginDesc.renderArea.height = alHeight;
		beginDesc.colorCount = 1;
		beginDesc.colors = &color;
		beginDesc.depthStencil = &depth;
		apCmd->vk_d3d12_beginRendering(&Interface<cGraphics>::Get()->device, beginDesc);

		RIViewport flippedViewport = {};
		flippedViewport.y = (float)alHeight;
		flippedViewport.width = (float)alWidth;
		flippedViewport.height = -(float)alHeight;
		flippedViewport.depthMax = 1.0f;
		RIRect scissor = {};
		scissor.width = alWidth;
		scissor.height = alHeight;
		apCmd->setViewport(&Interface<cGraphics>::Get()->device, flippedViewport);
		apCmd->setScissor(&Interface<cGraphics>::Get()->device, scissor);

		// Two passes: (1) mark stencil from the original silhouette, (2) draw
		// the glow color from the scaled-out mesh where stencil != 1 (the rim).
		for (int pass = 0; pass < 2; ++pass) {
			const eGeomPassMode mode =
				(pass == 0) ? eGeomPassMode_OutlineMark : eGeomPassMode_OutlineRim;
			for (size_t i = 0; i < lstObjects.size(); ++i) {
				iRenderable *pObject = lstObjects[i];
				cVertexBuffer *pVB = pObject->GetVertexBuffer();
				if (pVB == NULL) continue;

				// Model matrix; pass 1 scales the mesh out ~2% about its local
				// centre so the rim sits just outside the silhouette.
				cMatrixf *pMtx = pObject->GetModelMatrix(apFrustum);
				cMatrixf mtxWorld = pMtx ? *pMtx : cMatrixf::Identity;
				if (pass == 1) {
					cBoundingVolume *pBV = pObject->GetBoundingVolume();
					cVector3f vLocalSize = pBV->GetLocalMax() - pBV->GetLocalMin();
					cVector3f vScale = (cVector3f(1.0f) / vLocalSize) * kOutlineScaleAdd +
									   cVector3f(1.0f);
					cMatrixf mtxScale = cMath::MatrixMul(
						cMath::MatrixScale(vScale),
						cMath::MatrixTranslate(pBV->GetLocalCenter() * -1));
					mtxScale.SetTranslation(mtxScale.GetTranslation() +
											pBV->GetLocalCenter());
					mtxWorld = cMath::MatrixMul(mtxWorld, mtxScale);
				}
				const ml::float4x4 model = cMath::ToFloatTranspose4x4(mtxWorld);

				// Alpha-tested (cutout) materials: sample the Alpha texture and
				// discard so the mark + rim follow the visible silhouette. Falls
				// back to the solid path if the mesh has no UVs.
				cMaterial *pMat = pObject->GetMaterial();
				Image *pAlphaImage = pMat ? pMat->GetImage(eMaterialTexture_Alpha) : NULL;
				cTexture *alphaTex =
					pAlphaImage ? pAlphaImage->GetTexture() : nullptr;

				bool bDrewAlpha = false;
				if (alphaTex && !alphaTex->view.isEmpty() && BindGeomStreamsUv(apCmd, pVB)) {
					BindGeomPipeline(mAlphaProgram, apCmd, mode, /*normalPresent=*/false,
									 /*uvLayout=*/true, "LuxOutline.alpha");

					OutlineAlphaPushConstants pc = {};
					std::memcpy(pc.model, model.a, sizeof(pc.model));
					pc.color[0] = sRGBToLinear(kOutlineGlow[0]);
					pc.color[1] = sRGBToLinear(kOutlineGlow[1]);
					pc.color[2] = sRGBToLinear(kOutlineGlow[2]);
					pc.color[3] = 1.0f;
					pc.params[0] =
						(RIFormatChannelCount(alphaTex->format) == 1) ? 1.0f : 0.0f;

					auto pSampler = Interface<cGraphics>::Get()->resolve_filter_descriptor(
						eTextureWrap_Repeat, eTextureWrap_Repeat, eTextureWrap_Repeat,
						eTextureFilter_Bilinear);
					RIProgram::DescriptorBinding bindings[3] = {};
					bindings[0].descriptor = aPassDesc;
					bindings[0].handle = DescriptorBindingID::Create("pass");
					bindings[1].descriptor = *pSampler;
					bindings[1].handle = DescriptorBindingID::Create("alphaSampler");
					bindings[2].descriptor = alphaTex->descriptor();
					bindings[2].handle = DescriptorBindingID::Create("alphaMap");
					mAlphaProgram.bindDescriptors(ctx.device, apCmd, ctx.frameIndex, bindings, 3);

					// After bindDescriptors: on D3D12 binding a root signature
					// invalidates every root argument, and this loop alternates
					// between the alpha and geom signatures per object.
					apCmd->vk_d3d12_setPushConstants(&Interface<cGraphics>::Get()->device,
													 mAlphaProgram, 0, sizeof(pc), &pc);

					apCmd->drawIndexed(&Interface<cGraphics>::Get()->device, (uint32_t)pVB->GetIndexNum(), 1, 0, 0, 0);
					bDrewAlpha = true;
				}
				if (bDrewAlpha) continue;

				// Solid path.
				bool bNormalPresent = false;
				if (!BindGeomStreams(apCmd, pVB, &bNormalPresent)) continue;
				BindGeomPipeline(mGeomProgram, apCmd, mode, bNormalPresent,
								 /*uvLayout=*/false, "LuxOutline.geom");

				OutlinePushConstants pc = {};
				std::memcpy(pc.model, model.a, sizeof(pc.model));
				pc.color[0] = sRGBToLinear(kOutlineGlow[0]);
				pc.color[1] = sRGBToLinear(kOutlineGlow[1]);
				pc.color[2] = sRGBToLinear(kOutlineGlow[2]);
				pc.color[3] = 1.0f;
				RIProgram::DescriptorBinding b = {};
				b.descriptor = aPassDesc;
				b.handle = DescriptorBindingID::Create("pass");
				mGeomProgram.bindDescriptors(ctx.device, apCmd, ctx.frameIndex, &b, 1);

				// After bindDescriptors, for the same root-signature reason as the
				// alpha path above.
				apCmd->vk_d3d12_setPushConstants(&Interface<cGraphics>::Get()->device,
												 mGeomProgram, 0, sizeof(pc), &pc);

				apCmd->drawIndexed(&Interface<cGraphics>::Get()->device, (uint32_t)pVB->GetIndexNum(), 1, 0, 0, 0);
			}
		}

		apCmd->vk_d3d12_endRendering(&Interface<cGraphics>::Get()->device);
	}

	// Offscreen color -> SHADER_RESOURCE for the blur to sample.
	EmitImageBarrier(apCmd, &m_outlineColor.texture, RI_RESOURCE_STATE_RENDER_TARGET,
					 RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);

	BlurOutline(apCmd, m_blur[0].width, m_blur[0].height);
}

//-----------------------------------------------------------------------

void cLuxEffectRenderer::BlurOutline(RICmd *apCmd, uint32_t alBlurW, uint32_t alBlurH)
{
	// First-use rest states (Bloom pattern): blur[0] SHADER_RESOURCE, blur[1]
	// RENDER_TARGET.
	if (mbBlurInit) {
		EmitImageBarrier(apCmd, &m_blur[0].texture, RI_RESOURCE_STATE_UNDEFINED,
						 RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
		EmitImageBarrier(apCmd, &m_blur[1].texture, RI_RESOURCE_STATE_UNDEFINED,
						 RI_STAGE_NONE, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
		mbBlurInit = false;
	}

	auto pSampler = Interface<cGraphics>::Get()->resolve_filter_descriptor(
		eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
		eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);

	// Negative height: the engine convention shared with posteffect_fullscreen.vert.
	RIViewport viewport = {};
	viewport.y = (float)alBlurH;
	viewport.width = (float)alBlurW;
	viewport.height = -(float)alBlurH;
	viewport.depthMax = 1.0f;
	RIRect scissor = {};
	scissor.width = alBlurW;
	scissor.height = alBlurH;

	auto blurPass = [&](const RITextureView &destView, RITexture *destTexture,
						RITexture *prevDestTexture, const RIDescriptor &inputDesc,
						float dirX, float dirY) {
		EmitImageBarrier(apCmd, destTexture, RI_RESOURCE_STATE_SHADER_RESOURCE,
						 RI_STAGE_FRAGMENT, RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE);
		EmitImageBarrier(apCmd, prevDestTexture, RI_RESOURCE_STATE_RENDER_TARGET,
						 RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);

		RIRenderingAttachment color = {};
		color.view = destView;
		color.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
		RIBeginRenderingDesc beginDesc = {};
		beginDesc.renderArea.width = alBlurW;
		beginDesc.renderArea.height = alBlurH;
		beginDesc.colorCount = 1;
		beginDesc.colors = &color;
		apCmd->vk_d3d12_beginRendering(&Interface<cGraphics>::Get()->device, beginDesc);

		apCmd->setViewport(&Interface<cGraphics>::Get()->device, viewport);
		apCmd->setScissor(&Interface<cGraphics>::Get()->device, scissor);

		BindFullscreenPipeline(mBlurProgram, apCmd, /*additive=*/false,
							   hash_u32(HASH_INITIAL_VALUE, 1u), "LuxOutline.blur");
		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].descriptor = *pSampler;
		bindings[0].handle = DescriptorBindingID::Create("inputSampler");
		bindings[1].descriptor = inputDesc;
		bindings[1].handle = DescriptorBindingID::Create("sourceInput");
		mBlurProgram.bindDescriptors(&Interface<cGraphics>::Get()->device, apCmd, Interface<cGraphics>::Get()->frameIndex, bindings, 2);

		BlurPushConstants pc = {};
		pc.blurDir[0] = dirX;
		pc.blurDir[1] = dirY;
		apCmd->vk_d3d12_setPushConstants(&Interface<cGraphics>::Get()->device,
										 mBlurProgram, 0, sizeof(pc), &pc);
		apCmd->draw(&Interface<cGraphics>::Get()->device, 3, 1, 0, 0);
		apCmd->vk_d3d12_endRendering(&Interface<cGraphics>::Get()->device);
	};

	const float fBlurSize = 1.0f;
	for (int iter = 0; iter < 2; ++iter) {
		const RIDescriptor firstInput =
			(iter == 0) ? m_outlineColor.descriptor() : m_blur[1].descriptor();
		// H: dest blur[0], read firstInput, prevDest blur[1].
		blurPass(m_blur[0].attachmentView, &m_blur[0].texture,
				 &m_blur[1].texture, firstInput, fBlurSize, 0.0f);
		// V: dest blur[1], read blur[0], prevDest blur[0].
		blurPass(m_blur[1].attachmentView, &m_blur[1].texture,
				 &m_blur[0].texture, m_blur[0].descriptor(), 0.0f, fBlurSize);
	}

	// blur[1] COLOR_ATTACH -> SHADER_RESOURCE for the composite to sample.
	EmitImageBarrier(apCmd, &m_blur[1].texture, RI_RESOURCE_STATE_RENDER_TARGET,
					 RI_STAGE_NONE, RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT);
}

//-----------------------------------------------------------------------
