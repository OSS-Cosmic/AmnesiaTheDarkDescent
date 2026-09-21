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

#include "scene/Viewport.h"

#include "graphics/PostEffectComposite.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/Graphics.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "graphics/Renderer.h"
#include "graphics/StandardRenderer.h"
#include "graphics/TemporalReactiveMask.h"
#include "graphics/TemporalPresentation.h"
#include "graphics/WaterReflectionPass.h"

#include "resources/Resources.h"

#include "system/Hasher.h"
#include "system/LowLevelSystem.h"

#if (DEVICE_IMPL_VULKAN)
#include <vk_mem_alloc.h>
#endif

#include "scene/Camera.h"
#include "scene/Scene.h"
#include "scene/World.h"

#include <cmath>
#include <optional>
#include <cstring>
#include <functional>

namespace hpl {
namespace {

constexpr const char *kTemporalProviderInvalidRenderExtent =
		"provider returned an invalid render extent";
constexpr const char *kTemporalProviderCreationFailed =
		"provider creation failed";
constexpr const char *kTemporalProviderPreparationFailed =
		"provider context preparation failed";
constexpr const char *kTemporalProviderUnknownFailure =
		"unknown provider failure";

TemporalUpscalerExtent ToTemporalExtent(const cVector2l &aExtent)
{
	return {
		aExtent.x > 0 ? static_cast<uint32_t>(aExtent.x) : 0u,
		aExtent.y > 0 ? static_cast<uint32_t>(aExtent.y) : 0u};
}

cVector2l FromTemporalExtent(TemporalUpscalerExtent aExtent)
{
	return cVector2l(static_cast<int>(aExtent.width),
			static_cast<int>(aExtent.height));
}

TemporalRenderExtentOwner ToPolicyOwner(cViewport::eRenderExtentOwner aOwner)
{
	switch (aOwner) {
	case cViewport::eRenderExtentOwner::DevRenderScale:
		return TemporalRenderExtentOwner::DevRenderScale;
	case cViewport::eRenderExtentOwner::Provider:
		return TemporalRenderExtentOwner::Provider;
	case cViewport::eRenderExtentOwner::Native:
	default:
		return TemporalRenderExtentOwner::Native;
	}
}

cViewport::eRenderExtentOwner FromPolicyOwner(TemporalRenderExtentOwner aOwner)
{
	switch (aOwner) {
	case TemporalRenderExtentOwner::DevRenderScale:
		return cViewport::eRenderExtentOwner::DevRenderScale;
	case TemporalRenderExtentOwner::Provider:
		return cViewport::eRenderExtentOwner::Provider;
	case TemporalRenderExtentOwner::Native:
	default:
		return cViewport::eRenderExtentOwner::Native;
	}
}

bool EnsureViewportFeedProgram(std::shared_ptr<RIProgram> &program)
{
	if (program)
		return true;
	cResources *resources = Interface<cResources>::Get();
	if (!resources || !resources->GetFileSearcher())
		return false;
	// One source, two entry points: load per stage so D3D12 gets the per-entry
	// executable rather than the lib_6_8 library a multi-entry source compiles
	// to, which no graphics PSO can consume. Vulkan resolves both to the .spv.
	auto vsModule = RIProgram::loadShaderStage(resources->GetFileSearcher(),
			"viewport_feed.3d", "vsMain");
	auto psModule = RIProgram::loadShaderStage(resources->GetFileSearcher(),
			"viewport_feed.3d", "psMain");
	if (vsModule.empty() || psModule.empty())
		return false;
	std::array<RIProgram::ModuleStage, 2> stages = {
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_VERTEX, vsModule, "vsMain"},
			RIProgram::ModuleStage{RIProgram::PROGRAM_STAGE_FRAGMENT, psModule, "psMain"}};
	program = std::make_shared<RIProgram>();
	program->initialize(&Interface<cGraphics>::Get()->device, stages, {},
							"Viewport.Feed");
	return true;
}

bool IsViewportFeedCropValid(uint32_t sourceX, uint32_t sourceY,
							 uint32_t sourceWidth, uint32_t sourceHeight,
							 uint32_t sourceExtentWidth, uint32_t sourceExtentHeight)
{
	if (!sourceWidth || !sourceHeight || !sourceExtentWidth || !sourceExtentHeight)
		return false;
	return static_cast<uint64_t>(sourceX) + sourceWidth <= sourceExtentWidth &&
		static_cast<uint64_t>(sourceY) + sourceHeight <= sourceExtentHeight;
}

bool RecordViewportFeed(std::shared_ptr<RIProgram> &program, RICmd *cmd,
						RIDevice *device, uint32_t frameIndex, RITexture *source,
						RITextureView *sourceView, RIResourceState_e sourceEntry,
							uint32_t sourceEntryStage, RIResourceState_e sourceExit,
							uint32_t sourceExitStage, RITexture *destination,
							RITextureView *destinationView, uint32_t sourceX, uint32_t sourceY,
							uint32_t sourceWidth, uint32_t sourceHeight,
							uint32_t sourceExtentWidth, uint32_t sourceExtentHeight,
							uint32_t destinationWidth, uint32_t destinationHeight)
{
	if (!cmd || !device || !source || !sourceView || !destination ||
			!destinationView || !destinationWidth || !destinationHeight)
		return false;
	if (!IsViewportFeedCropValid(sourceX, sourceY, sourceWidth, sourceHeight,
			 sourceExtentWidth, sourceExtentHeight))
		return false;
		const bool copy = sourceWidth == destinationWidth &&
			sourceHeight == destinationHeight &&
			source->format != RI_FORMAT_UNKNOWN &&
			source->format == destination->format;
		const bool oneToOne = sourceWidth == destinationWidth &&
			sourceHeight == destinationHeight;
		cGraphics *graphics = Interface<cGraphics>::Get();
		if (!graphics)
			return false;
		if (!copy && !EnsureViewportFeedProgram(program))
			return false;
		std::optional<RIDescriptor> sampler;
		if (!copy)
			sampler = graphics->resolve_filter_descriptor(
					eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
					eTextureWrap_ClampToEdge,
					oneToOne ? eTextureFilter_Nearest : eTextureFilter_Bilinear);
		if (!copy && !sampler)
			return false;
	RITextureBarrier begin[2] = {
			RITextureBarrier(source, sourceEntry,
								copy ? RI_RESOURCE_STATE_COPY_SRC : RI_RESOURCE_STATE_SHADER_RESOURCE,
								 sourceEntryStage,
								copy ? RI_STAGE_COPY : RI_STAGE_FRAGMENT),
			RITextureBarrier(destination, RI_RESOURCE_STATE_UNDEFINED,
								copy ? RI_RESOURCE_STATE_COPY_DST : RI_RESOURCE_STATE_RENDER_TARGET,
								RI_STAGE_NONE, copy ? RI_STAGE_COPY : RI_STAGE_FRAGMENT)};
	cmd->vk_d3d12_textureBarriers<2>(2, begin);
	if (copy) {
		RIImageCopyDesc desc = {};
		desc.srcX = static_cast<int32_t>(sourceX);
		desc.srcY = static_cast<int32_t>(sourceY);
		desc.width = sourceWidth;
		desc.height = sourceHeight;
		desc.depth = 1;
		cmd->copyImage(device, source, destination, desc);
	} else {
		RIRenderingAttachment color = {};
		color.view = *destinationView;
		color.loadOp = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
		RIBeginRenderingDesc rendering = {};
		rendering.renderArea.width = destinationWidth;
		rendering.renderArea.height = destinationHeight;
		rendering.colorCount = 1;
		rendering.colors = &color;
		cmd->vk_d3d12_beginRendering(device, rendering);
		RIViewport viewport = {};
		viewport.y = static_cast<float>(destinationHeight);
		viewport.width = static_cast<float>(destinationWidth);
		viewport.height = -static_cast<float>(destinationHeight);
		viewport.depthMin = 0.0f;
		viewport.depthMax = 1.0f;
		cmd->setViewport(device, viewport);
		RIRect scissor = {};
		scissor.width = destinationWidth;
		scissor.height = destinationHeight;
		cmd->setScissor(device, scissor);
		const RIGraphicsPipelineDesc pipelineDesc = MakePostEffectPipelineDesc(
				static_cast<RI_Format_e>(destinationView->format), false);
		program->bindPipeline(device, cmd, HASH_INITIAL_VALUE, "Viewport.Feed",
				pipelineDesc);
		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].handle = DescriptorBindingID::Create("sourceSampler");
		bindings[0].descriptor = *sampler;
		bindings[1].handle = DescriptorBindingID::Create("sourceColor");
		bindings[1].descriptor = RIDescriptor::sampledImage(
				device, sourceView, RI_RESOURCE_STATE_SHADER_RESOURCE);
		program->bindDescriptors(device, cmd, frameIndex, bindings, 2);
		struct ViewportFeedPC {
			uint32_t sourceExtent[2];
			uint32_t pixelRectMin[2];
			uint32_t pixelRectMax[2];
		} push = {{sourceExtentWidth, sourceExtentHeight},
				  {sourceX, sourceY},
				  {sourceX + sourceWidth - 1u, sourceY + sourceHeight - 1u}};
		cmd->vk_d3d12_setPushConstants(device, *program, 0,
				sizeof(push), &push);
		cmd->draw(device, 3, 1, 0, 0);
		cmd->vk_d3d12_endRendering(device);
	}
	RITextureBarrier end[2] = {
			RITextureBarrier(source,
								copy ? RI_RESOURCE_STATE_COPY_SRC : RI_RESOURCE_STATE_SHADER_RESOURCE,
								 sourceExit, copy ? RI_STAGE_COPY : RI_STAGE_FRAGMENT,
								 sourceExitStage),
			RITextureBarrier(destination,
								copy ? RI_RESOURCE_STATE_COPY_DST : RI_RESOURCE_STATE_RENDER_TARGET,
								RI_RESOURCE_STATE_SHADER_RESOURCE,
								copy ? RI_STAGE_COPY : RI_STAGE_FRAGMENT, RI_STAGE_FRAGMENT)};
	cmd->vk_d3d12_textureBarriers<2>(2, end);
	return true;
}

} // namespace
} // namespace hpl

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

} // namespace hpl

void std::default_delete<hpl::WaterReflectionViewportState>::operator()(
    hpl::WaterReflectionViewportState *apState) const noexcept
{
    delete apState;
}

namespace hpl {

	//-----------------------------------------------------------------------

	cViewport::HybridViewportState::HybridViewportState() = default;

	cViewport::HybridViewportState::HybridViewportState(
		HybridViewportState &&rhs) noexcept = default;

	cViewport::cViewport(cScene *apScene)
	{
		mpScene = apScene;

		mbActive = true;
		mbVisible = true;

		mpRenderSettings = std::make_unique<cRenderSettings>();

		mpWorld = NULL;
		mpCamera = NULL;
		mpRenderer = NULL;
		mpPostEffectComposite = NULL;

		mbIsListener = false;
	}

	//-----------------------------------------------------------------------

	cViewport::~cViewport()
	{
		// GPU teardown of the viewport-owned targets: everything goes to the
		// frame freelist — freed once the in-flight pipeline is done with it
		// (or in cGraphics::Dispose at shutdown). No stall. The backend
		// state's resources are deferred by its destructor when m_state is
		// destroyed below; only the pogo buffer needs an explicit hand-off.
		cGraphics::FrameContext *cntx = Interface<cGraphics>::Get()->GetActiveSet();
		if (mpFeedProgram) {
			std::shared_ptr<RIProgram> keepProgram = std::move(mpFeedProgram);
			RIDevice *device = &Interface<cGraphics>::Get()->device;
			Interface<cGraphics>::Get()->graphicsDefer.push(std::function<void()>(
				[keepProgram = std::move(keepProgram), device]() mutable {
					keepProgram->dispose(device);
					keepProgram.reset();
				}));
		}
		if (mpTemporalPresentation)
			mpTemporalPresentation->Release(cntx);
		if (mpTemporalReactiveMask)
			mpTemporalReactiveMask->Release(cntx);
		ReleaseTemporalProvider();
		mTemporalReactiveMaskActive = false;
		ReleasePogoBuffer(cntx);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// TARGET / STATE VISITORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cVector2l cViewport::GetTargetSize() const
	{
		return std::visit(
			[](auto &&arg) -> cVector2l {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, TargetSwapchain>) {
					cGraphics* pGraphics = Interface<cGraphics>::Get();
					return cVector2l((int)pGraphics->swapchain->width,
									 (int)pGraphics->swapchain->height);
				} else {
					return cVector2l((int)arg.width, (int)arg.height);
				}
		},
			mTarget);
	}

	void cViewport::SetTarget(const Target &aTarget)
	{
		// Presentation images may still be referenced by an in-flight command
		// buffer. Release only through the graphics defer queue when the target
		// is replaced; the next Evaluate will recreate the presentation object.
		cGraphics *pGraphics = Interface<cGraphics>::Get();
		cGraphics::FrameContext *cntx =
			pGraphics ? pGraphics->GetActiveSet() : nullptr;
		if (mpTemporalPresentation)
		{
			mpTemporalPresentation->Release(cntx);
		}
		if (mpTemporalReactiveMask)
			mpTemporalReactiveMask->Release(cntx);
		ReleaseTemporalProvider();
		ClearTemporalProviderFailure();
		mTemporalReactiveMaskActive = false;
		mTemporalPresentationDepthValid = false;
		TemporalHistoryResetTriggers resetTriggers;
		resetTriggers.outputExtentChanged = true;
		mTemporalHistoryReset |= TemporalHistoryResetRequired(resetTriggers);
		mTarget = aTarget;
	}

	cVector2l cViewport::GetRenderExtent() const
	{
		const cVector2l displayExtent = GetDisplayExtent();
		if (displayExtent.x <= 0 || displayExtent.y <= 0)
			return displayExtent;

		if (mRenderExtent.x == 0 && mRenderExtent.y == 0)
			return displayExtent;

		const int renderWidth = mRenderExtent.x < 1 ? 1 : mRenderExtent.x;
		const int renderHeight = mRenderExtent.y < 1 ? 1 : mRenderExtent.y;
		return cVector2l(renderWidth > displayExtent.x ? displayExtent.x : renderWidth,
						 renderHeight > displayExtent.y ? displayExtent.y : renderHeight);
	}

	const TemporalUpscalerSettings &cViewport::GetTemporalUpscalerSettings() const
	{
		return mTemporalUpscalerSettings;
	}

	void cViewport::SetTemporalUpscalerSettings(
			const TemporalUpscalerSettings &aSettings)
	{
		mTemporalUpscalerRequestedSettings = aSettings;
		if (mpRenderSettings)
			mpRenderSettings->mTemporalUpscaler = aSettings;
		mTemporalUpscalerStatus.requestedProvider = aSettings.provider;
		mTemporalUpscalerStatus.requestedQuality = aSettings.quality;
		ClearTemporalProviderFailure();
	}

	uint32_t cViewport::GetTemporalJitterPhaseCount() const
	{
		return mbTemporalProviderPrepared ? mlTemporalJitterPhaseCount : 0;
	}

	bool cViewport::IsTemporalProviderPrepared() const
	{
		return mbTemporalProviderPrepared;
	}

	TemporalUpscalerStatus cViewport::GetTemporalUpscalerStatus() const
	{
		return mTemporalUpscalerStatus;
	}

	cTemporalReactiveMask *cViewport::GetTemporalReactiveMask()
	{
		return mTemporalReactiveMaskActive ? mpTemporalReactiveMask.get() : nullptr;
	}

	void cViewport::PublishRasterCamera(const float aViewMat[16],
											const float aProjMat[16])
	{
		// A raster-only publication must not retain a previous backend's
		// jitter/history. Temporal renderers publish their snapshot afterward.
		mRasterCamera = {};
		if (aViewMat == nullptr || aProjMat == nullptr)
		{
			mRasterCamera.valid = false;
			return;
		}

		std::memcpy(mRasterCamera.viewMat, aViewMat,
					sizeof(mRasterCamera.viewMat));
		std::memcpy(mRasterCamera.projMat, aProjMat,
					sizeof(mRasterCamera.projMat));
		cGraphics *pGraphics = Interface<cGraphics>::Get();
		mRasterCamera.frameIndex = pGraphics ? pGraphics->frameIndex : UINT32_MAX;
		mRasterCamera.valid = pGraphics != nullptr;
	}

	void cViewport::PublishRasterTemporalFrame(
			const TemporalFrameSnapshot &aFrame, float aDeltaTimeMs)
	{
		// Keep the actual raster pair in the same record as the temporal
		// snapshot. The renderer calls PublishRasterCamera immediately before
		// this method, but copying them here also makes this sibling API safe on
		// its own and still derives every field from the one snapshot.
		std::memcpy(mRasterCamera.viewMat, aFrame.viewMat,
					sizeof(mRasterCamera.viewMat));
		std::memcpy(mRasterCamera.projMat, aFrame.projMat,
					sizeof(mRasterCamera.projMat));
		std::memcpy(mRasterCamera.unjitteredProjMat, aFrame.unjitteredProjMat,
					sizeof(mRasterCamera.unjitteredProjMat));
		std::memcpy(mRasterCamera.previousViewMat, aFrame.prevViewMat,
					sizeof(mRasterCamera.previousViewMat));
		std::memcpy(mRasterCamera.previousUnjitteredProjMat,
					aFrame.prevUnjitteredProjMat,
					sizeof(mRasterCamera.previousUnjitteredProjMat));
		std::memcpy(mRasterCamera.jitterPixels, aFrame.jitterPixels,
					sizeof(mRasterCamera.jitterPixels));
		std::memcpy(mRasterCamera.previousJitterPixels,
					aFrame.prevJitterPixels,
					sizeof(mRasterCamera.previousJitterPixels));
		mRasterCamera.deltaTimeMs = aDeltaTimeMs;
		mRasterCamera.historyReset = aFrame.historyReset;
		cGraphics *pGraphics = Interface<cGraphics>::Get();
		mRasterCamera.frameIndex = pGraphics ? pGraphics->frameIndex : UINT32_MAX;
		mRasterCamera.valid = pGraphics != nullptr;
	}

	const cViewport::RasterCamera &cViewport::GetRasterCamera() const
	{
		return mRasterCamera;
	}

	bool cViewport::ConsumeTemporalHistoryReset()
	{
		const bool reset = mTemporalHistoryReset;
		mTemporalHistoryReset = false;
		return reset;
	}

	struct RITextureView* cViewport::GetRenderDepthView()
	{
		return std::visit(
			[](auto &&arg) -> struct RITextureView * {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, std::monostate>) {
					return nullptr;
				} else {
					const uint32_t swapchainIndex = Interface<cGraphics>::Get()->swapchainIndex;
					return arg.width > 0 && arg.height > 0 &&
						   !arg.depthView[swapchainIndex].isEmpty() &&
						   !arg.depthTextures[swapchainIndex].isEmpty()
						? arg.depthView[swapchainIndex].Get()
						: nullptr;
				}
			},
			m_state);
	}

	struct RITexture* cViewport::GetRenderDepthTexture()
	{
		return std::visit(
			[](auto &&arg) -> struct RITexture * {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, std::monostate>) {
					return nullptr;
				} else {
					const uint32_t swapchainIndex = Interface<cGraphics>::Get()->swapchainIndex;
					return arg.width > 0 && arg.height > 0 &&
						   !arg.depthTextures[swapchainIndex].isEmpty() &&
						   !arg.depthView[swapchainIndex].isEmpty()
						? arg.depthTextures[swapchainIndex].Get()
						: nullptr;
				}
			},
			m_state);
	}

	struct RITextureView* cViewport::GetRenderDepthSampleView()
	{
		return std::visit(
			[](auto &&arg) -> struct RITextureView * {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, std::monostate> ||
								  std::is_same_v<T, SimpleViewportState>) {
					return nullptr;
				} else {
					const uint32_t swapchainIndex = Interface<cGraphics>::Get()->swapchainIndex;
					return arg.width > 0 && arg.height > 0 &&
						   !arg.depthSampleView[swapchainIndex].isEmpty() &&
						   !arg.depthTextures[swapchainIndex].isEmpty()
						? arg.depthSampleView[swapchainIndex].Get()
						: nullptr;
				}
			},
			m_state);
	}

	bool cViewport::RenderExtentEqualsDisplayExtent() const
	{
		const cVector2l displayExtent = GetDisplayExtent();
		if (displayExtent.x <= 0 || displayExtent.y <= 0)
			return false;
		const cVector2l renderExtent = GetRenderExtent();
		return renderExtent.x == displayExtent.x &&
			   renderExtent.y == displayExtent.y;
	}

	cViewport::DisplayDepthResources cViewport::BuildDisplayDepthInputs() const
	{
		DisplayDepthResources resources = {};
		cGraphics *pGraphics = Interface<cGraphics>::Get();
		if (!pGraphics)
			return resources;

		const cVector2l displayExtent = GetDisplayExtent();
		if (displayExtent.x <= 0 || displayExtent.y <= 0)
			return resources;

		resources.inputs.displayExtent = {
			static_cast<uint32_t>(displayExtent.x),
			static_cast<uint32_t>(displayExtent.y)};

		if (mTemporalPresentationDepthValid && mpTemporalPresentation) {
			const TemporalUpscalerExtent extent = {
				resources.inputs.displayExtent.width,
				resources.inputs.displayExtent.height};
			resources.presentation.image =
				mpTemporalPresentation->GetDisplayDepthTexture(
					pGraphics->frameIndex, extent);
			resources.presentation.attachmentView =
				mpTemporalPresentation->GetDisplayDepthAttachmentView(
					pGraphics->frameIndex, extent);
			resources.inputs.presentation.allocatedExtent =
				resources.inputs.displayExtent;
			resources.inputs.presentation.hasImage =
				resources.presentation.image != nullptr;
			resources.inputs.presentation.hasAttachmentView =
				resources.presentation.attachmentView != nullptr;
			resources.inputs.presentationCurrent =
				resources.inputs.presentation.hasImage &&
				resources.inputs.presentation.hasAttachmentView;
		}

		resources.inputs.sceneIndexInRange =
			pGraphics->swapchainIndex < RI_MAX_SWAPCHAIN_IMAGES;
		resources.inputs.sceneExtentCompatible =
			RenderExtentEqualsDisplayExtent();
		if (resources.inputs.sceneIndexInRange) {
			const uint32_t swapchainIndex = pGraphics->swapchainIndex;
			std::visit(
				[&resources, swapchainIndex](auto &&arg) {
					using T = std::decay_t<decltype(arg)>;
					if constexpr (!std::is_same_v<T, std::monostate>) {
						resources.inputs.scene.allocatedExtent = {
							arg.width, arg.height};
						resources.inputs.scene.hasImage =
							!arg.depthTextures[swapchainIndex].isEmpty();
						resources.inputs.scene.hasAttachmentView =
							!arg.depthView[swapchainIndex].isEmpty();
						resources.scene.image =
							resources.inputs.scene.hasImage
								? arg.depthTextures[swapchainIndex].Get()
								: nullptr;
						resources.scene.attachmentView =
							resources.inputs.scene.hasAttachmentView
								? arg.depthView[swapchainIndex].Get()
								: nullptr;
					}
				},
				m_state);
		}

		return resources;
	}

	cViewport::DisplayDepthPair cViewport::ResolveDisplayDepth(
			const DisplayDepthExtent *apRequestedExtent) const
	{
		const DisplayDepthResources resources = BuildDisplayDepthInputs();
		const DisplayDepthSource source = apRequestedExtent
			? SelectDisplayDepthForExtent(resources.inputs,
												apRequestedExtent->width,
												apRequestedExtent->height)
			: SelectDisplayDepth(resources.inputs);
		switch (source) {
		case DisplayDepthSource::Presentation:
			return resources.presentation;
		case DisplayDepthSource::Scene:
			return resources.scene;
		default:
			return {};
		}
	}

	struct RITextureView* cViewport::GetDepthView()
	{
		return ResolveDisplayDepth().attachmentView;
	}

	struct RITexture* cViewport::GetDepthTexture()
	{
		return ResolveDisplayDepth().image;
	}

	struct RITextureView* cViewport::GetDepthViewForExtent(uint32_t alWidth,
														  uint32_t alHeight)
	{
		const DisplayDepthExtent requestedExtent = {alWidth, alHeight};
		return ResolveDisplayDepth(&requestedExtent).attachmentView;
	}

	cViewport::BackBuffer cViewport::GetBackBuffer()
	{
		return std::visit(
			[](auto &&arg) -> BackBuffer {
				using T = std::decay_t<decltype(arg)>;
				if constexpr (std::is_same_v<T, std::monostate>) {
					return BackBuffer{}; // zeroed — check renderTarget.vk.image
				} else {
					return arg.width > 0 && arg.height > 0 ? arg.GetBackBuffer()
														 : BackBuffer{};
				}
			},
			m_state);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// VIEWPORT TEXTURE HELPERS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

// Names a viewport image for validation and captures, so a layout, usage or
// uninitialized-resource error reports "StandardViewportState.depth" instead of
// a bare handle. RITexture::setDebugObjectName dispatches to
// vkSetDebugUtilsObjectNameEXT or ID3D12Object::SetName; going through it keeps
// D3D12 targets named too, which the old Vulkan-only body did not.
static void NameViewportImage(struct RIDevice *device, RITexture &texture,
							  const char *what) {
	if (what == nullptr || texture.isEmpty())
		return;
	texture.setDebugObjectName(device, what);
}

bool CreateViewportColorTexture(struct RIDevice *device, uint32_t width,
								uint32_t height, enum RI_Format_e format,
								uint32_t usage,
								RISharedPointer<RITexture> *tex,
								RISharedPointer<RITextureView> *view,
								const char *what) {
	RITextureDesc desc = {};
	desc.type = RI_TEXTURE_2D;
	desc.format = format;
	desc.width = width;
	desc.height = height;
	desc.usage = usage;
	RITexture t = RITexture::create(device, desc);
	if (t.isEmpty()) {
		Error("%s: failed to create %ux%u color image\n", what, width, height);
		*tex = {};
		return false;
	}

	RITextureViewDesc viewDesc = {};
	viewDesc.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
	viewDesc.format = format;
	viewDesc.mipNum = 1;
	viewDesc.layerNum = 1;
	RITextureView v = RITextureView::create(device, &t, viewDesc);
	if (v.isEmpty()) {
		Error("%s: failed to create image view\n", what);
		t.dispose(device);
		*tex = {};
		*view = {};
		return false;
	}
	NameViewportImage(device, t, what);
	*tex = RISharedPointer<RITexture>(device, t);
	*view = RISharedPointer<RITextureView>(device, v);
	return true;
}

// Depth / visibility creation — ported verbatim from the retired swapchain-init
// block in Graphics.cpp, parameterized by extent. The view aspect is derived
// from the format inside RITextureView::create.
bool CreateViewportAttachmentTexture(struct RIDevice *device, uint32_t width,
									 uint32_t height, enum RI_Format_e format,
									 uint32_t usage,
									 enum RITextureViewType_e viewType,
									 RISharedPointer<RITexture> *tex,
									 RISharedPointer<RITextureView> *view,
									 const char *what, uint32_t layerNum) {
	RITextureDesc desc = {};
	desc.type = RI_TEXTURE_2D;
	desc.format = format;
	desc.width = width;
	desc.height = height;
	desc.layerNum = layerNum;
	desc.usage = usage;
	RITexture t = RITexture::create(device, desc);
	if (t.isEmpty()) {
		Error("%s: failed to create %ux%u image\n", what, width, height);
		*tex = {};
		return false;
	}

	RITextureViewDesc viewDesc = {};
	viewDesc.viewType = viewType;
	viewDesc.format = format;
	viewDesc.mipNum = 1;
	viewDesc.layerNum = layerNum;
	RITextureView v = RITextureView::create(device, &t, viewDesc);
	if (v.isEmpty()) {
		Error("%s: failed to create image view\n", what);
		t.dispose(device);
		*tex = {};
		return false;
	}
	NameViewportImage(device, t, what);
	*tex = RISharedPointer<RITexture>(device, t);
	*view = RISharedPointer<RITextureView>(device, v);
	return true;
}

bool CreateViewportColorAttachmentView(struct RIDevice *device,
									   RISharedPointer<RITexture> *tex,
									   enum RI_Format_e format,
									   RISharedPointer<RITextureView> *view) {
	RITextureViewDesc vd = {};
	vd.viewType = RI_VIEWTYPE_COLOR_ATTACHMENT;
	vd.format = format;
	vd.mipNum = 1;
	vd.layerNum = 1;
	RITextureView av = RITextureView::create(device, tex->Get(), vd);
	*view = RISharedPointer<RITextureView>(device, av);
	return !av.isEmpty();
}

void ReleaseViewportAttachmentTexture(RISharedPointer<RITexture> *tex,
									  RISharedPointer<RITextureView> *view) {
	cGraphics* pGraphics = Interface<cGraphics>::Get();
	if (!view->isEmpty()) {
		pGraphics->graphicsDefer.push(*view);
	}
	if (!tex->isEmpty()) {
		pGraphics->graphicsDefer.push(*tex);
	}
	*view = {};
	*tex = {};
}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// POGO BUFFER
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	RI_PogoBuffer* cViewport::PreparePogoBuffer(cGraphics::FrameContext *cntx)
	{
		const cVector2l vSize = GetTargetSize();
		if(vSize.x <= 0 || vSize.y <= 0)
			return nullptr;
		const uint32_t alWidth = (uint32_t)vSize.x;
		const uint32_t alHeight = (uint32_t)vSize.y;
		if(mlPogoWidth == alWidth && mlPogoHeight == alHeight &&
			PogoBuffer() != nullptr)
		{
			return &mPogoBuffer;
		}

		// Resize: hand the old halves to the frame freelist (the in-flight
		// pipeline still reads them) and recreate at the new extent — no
		// stall.
		ReleasePogoBuffer(cntx);

		RI_PogoBufferInit(&Interface<cGraphics>::Get()->device, &mPogoBuffer, alWidth, alHeight,
		                  cGraphics::PogoColorFormat);
		mlPogoWidth = alWidth;
		mlPogoHeight = alHeight;
		return &mPogoBuffer;
	}

	void cViewport::ReleasePogoBuffer(cGraphics::FrameContext *cntx)
	{
		cGraphics* pGraphics = Interface<cGraphics>::Get();
		if(mlPogoWidth == 0) return;

		for(size_t p = 0; p < 2; p++)
		{
			if(!mPogoBuffer.pogoView[p].isEmpty())
			{
				pGraphics->graphicsDefer.push(mPogoBuffer.pogoView[p]);
			}
			if(!mPogoBuffer.attachmentView[p].isEmpty())
			{
				pGraphics->graphicsDefer.push(mPogoBuffer.attachmentView[p]);
			}
			if(!mPogoBuffer.textures[p].isEmpty())
			{
				pGraphics->graphicsDefer.push(mPogoBuffer.textures[p]);
			}
			mPogoBuffer.pogoView[p] = {};
			mPogoBuffer.attachmentView[p] = {};
			mPogoBuffer.textures[p] = {};
		}
		mPogoBuffer.attachmentIndex = 0;
		mlPogoWidth = 0;
		mlPogoHeight = 0;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// EVALUATE (world draw -> feed -> post -> delivery)
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	namespace {

	// Single-subresource color barrier with the texture and transition filled
	// in — the feed/delivery path always touches exactly mip 0 / layer 0.
	RITextureBarrier ColorBarrier(RITexture *apTexture,
									enum RIResourceState_e aBefore, uint32_t aBeforeStages,
									enum RIResourceState_e aAfter, uint32_t aAfterStages)
	{
		RITextureBarrier barrier = {};
		barrier.texture = apTexture;
		barrier.before = aBefore;
		barrier.beforeStages = aBeforeStages;
		barrier.after = aAfter;
		barrier.afterStages = aAfterStages;
		barrier.mipCount = 1;
		barrier.layerCount = 1;
		return barrier;
	}

	// Fullscreen draw of the pogo READ half into a color-attachable view —
	// the single delivery primitive (TargetView panes and the swapchain tail
	// only differ in view/extent/format/pipeline-cache salt). The caller owns
	// the view's layout (must be COLOR_ATTACHMENT_OPTIMAL around the draw).
	void DrawPogoToTarget(RI_PogoBuffer *apPogo, const RITextureView &aView,
						  uint32_t alWidth, uint32_t alHeight, enum RI_Format_e aFormat,
						  uint32_t alHashSalt, const char *asLabel)
	{
		cGraphics* pGraphics = Interface<cGraphics>::Get();
		RIRenderingAttachment color = {};
		color.view    = aView;
		color.loadOp  = RI_ATTACHMENT_LOAD_OP_DONT_CARE;
		color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

		RIBeginRenderingDesc beginDesc = {};
		beginDesc.renderArea.width  = alWidth;
		beginDesc.renderArea.height = alHeight;
		beginDesc.colorCount = 1;
		beginDesc.colors     = &color;
		pGraphics->primary.cmds[0].vk_d3d12_beginRendering(&pGraphics->device, beginDesc);

		RIViewport viewport = {};
		viewport.y = static_cast<float>(alHeight);
		viewport.width = static_cast<float>(alWidth);
		viewport.height = -static_cast<float>(alHeight);
		viewport.depthMin = 0.0f;
		viewport.depthMax = 1.0f;
		pGraphics->primary.cmds[0].setViewport(&pGraphics->device, viewport);
		RIRect scissor = {};
		scissor.width = alWidth;
		scissor.height = alHeight;
		pGraphics->primary.cmds[0].setScissor(&pGraphics->device, scissor);

		const RIGraphicsPipelineDesc blitDesc = MakePostEffectPipelineDesc(aFormat, false);
		// Only the caller's salt: bindPipeline folds RIHashGraphicsPipelineDesc
		// into the cache key, so the attachment format is already part of it and
		// two targets sharing a salt but differing in format cannot collide.
		const hash_t kHash = hash_u32(HASH_INITIAL_VALUE, alHashSalt);
		pGraphics->postEffectBlit.bindPipeline(&pGraphics->device, &pGraphics->primary.cmds[0], kHash,
		                               asLabel, blitDesc);

		auto samplerDesc = pGraphics->resolve_filter_descriptor(
		    eTextureWrap_ClampToEdge, eTextureWrap_ClampToEdge,
		    eTextureWrap_ClampToEdge, eTextureFilter_Bilinear);
		RIProgram::DescriptorBinding bindings[2] = {};
		bindings[0].descriptor = *samplerDesc;
		bindings[0].handle     = DescriptorBindingID::Create("inputSampler");
		bindings[1].descriptor = RI_PogoBufferShaderResource(apPogo);
		bindings[1].handle     = DescriptorBindingID::Create("sourceInput");
		pGraphics->postEffectBlit.bindDescriptors(&pGraphics->device, &pGraphics->primary.cmds[0], pGraphics->frameIndex, bindings, 2);

		// Dither only the final normalized display target, never the floating
		// point pogo buffers or image-trail history. sRGB attachments perform
		// their own nonlinear encoding and need a different noise scale.
		float quantizationStep[4] = {};
		const RIFormatProps* props = GetRIFormatProps(aFormat);
		if (props->isNorm && !props->isFloat && !props->isSigned && !props->isSrgb)
		{
			const uint8_t bits[3] = {props->redBits, props->greenBits, props->blueBits};
			for (int channel = 0; channel < 3; ++channel)
				if (bits[channel] > 0 && bits[channel] < 32)
					quantizationStep[channel] = 1.0f / float((1u << bits[channel]) - 1u);
		}
		pGraphics->primary.cmds[0].vk_d3d12_setPushConstants(&pGraphics->device,
			pGraphics->postEffectBlit, 0, sizeof(quantizationStep), quantizationStep);

		pGraphics->primary.cmds[0].draw(&pGraphics->device, 3, 1, 0, 0);
		pGraphics->primary.cmds[0].vk_d3d12_endRendering(&pGraphics->device);
	}

	} // namespace

	//-----------------------------------------------------------------------

	bool cViewport::ClaimProviderRenderExtent(cVector2l aRenderExtent)
	{
		const TemporalRenderExtentState current = {
				ToPolicyOwner(mRenderExtentOwner), ToTemporalExtent(mRenderExtent)};
		const TemporalRenderExtentDecision decision =
				ClaimTemporalProviderRenderExtent(current,
						ToTemporalExtent(aRenderExtent));
		mRenderExtentOwner = FromPolicyOwner(decision.state.owner);
		SetRenderExtent(FromTemporalExtent(decision.state.extent));
		if (decision.historyReset)
			mTemporalHistoryReset = true;
		return decision.ownershipChanged || decision.historyReset;
	}

	void cViewport::ReleaseProviderRenderExtent()
	{
		if (mRenderExtentOwner != eRenderExtentOwner::Provider)
			return;

		const TemporalRenderExtentState current = {
				ToPolicyOwner(mRenderExtentOwner), ToTemporalExtent(mRenderExtent)};
		const TemporalRenderExtentDecision decision =
				ReleaseTemporalProviderRenderExtent(current);
		mRenderExtentOwner = FromPolicyOwner(decision.state.owner);
		SetRenderExtent(FromTemporalExtent(decision.state.extent));
		if (decision.historyReset)
			mTemporalHistoryReset = true;
		mbDevRenderScaleIgnoredLogged = false;
	}

	void cViewport::ReleaseTemporalProvider()
	{
		cGraphics *pGraphics = Interface<cGraphics>::Get();
		if (mpTemporalUpscalerProvider) {
			std::shared_ptr<iTemporalUpscaler> keepProvider =
					std::move(mpTemporalUpscalerProvider);
			if (pGraphics) {
				pGraphics->graphicsDefer.push(std::function<void()>(
						[keepProvider = std::move(keepProvider)]() mutable {
							keepProvider.reset();
						}));
			}
		}

		mbTemporalProviderPrepared = false;
		mlTemporalJitterPhaseCount = 0;
		mTemporalProviderPreparedSettings = {};
		mTemporalProviderPreparedRenderExtent = {};
		mTemporalProviderPreparedDisplayExtent = {};
		mTemporalUpscalerSettings = {};
		mTemporalUpscalerStatus.effectiveProvider =
				TemporalUpscalerProvider::Off;
		mTemporalUpscalerStatus.effectiveQuality =
				TemporalUpscalerQuality::Quality;
		ReleaseProviderRenderExtent();
	}

	void cViewport::ClearTemporalProviderFailure()
	{
		::hpl::ClearTemporalProviderFailure(mTemporalProviderFailure);
		mpTemporalProviderFailureReason = nullptr;
		mTemporalUpscalerStatus.unavailableReason = nullptr;
	}

	bool cViewport::PrepareTemporalProvider(cGraphics::FrameContext *cntx,
			bool worldWillRender)
	{
		const bool hadPreparedProvider = mbTemporalProviderPrepared;
		const TemporalUpscalerSettings previousPreparedSettings =
				mTemporalProviderPreparedSettings;
		const TemporalUpscalerExtent previousPreparedDisplayExtent =
				mTemporalProviderPreparedDisplayExtent;
		mbTemporalProviderPrepared = false;
		mlTemporalJitterPhaseCount = 0;
		mTemporalUpscalerSettings = {};

		const TemporalUpscalerSettings desired =
				GetRenderSettings() ? GetRenderSettings()->mTemporalUpscaler
														: TemporalUpscalerSettings{};
		mTemporalUpscalerRequestedSettings = desired;
		mTemporalUpscalerStatus = {};
		mTemporalUpscalerStatus.requestedProvider = desired.provider;
		mTemporalUpscalerStatus.requestedQuality = desired.quality;
		mTemporalUpscalerStatus.effectiveProvider =
				TemporalUpscalerProvider::Off;
		mTemporalUpscalerStatus.effectiveQuality =
				TemporalUpscalerQuality::Quality;
		mTemporalUpscalerStatus.available =
				desired.provider == TemporalUpscalerProvider::Off;



		const cVector2l displaySize = GetDisplayExtent();
		const bool displayValid = displaySize.x > 0 && displaySize.y > 0;
		const TemporalProviderFailureKey failureKey = {
				desired.provider,
				desired.quality,
				ToTemporalExtent(displaySize)};
		if (mTemporalProviderFailure.latched &&
				!TemporalProviderFailureKeyMatches(
						mTemporalProviderFailure.key, failureKey))
			ClearTemporalProviderFailure();

		if (desired.provider == TemporalUpscalerProvider::Off ||
				!worldWillRender) {
			ReleaseTemporalProvider();
			return false;
		}

		if (!displayValid) {
			ReleaseTemporalProvider();
			return false;
		}
		const TemporalUpscalerExtent displayExtent = {
				static_cast<uint32_t>(displaySize.x),
				static_cast<uint32_t>(displaySize.y)};

		if (TemporalProviderFailureSuppressed(mTemporalProviderFailure,
				failureKey)) {
			mTemporalUpscalerStatus.available = false;
			mTemporalUpscalerStatus.unavailableReason =
					mpTemporalProviderFailureReason;
			ReleaseTemporalProvider();
			return false;
		}

		auto latchFailure = [&](const char *reason) {
			LatchTemporalProviderFailure(
					mTemporalProviderFailure,
					{desired.provider, desired.quality, displayExtent});
			mpTemporalProviderFailureReason =
					reason ? reason : kTemporalProviderUnknownFailure;
			TemporalHistoryResetTriggers resetTriggers;
			resetTriggers.providerFailure = true;
			mTemporalHistoryReset |=
					TemporalHistoryResetRequired(resetTriggers);
			mTemporalUpscalerStatus.effectiveProvider =
					TemporalUpscalerProvider::Off;
			mTemporalUpscalerStatus.effectiveQuality =
					TemporalUpscalerQuality::Quality;
			mTemporalUpscalerStatus.available = false;
			mTemporalUpscalerStatus.unavailableReason =
					mpTemporalProviderFailureReason;
		};

		const TemporalUpscalerStatus queried =
				TemporalUpscalerQuery(desired);
		mTemporalUpscalerStatus = queried;
		if (!queried.available) {
			const char *reason = queried.unavailableReason
														? queried.unavailableReason
														: kTemporalProviderUnknownFailure;
			latchFailure(reason);
			if (!mpTemporalProviderUnavailableReasonLogged ||
					std::strcmp(mpTemporalProviderUnavailableReasonLogged, reason) != 0) {
				Warning("cViewport: temporal upscaler unavailable: %s\n", reason);
				mpTemporalProviderUnavailableReasonLogged = reason;
			}
			ReleaseTemporalProvider();
			return false;
		}

		const TemporalUpscalerSettings effective = {
				queried.effectiveProvider, queried.effectiveQuality};
		if (hadPreparedProvider &&
				(previousPreparedSettings.provider != effective.provider ||
				 previousPreparedSettings.quality != effective.quality ||
				 previousPreparedDisplayExtent.width != displayExtent.width ||
				 previousPreparedDisplayExtent.height != displayExtent.height)) {
			TemporalHistoryResetTriggers resetTriggers;
			resetTriggers.providerChanged =
					previousPreparedSettings.provider != effective.provider;
			resetTriggers.qualityChanged =
					previousPreparedSettings.quality != effective.quality;
			resetTriggers.outputExtentChanged =
					previousPreparedDisplayExtent.width != displayExtent.width ||
					previousPreparedDisplayExtent.height != displayExtent.height;
			mTemporalHistoryReset |=
					TemporalHistoryResetRequired(resetTriggers);
		}
		if (mpTemporalUpscalerProvider &&
				mTemporalProviderPreparedSettings.provider !=
						effective.provider) {
			ReleaseTemporalProvider();
		}

		if (!mpTemporalUpscalerProvider) {
			std::unique_ptr<iTemporalUpscaler> created =
					TemporalUpscalerCreate(effective.provider,
														Interface<cGraphics>::Get());
			mpTemporalUpscalerProvider = std::move(created);
			if (!mpTemporalUpscalerProvider) {
				latchFailure(kTemporalProviderCreationFailed);
				ReleaseTemporalProvider();
				Warning("cViewport: temporal upscaler preparation failed: %s\n",
						kTemporalProviderCreationFailed);
				return false;
			}
		}

		const TemporalUpscalerExtent renderExtent =
				effective.quality == TemporalUpscalerQuality::NativeAA
						? displayExtent
						: mpTemporalUpscalerProvider->GetRecommendedRenderExtent(
								displayExtent, effective.quality);
		if (!TemporalUpscalerNegotiatedExtentValid(
					renderExtent, displayExtent, effective.quality)) {
			latchFailure(kTemporalProviderInvalidRenderExtent);
			ReleaseTemporalProvider();
			Warning("cViewport: temporal upscaler preparation failed: %s\n",
					kTemporalProviderInvalidRenderExtent);
			return false;
		}

		if (!mpTemporalUpscalerProvider->PrepareContext(
					effective, renderExtent, displayExtent, cntx)) {
			latchFailure(kTemporalProviderPreparationFailed);
			ReleaseTemporalProvider();
			Warning("cViewport: temporal upscaler preparation failed: %s\n",
					kTemporalProviderPreparationFailed);
			return false;
		}

		mlTemporalJitterPhaseCount =
				mpTemporalUpscalerProvider->GetJitterPhaseCount(renderExtent,
						displayExtent);
		if (mlTemporalJitterPhaseCount == 0)
			mlTemporalJitterPhaseCount = 1;
		mbTemporalProviderPrepared = true;
		mTemporalProviderPreparedSettings = effective;
		mTemporalProviderPreparedRenderExtent = renderExtent;
		mTemporalProviderPreparedDisplayExtent = displayExtent;
		mTemporalUpscalerSettings = effective;
		mTemporalUpscalerStatus = queried;
		mTemporalUpscalerStatus.available = true;
		mTemporalUpscalerStatus.unavailableReason = nullptr;
		::hpl::ClearTemporalProviderFailure(mTemporalProviderFailure);
		mpTemporalProviderFailureReason = nullptr;
		ClaimProviderRenderExtent(
				cVector2l(static_cast<int>(renderExtent.width),
							static_cast<int>(renderExtent.height)));
		return true;
	}

	//-----------------------------------------------------------------------

	bool cViewport::Evaluate(cGraphics::FrameContext *cntx, float afFrameTime, tFlag alFlags)
	{
		cGraphics* pGraphics = Interface<cGraphics>::Get();
		// Once per frame: the renderers' initial UNDEFINED backbuffer barriers
		// have an empty before-scope, so a second draw of the same per-
		// swapchain-image backbuffer would race this evaluation's feed blit.
		if(mlLastEvaluatedFrame == pGraphics->frameIndex)
		{
			assert(false && "cViewport::Evaluate called twice in one frame");
			return false;
		}
		mlLastEvaluatedFrame = pGraphics->frameIndex;
		// Display depth is exposed only after a successful presentation resolve
		// in this evaluation. This also rejects a provider failure that occurred
		// after the module had already prepared its display-depth resource.
		mTemporalPresentationDepthValid = false;
		mTemporalReactiveMaskActive = false;

		// A provider failure is latched by the presentation module for the next
		// frame. Revert to native input and carry the reset into the renderer's
		// single TemporalBeginFrame snapshot.
		if (mpTemporalPresentation &&
				mpTemporalPresentation->ConsumeProviderFailure()) {
			const TemporalRenderExtentState current = {
					ToPolicyOwner(mRenderExtentOwner), ToTemporalExtent(mRenderExtent)};
			const TemporalRenderExtentDecision decision =
					ConsumeTemporalProviderFailure(current);
			mRenderExtentOwner = FromPolicyOwner(decision.state.owner);
			SetRenderExtent(FromTemporalExtent(decision.state.extent));
			if (decision.historyReset)
				mTemporalHistoryReset = true;
			mbDevRenderScaleIgnoredLogged = false;
		}

		cFrustum* pFrustum = mpCamera ? mpCamera->GetFrustum() : NULL;

		const bool worldRendered =
				(alFlags & tSceneRenderFlag_World) &&
				mpRenderer && mpWorld && pFrustum;

		// This must claim the negotiated input extent before the development
		// render-scale override and before Draw allocates scene targets.
		PrepareTemporalProvider(cntx, worldRendered);

		const float fDevScale = pGraphics ? pGraphics->devRenderScale : 1.0f;
		const TemporalRenderExtentResolution extentResolution = {
				mbTemporalProviderPrepared,
				mTemporalProviderPreparedRenderExtent,
				fDevScale,
				ToTemporalExtent(GetDisplayExtent()),
				{ToPolicyOwner(mRenderExtentOwner), ToTemporalExtent(mRenderExtent)}};
		const TemporalRenderExtentDecision extentDecision =
				ResolveTemporalRenderExtent(extentResolution);
		mRenderExtentOwner = FromPolicyOwner(extentDecision.state.owner);
		SetRenderExtent(FromTemporalExtent(extentDecision.state.extent));
		if (extentDecision.historyReset)
			mTemporalHistoryReset = true;
		if (extentDecision.devRenderScaleIgnored &&
				!mbDevRenderScaleIgnoredLogged) {
				Warning("cViewport: development render scale %.3f ignored because "
						"the temporal upscaler owns the render extent\n",
						fDevScale);
				mbDevRenderScaleIgnoredLogged = true;
			}

		if (!worldRendered && mpTemporalReactiveMask) {
			TemporalReactiveMaskFrameDesc disabledMaskFrame = {};
			disabledMaskFrame.frameContext = cntx;
			disabledMaskFrame.frameIndex = pGraphics->frameIndex;
			disabledMaskFrame.viewportCookie = this;
			mpTemporalReactiveMask->BeginFrame(disabledMaskFrame);
		}

		if(alFlags & tSceneRenderFlag_World)
		{
			const cVector2l vPreSize = GetTargetSize();
			WorldDrawCtx preCtx{};
			preCtx.viewport   = this;
			preCtx.cmd        = &pGraphics->primary.cmds[0];
			preCtx.device     = &pGraphics->device;
			preCtx.frame      = cntx;
			preCtx.frustum    = pFrustum;
			preCtx.width      = vPreSize.x > 0 ? (uint32_t)vPreSize.x : 0;
			preCtx.height     = vPreSize.y > 0 ? (uint32_t)vPreSize.y : 0;
			preCtx.frameIndex = pGraphics->frameIndex;
			preCtx.frameTime  = afFrameTime;
			m_onPreWorldDraw.Signal(preCtx);
			if (worldRendered) {
				// Publish the world's per-frame GPU state (TLAS, bindless object
				// slots, light/fog/decal buffers) before drawing — self-guarded
				// once per frame, so this no-ops for cScene-driven viewports (the
				// scene prepared already) and covers headless evaluates that run
				// before cScene::Render (editor camera capture, thumbnails), whose
				// RT passes would otherwise trace last frame's TLAS against this
				// frame's rewritten object slots.
				mpWorld->PrepareFrame(cntx);

				// Gate every mask allocation and dispatch on an active provider. The
				// Off/native path must incur no opaque copy, no dispatch, and no
				// retained targets, so BeginFrame releases whatever it owns and
				// returns false.
				mTemporalReactiveMaskActive = false;
				const cVector2l maskExtent = GetRenderExtent();
				if (mTemporalUpscalerSettings.provider !=
						TemporalUpscalerProvider::Off) {
					if (!mpTemporalReactiveMask) {
						cResources *pResources = Interface<cResources>::Get();
						if (pResources)
							mpTemporalReactiveMask =
								std::make_unique<cTemporalReactiveMask>(
										pResources->GetFileSearcher());
					}
				}
				if (mpTemporalReactiveMask) {
					TemporalReactiveMaskFrameDesc maskFrame = {};
					maskFrame.frameContext = cntx;
					maskFrame.frameIndex = pGraphics->frameIndex;
					maskFrame.viewportCookie = this;
					maskFrame.renderExtent = {
						maskExtent.x > 0 ? static_cast<uint32_t>(maskExtent.x) : 0u,
						maskExtent.y > 0 ? static_cast<uint32_t>(maskExtent.y) : 0u};
					maskFrame.enabled =
						mTemporalUpscalerSettings.provider !=
						TemporalUpscalerProvider::Off;
					maskFrame.needUnjitteredVariant =
						mTemporalUpscalerSettings.provider ==
						TemporalUpscalerProvider::XeSS;
					mTemporalReactiveMaskActive =
						mpTemporalReactiveMask->BeginFrame(maskFrame);
				}

				mpRenderer->Draw(
						cntx,
						this,
						afFrameTime,
						pFrustum,
						mpWorld,
						GetRenderSettings(),
						false);

				// PRE-FEED HDR hook: the renderer left the BackBuffer holding
				// the linear-HDR scene (SHADER_READ) and depth in
				// DEPTH_ATTACHMENT_OPTIMAL. Handlers draw additive geometry
				// here (pickup flash / enemy glow) so the still-to-run feed
				// blit + post chain (bloom + tonemap) process it — the pogo
				// does not exist yet. Handlers must return the BackBuffer to
				// SHADER_READ for the feed blit below.
				const cVector2l vRenderSize = GetRenderExtent();
				BackBuffer postTransBackBuffer = GetBackBuffer();
				if (vRenderSize.x > 0 && vRenderSize.y > 0 &&
					!postTransBackBuffer.renderTarget.isEmpty() &&
					!postTransBackBuffer.renderTargetView.isEmpty() &&
					GetRenderDepthView() != nullptr &&
					GetRenderDepthTexture() != nullptr) {
					PostTranslucenceDrawCtx transCtx{};
					transCtx.viewport   = this;
					transCtx.cmd        = &pGraphics->primary.cmds[0];
					transCtx.device     = &pGraphics->device;
					transCtx.frame      = cntx;
					transCtx.frustum    = pFrustum;
					transCtx.width      = (uint32_t)vRenderSize.x;
					transCtx.height     = (uint32_t)vRenderSize.y;
					transCtx.frameIndex = pGraphics->frameIndex;
					transCtx.frameTime  = afFrameTime;
					transCtx.depthView  = GetRenderDepthView(); // pogo not created yet

					// Hand additive geometry the exact raster camera used by Draw. The
					// frame check prevents a previous viewport/frame publication from
					// leaking into a failed or world-less draw.
					const RasterCamera &rasterCamera = GetRasterCamera();
					const bool rasterCameraCurrent =
						rasterCamera.valid &&
						rasterCamera.frameIndex == pGraphics->frameIndex;
					if (rasterCameraCurrent) {
						std::memcpy(transCtx.viewMat, rasterCamera.viewMat,
									sizeof(transCtx.viewMat));
						std::memcpy(transCtx.projMat, rasterCamera.projMat,
									sizeof(transCtx.projMat));
					} else {
						const ml::float4x4 frustumView = pFrustum->GetViewMat();
						const ml::float4x4 frustumProjection =
								pFrustum->GetProjectionMat();
						std::memcpy(transCtx.viewMat, frustumView.a,
									sizeof(transCtx.viewMat));
						std::memcpy(transCtx.projMat, frustumProjection.a,
									sizeof(transCtx.projMat));
					}
					m_onPostTranslucenceDraw.Signal(transCtx);

					if (mTemporalReactiveMaskActive && mpTemporalReactiveMask) {
						TemporalReactiveMaskRecordDesc maskRecord = {};
						maskRecord.cmd = &pGraphics->primary.cmds[0];
						maskRecord.finalColor =
							&postTransBackBuffer.renderTarget;
						maskRecord.finalColorView =
							&postTransBackBuffer.renderTargetView;
						maskRecord.finalColorFormat = cGraphics::PogoColorFormat;
						maskRecord.extent = {
							vRenderSize.x > 0 ? static_cast<uint32_t>(vRenderSize.x)
													: 0u,
							vRenderSize.y > 0 ? static_cast<uint32_t>(vRenderSize.y)
													: 0u};
						maskRecord.frameIndex = pGraphics->frameIndex;
						maskRecord.viewportCookie = this;
						maskRecord.finalColorEntryState =
							RI_RESOURCE_STATE_SHADER_RESOURCE;
						maskRecord.finalColorExitState =
							RI_RESOURCE_STATE_SHADER_RESOURCE;
						const RasterCamera &maskRasterCamera = GetRasterCamera();
						if (maskRasterCamera.valid &&
							maskRasterCamera.frameIndex == pGraphics->frameIndex) {
							maskRecord.jitterPixels[0] =
								maskRasterCamera.jitterPixels[0];
							maskRecord.jitterPixels[1] =
								maskRasterCamera.jitterPixels[1];
						}
						maskRecord.provider = mTemporalUpscalerSettings.provider;
						// RecordMasks restores the scene color to the exit state,
						// exactly what the feed blit below expects.
						mpTemporalReactiveMask->RecordMasks(maskRecord);
					}
				}
			}
		}

		// FEED + POST: once the world draw fully evaluated the viewport,
		// blit the backend's BackBuffer window (currently the whole negotiated
		// scene/input image; crop fields are reserved for a future guard band)
		// into the viewport pogo READ half, prep the ATTACH half, and run the
		// post-effect chain on the pogo (each effect samples the read
		// half, renders the attach half, and toggles). Delivery happens
		// after, per the viewport's Target.
		BackBuffer backBuffer = GetBackBuffer();
		TemporalPresentationResult presentationResult = {};
		if (worldRendered && !backBuffer.renderTarget.isEmpty() &&
			backBuffer.width != 0 && backBuffer.height != 0) {
			const cVector2l renderSize = GetRenderExtent();
			const cVector2l displaySize = GetDisplayExtent();
			if (renderSize.x > 0 && renderSize.y > 0 && displaySize.x > 0 &&
				displaySize.y > 0) {
				if (!mpTemporalPresentation) {
					cResources *pResources = Interface<cResources>::Get();
					if (pResources)
						mpTemporalPresentation =
							std::make_unique<cTemporalPresentation>(
								pResources->GetFileSearcher());
				}

				if (mpTemporalPresentation) {
					TemporalPresentationFrameInput presentationInput = {};
					presentationInput.frameContext = cntx;
					presentationInput.cmd = &pGraphics->primary.cmds[0];
					presentationInput.renderExtent = {
						static_cast<uint32_t>(renderSize.x),
						static_cast<uint32_t>(renderSize.y)};
					presentationInput.displayExtent = {
						static_cast<uint32_t>(displaySize.x),
						static_cast<uint32_t>(displaySize.y)};

					// The scene image and valid rectangle are both INPUT-space;
					// never substitute the display extent for this rectangle.
					presentationInput.scene.hdrColor.texture =
						&backBuffer.renderTarget;
					presentationInput.scene.hdrColor.view =
						&backBuffer.renderTargetView;
					presentationInput.scene.hdrColor.format =
						cGraphics::PogoColorFormat;
					presentationInput.scene.hdrColor.extent =
						presentationInput.renderExtent;
					presentationInput.scene.hdrColor.entryState =
						RI_RESOURCE_STATE_SHADER_RESOURCE;
					presentationInput.scene.hdrColor.exitState =
						RI_RESOURCE_STATE_SHADER_RESOURCE;
					presentationInput.scene.hdrColor.entryStage = RI_STAGE_FRAGMENT;
					presentationInput.scene.hdrColor.exitStage = RI_STAGE_FRAGMENT;
					presentationInput.scene.hdrValidRect = {
						backBuffer.x, backBuffer.y, backBuffer.width,
						backBuffer.height};

					presentationInput.scene.renderDepthTexture =
						GetRenderDepthTexture();
					presentationInput.scene.renderDepthAttachmentView =
						GetRenderDepthView();
					presentationInput.scene.renderDepthSampleView =
						GetRenderDepthSampleView();

					presentationInput.provider =
							mbTemporalProviderPrepared
									? mpTemporalUpscalerProvider.get()
									: nullptr;
					presentationInput.settings = mTemporalUpscalerSettings;

					// Keep optional motion-vector metadata tied to this render image
					// for the future activation path.
					// Depth entry/exit states depend on how each renderer variant leaves the depth.
					std::visit(
						[&](auto &&arg) {
							using T = std::decay_t<decltype(arg)>;
							if constexpr (std::is_same_v<T, HybridViewportState>) {
								presentationInput.scene.renderDepthEntryState =
									RI_RESOURCE_STATE_DEPTH_WRITE;
								presentationInput.scene.renderDepthExitState =
									RI_RESOURCE_STATE_DEPTH_WRITE;
								const uint32_t imageIndex = pGraphics->swapchainIndex;
								if (!arg.velocityTexture[imageIndex].isEmpty() &&
									!arg.velocityView[imageIndex].isEmpty()) {
									presentationInput.scene.motionVectors.texture =
										arg.velocityTexture[imageIndex].Get();
									presentationInput.scene.motionVectors.view =
										arg.velocityView[imageIndex].Get();
									presentationInput.scene.motionVectors.format =
										cGraphics::VelocityFormat;
									presentationInput.scene.motionVectors.extent =
										presentationInput.renderExtent;
									presentationInput.scene.motionVectors.entryState =
										RI_RESOURCE_STATE_SHADER_RESOURCE;
									presentationInput.scene.motionVectors.exitState =
										RI_RESOURCE_STATE_SHADER_RESOURCE;
								}
						} else if constexpr (std::is_same_v<T, StandardViewportState>) {
								// cStandardRenderer::Draw hands depth back in
								// DEPTH_WRITE, like Hybrid.
								presentationInput.scene.renderDepthEntryState =
									RI_RESOURCE_STATE_DEPTH_WRITE;
								presentationInput.scene.renderDepthExitState =
									RI_RESOURCE_STATE_DEPTH_WRITE;
								const uint32_t imageIndex = pGraphics->swapchainIndex;
								if (!arg.velocityTexture[imageIndex].isEmpty() &&
									!arg.velocityView[imageIndex].isEmpty()) {
									presentationInput.scene.motionVectors.texture =
										arg.velocityTexture[imageIndex].Get();
									presentationInput.scene.motionVectors.view =
										arg.velocityView[imageIndex].Get();
									presentationInput.scene.motionVectors.format =
										cGraphics::VelocityFormat;
									presentationInput.scene.motionVectors.extent =
										presentationInput.renderExtent;
									presentationInput.scene.motionVectors.entryState =
										RI_RESOURCE_STATE_SHADER_RESOURCE;
									presentationInput.scene.motionVectors.exitState =
										RI_RESOURCE_STATE_SHADER_RESOURCE;
								}
							}
						},
						m_state);

					// Explicit, caller-produced masks. GetBindings is frame- and
					// and the provider falls back to its own neutral default; an old
					// frame's or another viewport's resource can never be passed.
					if (mTemporalReactiveMaskActive && mpTemporalReactiveMask) {
						const TemporalReactiveMaskBindings maskBindings =
							mpTemporalReactiveMask->GetBindings(
									pGraphics->frameIndex,
									presentationInput.renderExtent, this);
						if (maskBindings.valid) {
							presentationInput.scene.opaqueColor =
								maskBindings.opaqueColor;
							presentationInput.scene.reactiveMaskJittered =
								maskBindings.reactiveMaskJittered;
							presentationInput.scene.compositionMaskJittered =
								maskBindings.compositionMaskJittered;
							presentationInput.scene.responsiveMaskUnjittered =
								maskBindings.responsiveMaskUnjittered;
						}
					}

					const RasterCamera &rasterCamera = GetRasterCamera();
					const bool rasterCameraCurrent =
						rasterCamera.valid &&
						rasterCamera.frameIndex == pGraphics->frameIndex;
					if (rasterCameraCurrent) {
						std::memcpy(presentationInput.viewMat,
								rasterCamera.viewMat,
								sizeof(presentationInput.viewMat));
						std::memcpy(presentationInput.unjitteredProjMat,
								rasterCamera.unjitteredProjMat,
								sizeof(presentationInput.unjitteredProjMat));
						std::memcpy(presentationInput.previousViewMat,
								rasterCamera.previousViewMat,
								sizeof(presentationInput.previousViewMat));
						std::memcpy(
							presentationInput.previousUnjitteredProjMat,
							rasterCamera.previousUnjitteredProjMat,
							sizeof(presentationInput.previousUnjitteredProjMat));
						presentationInput.jitterPixels[0] =
							rasterCamera.jitterPixels[0];
						presentationInput.jitterPixels[1] =
							rasterCamera.jitterPixels[1];
						presentationInput.previousJitterPixels[0] =
							rasterCamera.previousJitterPixels[0];
						presentationInput.previousJitterPixels[1] =
							rasterCamera.previousJitterPixels[1];
						presentationInput.deltaTimeMs = rasterCamera.deltaTimeMs;
						presentationInput.resetHistory = rasterCamera.historyReset;
					} else {
						// Draw should have published this snapshot. Keep a complete
						// camera fallback if preparation failed.
						const ml::float4x4 frustumView = pFrustum->GetViewMat();
						const ml::float4x4 frustumProjection =
							pFrustum->GetProjectionMat();
						std::memcpy(presentationInput.viewMat, frustumView.a,
							sizeof(presentationInput.viewMat));
						std::memcpy(presentationInput.unjitteredProjMat,
							frustumProjection.a,
							sizeof(presentationInput.unjitteredProjMat));
						std::memcpy(presentationInput.previousViewMat,
							frustumView.a,
							sizeof(presentationInput.previousViewMat));
						std::memcpy(
							presentationInput.previousUnjitteredProjMat,
							frustumProjection.a,
							sizeof(presentationInput.previousUnjitteredProjMat));
						presentationInput.deltaTimeMs = afFrameTime * 1000.0f;
						presentationInput.resetHistory = true;
					}
					presentationInput.zNear = pFrustum->GetNearPlane();
					presentationInput.zFar = pFrustum->GetFarPlane();
					presentationInput.verticalFovRadians = pFrustum->GetFOV();
					presentationInput.frameIndex = pGraphics->frameIndex;
					presentationResult = mpTemporalPresentation->Resolve(
						presentationInput);
				}
			}
		}

		RI_PogoBuffer *pPogo = nullptr;
		const cVector2l displaySize = GetDisplayExtent();
		if(worldRendered && !backBuffer.renderTarget.isEmpty() &&
			backBuffer.width != 0 && backBuffer.height != 0 &&
			displaySize.x > 0 && displaySize.y > 0)
		{
			pPogo = PreparePogoBuffer(cntx);
			if (pPogo == nullptr || pPogo->textures[0].isEmpty() ||
				pPogo->textures[1].isEmpty() || pPogo->pogoView[0].isEmpty() ||
				pPogo->pogoView[1].isEmpty() || pPogo->attachmentView[0].isEmpty() ||
				pPogo->attachmentView[1].isEmpty()) {
				pPogo = nullptr;
			} else {
			// Only expose the display attachment after the presentation resolve
			// and the feed target both prepared successfully. A failed pogo
			// preparation must use the render-depth pair as well.
				// A spatial fallback is a defined current-frame image, never a stale
				// last-successful SDK frame. providerFailed remains latched so the
				// next frame still consumes the native-extent reset.
				mTemporalPresentationDepthValid =
					presentationResult.displayDepthProduced &&
					(!presentationResult.providerFailed ||
					 presentationResult.colorIsSpatialFallback);

			const uint32_t readIdx = (pPogo->attachmentIndex + 1u) % 2u;
			const uint32_t displayWidth = static_cast<uint32_t>(displaySize.x);
			const uint32_t displayHeight = static_cast<uint32_t>(displaySize.y);
			const bool providerColor =
				presentationResult.colorProduced &&
				(!presentationResult.providerFailed ||
				 presentationResult.colorIsSpatialFallback) &&
				presentationResult.color.texture != nullptr &&
				presentationResult.color.view != nullptr &&
				presentationResult.color.IsValid() &&
				presentationResult.color.extent.width == displayWidth &&
				presentationResult.color.extent.height == displayHeight;
			bool feedRecorded = false;

			if (providerColor) {
				// Provider output is a display-sized linear RGBA16F image. Only
				// consume it when the provider explicitly proved it wrote this
				// module-owned image; the scene BackBuffer remains untouched and
				// its provider exit contract leaves it SHADER_RESOURCE.
				const uint32_t providerExtentWidth =
					presentationResult.color.extent.width;
				const uint32_t providerExtentHeight =
					presentationResult.color.extent.height;
				if (IsViewportFeedCropValid(0, 0, displayWidth, displayHeight,
						providerExtentWidth, providerExtentHeight)) {
					const RITextureBarrier providerAttachment = RI_PogoAttachmentBarrier(
						pPogo->textures[pPogo->attachmentIndex].Get(), true);
					pGraphics->primary.cmds[0].vk_d3d12_textureBarriers<1>(1,
						&providerAttachment);
					feedRecorded = RecordViewportFeed(mpFeedProgram, &pGraphics->primary.cmds[0],
					&pGraphics->device, pGraphics->frameIndex,
					presentationResult.color.texture,
					presentationResult.color.view,
					presentationResult.colorState,
					presentationResult.colorEntryStage,
					presentationResult.colorState,
					presentationResult.colorExitStage,
					pPogo->textures[readIdx].Get(), pPogo->attachmentView[readIdx].Get(),
								0, 0, displayWidth, displayHeight,
								providerExtentWidth,
								providerExtentHeight,
								displayWidth, displayHeight);
				}
			} else {
				// Explicit spatial fallback: copy the valid INPUT rectangle and
				// honestly upscale it to the full DISPLAY extent. This is also the
				// only path allowed to consume the scene BackBuffer.

			// Pre-blit: BackBuffer (left SHADER_READ by the renderer) ->
			// TRANSFER_SRC, pogo read half -> TRANSFER_DST (UNDEFINED
			// discard — fully overwritten, the fragment-stage hint orders
			// the overwrite after the prior frame's reads), pogo attach
			// half UNDEFINED -> COLOR so the post chain (which renders
			// into the attach half with no barrier of its own) finds it
			// ready.
			const cVector2l sceneRenderExtent = GetRenderExtent();
			const uint32_t sceneExtentWidth = sceneRenderExtent.x > 0
				? static_cast<uint32_t>(sceneRenderExtent.x) : 0u;
			const uint32_t sceneExtentHeight = sceneRenderExtent.y > 0
				? static_cast<uint32_t>(sceneRenderExtent.y) : 0u;
			if (IsViewportFeedCropValid(backBuffer.x, backBuffer.y,
					backBuffer.width, backBuffer.height,
					sceneExtentWidth, sceneExtentHeight)) {
				const RITextureBarrier fallbackAttachment = RI_PogoAttachmentBarrier(
					pPogo->textures[pPogo->attachmentIndex].Get(), true);
				pGraphics->primary.cmds[0].vk_d3d12_textureBarriers<1>(1,
					&fallbackAttachment);
				feedRecorded = RecordViewportFeed(mpFeedProgram, &pGraphics->primary.cmds[0],
				&pGraphics->device, pGraphics->frameIndex,
				&backBuffer.renderTarget, &backBuffer.renderTargetView,
				RI_RESOURCE_STATE_SHADER_RESOURCE,
				RI_STAGE_FRAGMENT,
				RI_RESOURCE_STATE_SHADER_RESOURCE,
				RI_STAGE_FRAGMENT,
				pPogo->textures[readIdx].Get(), pPogo->attachmentView[readIdx].Get(),
				backBuffer.x, backBuffer.y, backBuffer.width, backBuffer.height,
				sceneExtentWidth, sceneExtentHeight,
					displayWidth, displayHeight);
			}
			}

			if (!feedRecorded) {
				pPogo = nullptr;
			} else {
				cPostEffectComposite *pComposite = GetPostEffectComposite();
				if(pComposite && (alFlags & tSceneRenderFlag_PostEffects) &&
				   pComposite->HasActiveEffects())
			{
				pComposite->Render(afFrameTime, &pGraphics->primary.cmds[0], pPogo,
					                   displayWidth, displayHeight,
				                   pGraphics->frameIndex);
			}

			// The pogo read half holds the final (post-processed) image.
			PostWorldDrawCtx postCtx{};
			postCtx.viewport   = this;
			postCtx.cmd        = &pGraphics->primary.cmds[0];
			postCtx.device     = &pGraphics->device;
			postCtx.frame      = cntx;
			postCtx.frustum    = pFrustum;
			postCtx.width      = displayWidth;
			postCtx.height     = displayHeight;
			postCtx.frameIndex = pGraphics->frameIndex;
			postCtx.frameTime  = afFrameTime;
			postCtx.pogo       = pPogo;
			postCtx.depthView  = GetDepthView();
			m_onPostWorldDraw.Signal(postCtx);
			}
			}
		}

		// Delivery — symmetric branch on the viewport's own Target
		// variant. Shared post-delivery context (pPogo is valid in both
		// branches' signal guard; the delivered target itself is reached
		// through ctx.viewport). TargetView (standalone, e.g. editor panes /
		// headless thumbnails): a fullscreen draw of the pogo read half into
		// the caller-provided view when one was set (contract: color-
		// attachable, matching the TargetView's `format`; fully rewritten
		// every frame and left SHADER_READ_ONLY for the consumer — the
		// editor's pane Image samples it). No swapchain composite or GUI for
		// these.
		const cVector2l vDeliverSize = GetTargetSize();
		WorldDrawCtx deliverCtx{};
		deliverCtx.viewport   = this;
		deliverCtx.cmd        = &pGraphics->primary.cmds[0];
		deliverCtx.device     = &pGraphics->device;
		deliverCtx.frame      = cntx;
		deliverCtx.frustum    = pFrustum;
		deliverCtx.width      = vDeliverSize.x > 0 ? (uint32_t)vDeliverSize.x : 0;
		deliverCtx.height     = vDeliverSize.y > 0 ? (uint32_t)vDeliverSize.y : 0;
		deliverCtx.frameIndex = pGraphics->frameIndex;
		deliverCtx.frameTime  = afFrameTime;
		if(const auto *pView = std::get_if<TargetView>(&mTarget))
		{
			if(!pView->view.isEmpty() &&
			   worldRendered && pPogo != nullptr)
			{
				// Caller texture: discard previous contents (fully
				// rewritten; also covers its first use) -> COLOR for the
				// delivery draw, then -> SHADER_READ for the consumer.
				RITexture *pViewTexture = const_cast<RITexture *>(&pView->texture);
				pGraphics->primary.cmds[0].vk_d3d12_textureBarrier(ColorBarrier(pViewTexture,
								 RI_RESOURCE_STATE_UNDEFINED, RI_STAGE_FRAGMENT,
								 RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE));

				DrawPogoToTarget(pPogo, pView->view, pView->width, pView->height,
								 pView->format, 2u, "PostEffect.targetViewBlit");

				pGraphics->primary.cmds[0].vk_d3d12_textureBarrier(ColorBarrier(pViewTexture,
								 RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE,
								 RI_RESOURCE_STATE_SHADER_RESOURCE, RI_STAGE_FRAGMENT));

				// The target is in a known layout (SHADER_READ_ONLY) —
				// handlers may record against it (e.g. a readback copy; the
				// thumbnail builder transitions to COPY_SRC and back).
				m_onPostDelivery.Signal(deliverCtx);
			}
		}
		// TargetSwapchain: composite to the swapchain — tail draw of the
		// (post-processed) pogo read half BEFORE the GUI overlays (the GUI
		// block runs in cScene::Render, which owns the gui sets).
		else if(std::holds_alternative<TargetSwapchain>(mTarget))
		{
			if(worldRendered && pPogo != nullptr)
			{
				// cGraphics::BeginActiveSet already moved the swapchain image
				// UNDEFINED -> RENDER_TARGET_READ in this command list. Declaring
				// UNDEFINED again would emit SyncBefore NONE on an already-accessed
				// subresource, which D3D12 rejects; order against its real state.
				pGraphics->primary.cmds[0].vk_d3d12_textureBarrier(ColorBarrier(
									&pGraphics->swapchain->textures[pGraphics->swapchainIndex],
								 RI_RESOURCE_STATE_RENDER_TARGET_READ, RI_STAGE_NONE,
								 RI_RESOURCE_STATE_RENDER_TARGET, RI_STAGE_NONE));

				DrawPogoToTarget(pPogo, *pGraphics->swapchain->textureView(pGraphics->swapchainIndex),
								 pGraphics->swapchain->width, pGraphics->swapchain->height,
								 (RI_Format_e)pGraphics->swapchain->format, 1u,
								 "PostEffect.tailBlit");

				m_onPostDelivery.Signal(deliverCtx);
			}
		}

		return worldRendered;
	}


	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cViewport::SetCamera(cCamera *apCamera)
	{
		if(apCamera == mpCamera)
			return;

		mpCamera = apCamera;
		ReleaseTemporalProvider();
		ClearTemporalProviderFailure();
		TemporalHistoryResetTriggers resetTriggers;
		resetTriggers.cameraReplaced = true;
		mTemporalHistoryReset |= TemporalHistoryResetRequired(resetTriggers);
		if(auto *pState = std::get_if<HybridViewportState>(&m_state))
		{
			// Reflection history is tied to the camera pose. A camera swap is a
			// genuine discontinuity; ordinary camera movement is handled by the
			// renderer and must not reset the water denoiser.
			pState->waterReflection.reset();
			pState->waterHistoryReset = true;
			pState->waterPrevCameraValid = false;
		}
	}

	//-----------------------------------------------------------------------
	
	void cViewport::SetWorld(cWorld *apWorld)
	{ 
		if(mpWorld != NULL && mpScene->WorldExists(mpWorld))
		{
			mpWorld->SetIsSoundEmitter(false);
		}

		mpWorld = apWorld;
		if(mpWorld) mpWorld->SetIsSoundEmitter(true);

		mpRenderSettings->ResetVariables();
		mTemporalUpscalerRequestedSettings = mpRenderSettings->mTemporalUpscaler;
		mTemporalUpscalerStatus.requestedProvider =
				mTemporalUpscalerRequestedSettings.provider;
		mTemporalUpscalerStatus.requestedQuality =
				mTemporalUpscalerRequestedSettings.quality;
		ReleaseTemporalProvider();
		ClearTemporalProviderFailure();
		TemporalHistoryResetTriggers resetTriggers;
		resetTriggers.worldReplaced = true;
		mTemporalHistoryReset |= TemporalHistoryResetRequired(resetTriggers);

		if(auto *pState = std::get_if<HybridViewportState>(&m_state))
		{
			// Reflection history must never transfer across worlds or level
			// loads. The opaque NRD state is intentionally left untouched here.
			pState->waterReflection.reset();
			pState->waterHistoryReset = true;
			pState->waterPrevCameraValid = false;
		}
	}
	

	//-----------------------------------------------------------------------

	void cViewport::AddGuiSet(cGuiSet *apSet)
	{
		m_guiSets.push_back(apSet);
	}
	void cViewport::RemoveGuiSet(cGuiSet *apSet)
	{
		STLFindAndRemove(m_guiSets, apSet);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	

	//-----------------------------------------------------------------------

}
