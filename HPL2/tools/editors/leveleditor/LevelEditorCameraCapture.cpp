/*
 * See LevelEditorCameraCapture.h for the design + threading contract.
 *
 * This TU deliberately includes NO rapidjson — it only builds the
 * MCP *image* content block, whose payload is base64 (a JSON-safe alphabet), so
 * it is assembled by plain string concatenation. That keeps hpl.h's X11 headers
 * (which #define Bool) away from rapidjson entirely.
 */

#include "hpl.h"
using namespace hpl;

#include "LevelEditorCameraCapture.h"

#include "../common/EditorBaseClasses.h"
#include "../common/EditorWorld.h"
#include "../common/EntityWrapper.h"

#include "math/BoundingVolume.h"
#include "math/Frustum.h"

#include "graphics/Graphics.h"

#include "Constants.h"   // kSceneExposure
#include "graphics/PostEffect_ToneMap.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "scene/Viewport.h"
#include "scene/World.h"

#include <IL/il.h>
#include <vk_mem_alloc.h>

#include <cstring>
#include <vector>

//--------------------------------------------------------------------

// Default / clamp bounds for the requested image size (kept modest so a single
// readback buffer + PNG encode stays cheap and the base64 payload reasonable).
static constexpr int kCaptureDefaultW = 1024;
static constexpr int kCaptureDefaultH = 576;
static constexpr int kCaptureMaxDim   = 2048;
static constexpr int kCaptureMinDim   = 16;

// Consecutive frames to render the same static pose before reading back, so the
// hybrid renderer's per-viewport temporal histories (direct-lighting denoiser +
// ReSTIR DI reservoirs) and any newly-spawned surfels converge. A static camera
// reprojects as identity, so history accumulates cleanly (count ramps 1 ->
// kDirectMaxAccum=64). ~48 gets closer to the kDirectTemporalAlpha=0.02 steady
// state; 32 is a good latency/quality balance (well under the 15s call timeout).
static constexpr int kCaptureAccumFrames = 32;

static int ClampInt(int alX, int alMin, int alMax)
{
	return alX < alMin ? alMin : (alX > alMax ? alMax : alX);
}

//--------------------------------------------------------------------

// Minimal RFC 4648 base64 (kept local so httplib.h stays out of this hpl TU).
static std::string Base64Encode(const unsigned char* apData, size_t alSize)
{
	static const char* kTable =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((alSize + 2) / 3) * 4);
	size_t i = 0;
	for(; i + 3 <= alSize; i += 3)
	{
		uint32_t n = (apData[i] << 16) | (apData[i+1] << 8) | apData[i+2];
		out.push_back(kTable[(n >> 18) & 0x3F]);
		out.push_back(kTable[(n >> 12) & 0x3F]);
		out.push_back(kTable[(n >>  6) & 0x3F]);
		out.push_back(kTable[ n        & 0x3F]);
	}
	if(alSize - i == 1)
	{
		uint32_t n = apData[i] << 16;
		out.push_back(kTable[(n >> 18) & 0x3F]);
		out.push_back(kTable[(n >> 12) & 0x3F]);
		out.push_back('=');
		out.push_back('=');
	}
	else if(alSize - i == 2)
	{
		uint32_t n = (apData[i] << 16) | (apData[i+1] << 8);
		out.push_back(kTable[(n >> 18) & 0x3F]);
		out.push_back(kTable[(n >> 12) & 0x3F]);
		out.push_back(kTable[(n >>  6) & 0x3F]);
		out.push_back('=');
	}
	return out;
}

// Encode tightly-packed RGBA8 pixels (top-row-first) to an in-memory PNG.
static bool EncodePngRGBA(const unsigned char* apData, int alWidth, int alHeight, std::string& asOut)
{
	// DevIL is initialised by the resource bitmap loader at startup; guard a
	// redundant init in case capture runs before any texture load.
	static bool s_bIlInit = false;
	if(s_bIlInit == false) { ilInit(); s_bIlInit = true; }

	// Our readback rows are top-to-bottom (Vulkan framebuffer origin), but DevIL's
	// PNG writer emits its in-memory image bottom-up — i.e. it flips vertically on
	// save (IL_ORIGIN_SET / IL_IMAGE_ORIGIN only affect loading, verified). Hand
	// it bottom-up rows so the flip lands us back at a correctly-oriented PNG.
	const size_t rowBytes = (size_t)alWidth * 4;
	std::vector<unsigned char> flipped((size_t)alHeight * rowBytes);
	for(int y = 0; y < alHeight; ++y)
		memcpy(&flipped[(size_t)(alHeight - 1 - y) * rowBytes],
			   apData + (size_t)y * rowBytes, rowBytes);

	ILuint lImg = 0;
	ilGenImages(1, &lImg);
	ilBindImage(lImg);

	if(ilTexImage(alWidth, alHeight, 1, 4, IL_RGBA, IL_UNSIGNED_BYTE, flipped.data()) == 0)
	{
		ilDeleteImages(1, &lImg);
		return false;
	}

	// First call sizes the buffer, second fills it (DevIL in-memory save).
	ILuint lSize = ilSaveL(IL_PNG, NULL, 0);
	if(lSize == 0)
	{
		ilDeleteImages(1, &lImg);
		return false;
	}
	asOut.resize(lSize);
	ILuint lWritten = ilSaveL(IL_PNG, &asOut[0], lSize);
	ilDeleteImages(1, &lImg);
	if(lWritten == 0)
		return false;
	asOut.resize(lWritten);
	return true;
}

//--------------------------------------------------------------------

cLevelEditorCameraCapture::cLevelEditorCameraCapture(iEditorBase* apEditor)
{
	mpEditor              = apEditor;
	mpViewport            = NULL;
	mpCamera              = NULL;
	mpPostEffectComposite = NULL;
	mpPostEffectToneMap   = NULL;
	mlNextJobId           = 0;
	mLastEvalFrame        = 0xFFFFFFFFu;
	mpActiveJob           = NULL;

	cEngine*   pEngine = mpEditor->GetEngine();
	cScene*    pScene  = pEngine->GetScene();
	cGraphics* pGfx    = pEngine->GetGraphics();

	//////////////////////////////////////////
	// Virtual camera: Fly move mode + Euler rotation so an arbitrary look-at
	// pose (yaw/pitch) applies directly; no pitch clamp.
	mpCamera = pScene->CreateCamera(eCameraMoveMode_Fly);
	mpCamera->SetPitchLimits(0, 0);

	//////////////////////////////////////////
	// One persistent HEADLESS viewport — never in cScene's visible loop; Pump
	// Evaluates it directly. eRenderer_Main (whichever lit renderer is active) so
	// the capture matches the editor's Main viewport; the world, the renderer and
	// the per-job target are set in Pump.
	mpViewport = pScene->CreateViewport(mpCamera, NULL);
	mpViewport->SetRenderer(pGfx->GetRenderer(eRenderer_Main));
	mpViewport->SetActive(false);
	mpViewport->SetVisible(false);

	//////////////////////////////////////////
	// Post chain: the hybrid renderer outputs linear HDR; the tonemap effect is
	// the mandatory display encode — exposure multiply, sRGB OETF, then the
	// display gamma (same as the editor pane's Main viewport). Without it the
	// readback pixels are wrong.
	mpPostEffectComposite = pGfx->CreatePostEffectComposite();
	mpViewport->SetPostEffectComposite(mpPostEffectComposite);

	cPostEffectParams_ToneMap tonemapParams;
	tonemapParams.mfExposure   = kSceneExposure;
	tonemapParams.mfShadowLift = 1.0f;
	mpPostEffectToneMap = pGfx->CreatePostEffect(&tonemapParams);
	mpPostEffectComposite->AddPostEffect(mpPostEffectToneMap, 0);

	//////////////////////////////////////////
	// Record the readback copy from the viewport's delivery stage — the handler
	// runs inside Evaluate with the delivered target in a known layout.
	mPostDeliveryHandler = EventHandler<const WorldDrawCtx&>(
		[this](const WorldDrawCtx& ctx){ RecordReadbackCopy(ctx); });
	mPostDeliveryHandler.Connect(mpViewport->OnPostDelivery());
}

//--------------------------------------------------------------------

cLevelEditorCameraCapture::~cLevelEditorCameraCapture()
{
	// Drop any queued/in-flight jobs — free their GPU resources. Parked HTTP
	// promises live in the MCP server and are error-fulfilled by its Stop().
	for(cCaptureJob& job : mlstJobs)
		FreeJobResources(job);
	mlstJobs.clear();
	mlstCompleted.clear();

	cScene* pScene = mpEditor->GetEngine()->GetScene();
	cGraphics* pGfx = mpEditor->GetEngine()->GetGraphics();

	if(mpPostEffectToneMap)   pGfx->DestroyPostEffect(mpPostEffectToneMap);
	if(mpPostEffectComposite) pGfx->DestroyPostEffectComposite(mpPostEffectComposite);
	if(mpViewport)            pScene->DestroyViewport(mpViewport);
	if(mpCamera)              pScene->DestroyCamera(mpCamera);
}

//--------------------------------------------------------------------

int cLevelEditorCameraCapture::Enqueue(const cCaptureRequest& aRequest)
{
	if(mpViewport == NULL) return -1; // headless setup failed

	cCaptureJob job;
	job.mlId     = mlNextJobId++;
	job.mvPos    = aRequest.mvPos;
	job.mvTarget = aRequest.mvTarget;
	job.mfFov    = aRequest.mfFov;
	job.mfNear   = aRequest.mfNear;
	job.mfFar    = aRequest.mfFar;
	job.mlWidth  = ClampInt(aRequest.mlWidth,  kCaptureMinDim, kCaptureMaxDim);
	job.mlHeight = ClampInt(aRequest.mlHeight, kCaptureMinDim, kCaptureMaxDim);
	job.mbIncludeVisible = aRequest.mbIncludeVisible;
	job.mfExposure = aRequest.mfExposure;
	job.mfGamma    = aRequest.mfGamma;
	job.mState   = eCaptureState_Idle;

	mlstJobs.push_back(std::move(job));
	return mlstJobs.back().mlId;
}

//--------------------------------------------------------------------

void cLevelEditorCameraCapture::Pump(float afFrameTime)
{
	if(mpViewport == NULL || mlstJobs.empty()) return;

	cGraphics* pGfx = Interface<cGraphics>::Get();

	//////////////////////////////////////////
	// 1) Staged -> Completed. The readback copy rode this job's submit, so it is
	//    done precisely once the GPU has reached that submit's timeline value —
	//    no frame counting. Poll the graphics timeline the frame deferral uses.
	const uint64_t lCompleted = pGfx->graphicsTimeline.completed(&pGfx->device);
	for(cCaptureJob& job : mlstJobs)
	{
		if(job.mState == eCaptureState_Staged && lCompleted >= job.mStageValue)
		{
			job.mResult = BuildResultFromReadback(job);
			job.mState  = eCaptureState_Completed;
		}
	}

	//////////////////////////////////////////
	// 2) Completed -> hand the result to the MCP server + free (deferred). Any
	//    number may finish in one frame; these record no GPU work.
	for(std::list<cCaptureJob>::iterator it = mlstJobs.begin(); it != mlstJobs.end(); )
	{
		if(it->mState == eCaptureState_Completed)
		{
			mlstCompleted.push_back(std::make_pair(it->mlId, it->mResult));
			FreeJobResources(*it);
			it = mlstJobs.erase(it);
		}
		else
			++it;
	}

	//////////////////////////////////////////
	// 3) Serialize: keep at most ONE capture in flight (Warming or Staged). The
	//    shared headless viewport accumulates one job's temporal history at a
	//    time and is re-sized per job, so overlapping jobs would churn its render
	//    targets while the GPU reads them.
	cCaptureJob* pJob = NULL;
	for(cCaptureJob& job : mlstJobs)
		if(job.mState == eCaptureState_Warming || job.mState == eCaptureState_Staged)
		{
			pJob = &job;
			break;
		}

	//////////////////////////////////////////
	// 4) Nothing in flight: promote one Idle job to Warming. Allocate its GPU
	//    resources and set the STATIC camera pose ONCE — kept fixed across the warm
	//    frames so temporal reprojection stays identity and history accumulates.
	if(pJob == NULL)
	{
		for(cCaptureJob& job : mlstJobs)
			if(job.mState == eCaptureState_Idle) { pJob = &job; break; }
		if(pJob == NULL) return;

		iEditorWorld* pEdWorld = mpEditor->GetEditorWorld();
		cWorld* pWorld = pEdWorld ? pEdWorld->GetWorld() : NULL;
		if(pWorld == NULL) return; // no world yet; retry next frame (still Idle)

		// The readback (RecordReadbackCopy / BuildResultFromReadback) is still
		// Vulkan-only: a D3D12 port needs 256 B-aligned row pitches + repacking.
		if(RIActiveBackendApi() != RI_DEVICE_API_VK)
		{
			cMCPToolResult err;
			err.mbIsError = true;
			err.msContentJson = "[{\"type\":\"text\",\"text\":\"capture failed: camera capture is only supported on the Vulkan backend\"}]";
			mlstCompleted.push_back(std::make_pair(pJob->mlId, err));
			for(std::list<cCaptureJob>::iterator it = mlstJobs.begin(); it != mlstJobs.end(); ++it)
				if(&(*it) == pJob) { mlstJobs.erase(it); break; }
			return;
		}

		//////////////////////////////////////////
		// Per-job GPU resources: an RGBA8_UNORM color target (TRANSFER_SRC backs the
		// readback copy) + a host-readable buffer the copy lands in. UNORM is
		// deliberate: the tonemap post effect already writes DISPLAY-ENCODED sRGB
		// values (posteffect_tonemap.frag.slang does the linearToSRGB), and the
		// editor pane stores them raw too — an SRGB attachment here would encode a
		// second time and wash the capture out relative to the viewport. Same size
		// for every warm frame, so HybridViewportState::Update never recreates
		// (which would reset the temporal history we are accumulating).
		if(!CreateViewportColorTexture(&pGfx->device, (uint32_t)pJob->mlWidth, (uint32_t)pJob->mlHeight,
									   RI_FORMAT_RGBA8_UNORM,
									   RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_SRC,
									   &pJob->mTargetTexture, &pJob->mTargetView,
									   "MCPCameraCapture") ||
		   !CreateViewportColorAttachmentView(&pGfx->device, &pJob->mTargetTexture,
											  RI_FORMAT_RGBA8_UNORM, &pJob->mTargetAttachmentView))
		{
			// Give up on this job — report an error rather than retry forever.
			cMCPToolResult err;
			err.mbIsError = true;
			err.msContentJson = "[{\"type\":\"text\",\"text\":\"capture failed: could not allocate render target\"}]";
			mlstCompleted.push_back(std::make_pair(pJob->mlId, err));
			FreeJobResources(*pJob);
			for(std::list<cCaptureJob>::iterator it = mlstJobs.begin(); it != mlstJobs.end(); ++it)
				if(&(*it) == pJob) { mlstJobs.erase(it); break; }
			return;
		}

		RIBufferDesc bd = {};
		bd.size     = (uint64_t)pJob->mlWidth * (uint64_t)pJob->mlHeight * 4ull;
		bd.usage    = RI_BUFFER_USAGE_TRANSFER_DST;
		bd.location = RI_MEMORY_HOST_READBACK;
		RIBuffer rb = RIBuffer::create(&pGfx->device, bd);
		if(rb.isEmpty())
		{
			// Without a readback buffer the OnPostDelivery copy would target null —
			// fail the job cleanly instead of evaluating.
			cMCPToolResult err;
			err.mbIsError = true;
			err.msContentJson = "[{\"type\":\"text\",\"text\":\"capture failed: could not allocate readback buffer\"}]";
			mlstCompleted.push_back(std::make_pair(pJob->mlId, err));
			FreeJobResources(*pJob);
			for(std::list<cCaptureJob>::iterator it = mlstJobs.begin(); it != mlstJobs.end(); ++it)
				if(&(*it) == pJob) { mlstJobs.erase(it); break; }
			return;
		}
		pJob->mReadback = RISharedPointer<RIBuffer>(&pGfx->device, rb);

		//////////////////////////////////////////
		// Static camera pose (set ONCE): eye at position, look at target (yaw/pitch
		// from the two points, matching the thumbnail builder's look-at math),
		// aspect from the requested image size.
		mpCamera->SetPosition(pJob->mvPos);
		cVector3f vAngles = cMath::GetAngleFromPoints3D(pJob->mvPos, pJob->mvTarget);
		mpCamera->SetYaw(vAngles.y);
		if(vAngles.x > kPif) vAngles.x = vAngles.x - k2Pif;
		mpCamera->SetPitch(vAngles.x);
		mpCamera->SetFOV(pJob->mfFov);
		mpCamera->SetAspect((float)pJob->mlWidth / (float)pJob->mlHeight);
		mpCamera->SetNearClipPlane(pJob->mfNear);
		mpCamera->SetFarClipPlane(pJob->mfFar);

		// Snapshot which entities fall in this pose's frustum NOW — mpCamera is shared,
		// so a later job would re-pose it before this one's readback completes.
		if(pJob->mbIncludeVisible)
			ComputeVisibleEntities(*pJob);

		// Apply this job's tonemap overrides. Set unconditionally (defaults are the
		// editor-viewport values) so a previous job's override never leaks forward.
		// Safe: at most one job is Warming/Staged at a time.
		cPostEffectParams_ToneMap tonemapParams;
		tonemapParams.mfExposure   = pJob->mfExposure;
		tonemapParams.mfShadowLift = 1.0f;
		tonemapParams.mfGamma      = pJob->mfGamma;
		mpPostEffectToneMap->SetParams(&tonemapParams);

		pJob->mWarmRemaining = kCaptureAccumFrames;
		pJob->mState         = eCaptureState_Warming;
	}

	//////////////////////////////////////////
	// 5) Staged jobs just wait for the timeline; step 1 completes them.
	if(pJob->mState == eCaptureState_Staged)
		return;

	//////////////////////////////////////////
	// 6) Warm ONE frame (one Evaluate — the viewport's once-per-frame guard).
	//    Point at the CURRENT editor world each frame so a map reload never leaves
	//    us on a stale world. Record the readback copy + stage the timeline value
	//    ONLY on the final warm frame; earlier frames exist solely to advance the
	//    per-viewport temporal histories (+ the global surfel pool).
	//
	// At most ONE warm Evaluate per frameIndex: OnPostRender can fire more than
	// once per frame (skipped / not-yet-submitted frames), and cViewport::Evaluate
	// asserts on a second call in the same frame. Skip until frameIndex advances.
	if(pGfx->frameIndex == mLastEvalFrame)
		return;

	iEditorWorld* pEdWorld = mpEditor->GetEditorWorld();
	cWorld* pWorld = pEdWorld ? pEdWorld->GetWorld() : NULL;
	if(pWorld == NULL) return; // world gone mid-warm; retry next frame

	mpViewport->SetWorld(pWorld);
	// Re-fetched per capture, not cached from construction: View > Render mode
	// can repoint eRenderer_Main at the other lit renderer at any time.
	mpViewport->SetRenderer(pGfx->GetRenderer(eRenderer_Main));
	if(pEdWorld) mpViewport->GetRenderSettings()->mClearColor = pEdWorld->GetBGDefaultColor();

	cViewport::TargetView target = {};
	target.width  = (uint32_t)pJob->mlWidth;
	target.height = (uint32_t)pJob->mlHeight;
	target.texture      = *pJob->mTargetTexture;
	target.view   = *pJob->mTargetAttachmentView;
	target.format = RI_FORMAT_RGBA8_UNORM;
	mpViewport->SetTarget(target);

	const bool bFinalWarm = (pJob->mWarmRemaining <= 1);

	// mpActiveJob arms RecordReadbackCopy — record the copy only on the final
	// frame. That frame's commands ride the primary submit (the only
	// graphicsTimeline.next() per frame, in CloseAndSubmitActiveSet after OnDraw),
	// which signals pending()+1 — the value marking the readback copy complete.
	mpActiveJob = bFinalWarm ? pJob : NULL;
	mpViewport->Evaluate(pGfx->GetActiveSet(), afFrameTime,
						 tSceneRenderFlag_World | tSceneRenderFlag_PostEffects);
	mpActiveJob    = NULL;
	mLastEvalFrame = pGfx->frameIndex;

	if(pJob->mWarmRemaining > 0)
		--pJob->mWarmRemaining;

	if(bFinalWarm)
	{
		pJob->mStageValue = pGfx->graphicsTimeline.pending() + 1;
		pJob->mState      = eCaptureState_Staged;
	}
}

//--------------------------------------------------------------------

void cLevelEditorCameraCapture::RecordReadbackCopy(const WorldDrawCtx& ctx)
{
	// Fired for EVERY delivery — act only when this Pump armed a job.
	if(mpActiveJob == NULL) return;
	cCaptureJob* pJob = mpActiveJob;

	RICmd* pCmd = ctx.cmd;

	// Delivery left the target SHADER_RESOURCE; move it to COPY_SRC and copy the
	// whole image into the host-readable buffer (tightly packed rows).
	RITextureBarrier toCopy(pJob->mTargetTexture.Get(),
		RI_RESOURCE_STATE_SHADER_RESOURCE, RI_RESOURCE_STATE_COPY_SRC,
		RI_STAGE_FRAGMENT, RI_STAGE_COPY);
	pCmd->vk_d3d12_textureBarrier(toCopy);

	VkBufferImageCopy region = {};
	region.bufferOffset      = 0;
	region.bufferRowLength   = 0; // tightly packed
	region.bufferImageHeight = 0;
	region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageOffset       = { 0, 0, 0 };
	region.imageExtent       = { (uint32_t)pJob->mlWidth, (uint32_t)pJob->mlHeight, 1 };
	vkCmdCopyImageToBuffer(pCmd->vk.cmd,
						   pJob->mTargetTexture->vk.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   pJob->mReadback->vk.buffer, 1, &region);
	// No restore barrier: the target is a per-job image freed once the copy's
	// fence signals; layout is irrelevant to its destruction.
}

//--------------------------------------------------------------------

void cLevelEditorCameraCapture::ComputeVisibleEntities(cCaptureJob& aJob)
{
	aJob.mvVisibleIds.clear();

	iEditorWorld* pEdWorld = mpEditor->GetEditorWorld();
	if(pEdWorld == NULL) return;

	cFrustum* pFrustum = mpCamera->GetFrustum();
	tEntityWrapperMap& mapEntities = pEdWorld->GetEntities();
	for(tEntityWrapperMapIt it = mapEntities.begin(); it != mapEntities.end(); ++it)
	{
		iEntityWrapper* pEnt = it->second;
		if(pEnt == NULL || pEnt->IsVisible() == false) continue; // match the rendered image

		// Mesh entities carry a render AABB; point-like entities (lights, areas) have
		// none, so fall back to the icon volume, then to a point test. (Mirrors
		// iEngineEntity::IsCulledByFrustum; a plain IsCulledByFrustum would call every
		// engine-entity-less wrapper "visible".)
		cBoundingVolume* pBV = pEnt->GetRenderBV();
		if(pBV == NULL) pBV = pEnt->GetPickBV(NULL);
		bool bVisible = pBV ? (pFrustum->CollideBoundingVolume(pBV) != eCollision_Outside)
							: pFrustum->CollidePoint(pEnt->GetPosition());
		if(bVisible) aJob.mvVisibleIds.push_back(pEnt->GetID());
	}
}

//--------------------------------------------------------------------

cMCPToolResult cLevelEditorCameraCapture::BuildResultFromReadback(cCaptureJob& aJob)
{
	cMCPToolResult r;

	cGraphics* pGfx = Interface<cGraphics>::Get();

	if(aJob.mReadback.isEmpty() || aJob.mReadback->mappedAddress == NULL)
	{
		r.mbIsError = true;
		r.msContentJson = "[{\"type\":\"text\",\"text\":\"capture failed: readback buffer not mapped\"}]";
		return r;
	}

	// The copy's submit has completed (the caller gated on the graphics timeline);
	// invalidate in case the readback landed in non-coherent HOST_CACHED memory,
	// then read.
	vmaInvalidateAllocation(pGfx->device.vk.vmaAllocator, aJob.mReadback->vk.allocation, 0, VK_WHOLE_SIZE);

	const unsigned char* pPixels = (const unsigned char*)aJob.mReadback->mappedAddress;

	std::string sPng;
	if(EncodePngRGBA(pPixels, aJob.mlWidth, aJob.mlHeight, sPng) == false)
	{
		r.mbIsError = true;
		r.msContentJson = "[{\"type\":\"text\",\"text\":\"capture failed: PNG encode error\"}]";
		return r;
	}

	// base64 uses only [A-Za-z0-9+/=] — all JSON-safe, so the content block is
	// assembled without a JSON library.
	std::string sB64 = Base64Encode((const unsigned char*)sPng.data(), sPng.size());
	r.mbIsError = false;

	std::string sContent = "[{\"type\":\"image\",\"data\":\"" + sB64 + "\",\"mimeType\":\"image/png\"}";
	if(aJob.mbIncludeVisible)
	{
		// Second content block: a text block whose text is itself JSON the agent can
		// parse. Ids are integers, so no escaping/injection concerns.
		std::string sIds;
		for(size_t i=0;i<aJob.mvVisibleIds.size();++i)
		{
			if(i) sIds += ",";
			sIds += std::to_string(aJob.mvVisibleIds[i]);
		}
		sContent += ",{\"type\":\"text\",\"text\":\"{\\\"visible\\\":[" + sIds + "]}\"}";
	}
	sContent += "]";
	r.msContentJson = sContent;
	return r;
}

//--------------------------------------------------------------------

void cLevelEditorCameraCapture::FreeJobResources(cCaptureJob& aJob)
{
	// Every per-job GPU resource is destroyed through the main graphics deferral
	// queue, so none is freed while an in-flight frame still references it — the
	// color target is left SHADER_RESOURCE and sampled by the composite/tonemap,
	// so freeing it early is a GPU use-after-free (RADV TCP read VM fault).
	// graphicsDefer.push() no-ops on an empty handle.
	cGraphics* pGfx = Interface<cGraphics>::Get();
	pGfx->graphicsDefer.push(aJob.mTargetAttachmentView);
	pGfx->graphicsDefer.push(aJob.mTargetView);
	pGfx->graphicsDefer.push(aJob.mTargetTexture);
	pGfx->graphicsDefer.push(aJob.mReadback);
	aJob.mTargetAttachmentView = {};
	aJob.mTargetView    = {};
	aJob.mTargetTexture = {};
	aJob.mReadback      = {};
}

//--------------------------------------------------------------------

bool cLevelEditorCameraCapture::PopCompleted(int& alJobId, cMCPToolResult& aResult)
{
	if(mlstCompleted.empty()) return false;
	alJobId = mlstCompleted.front().first;
	aResult = mlstCompleted.front().second;
	mlstCompleted.pop_front();
	return true;
}
