/*
 * Off-screen "virtual camera" capture for the LevelEditor MCP server.
 *
 * Lets an external AI agent render the current map from an arbitrary camera
 * (eye position + look-at target + fov) and get back a PNG image, so it can
 * actually SEE the scene it is editing instead of only reading entity XML.
 *
 * Shape (mirrors cEditorThumbnailBuilder — async, GPU-resident):
 *   - The MCP tool handler only ENQUEUES a job (Enqueue) and returns a
 *     "deferred" result carrying the job id; the parked HTTP promise lives in
 *     the MCP server keyed by that id (see LevelEditorMCPServer::FulfillDeferred).
 *   - Pump() runs each frame from cLevelEditor::OnPostRender (AFTER cScene::Render,
 *     so the editor world's per-frame GPU data — TLAS / lights / decals — has
 *     already been published by PrepareFrame). It drives a small state machine:
 *       NEW       -> set the camera pose, point a persistent headless viewport
 *                    (eRenderer_Main + tonemap -- re-fetched per capture, so it
 *                    follows a live View > Render mode backend switch) at the
 *                    editor world, allocate a
 *                    per-job RGBA8_UNORM target + a host-readback buffer, and
 *                    Evaluate once. The viewport's OnPostDelivery records a
 *                    barrier + vkCmdCopyImageToBuffer into the readback buffer.
 *       SUBMITTED -> after RI_NUMBER_FRAMES_FLIGHT frames the copy's fence has
 *                    signalled; map the buffer, encode a PNG, base64 it, build
 *                    the MCP image content block and push it to the completed
 *                    queue (drained by cLevelEditor and handed to the server).
 *   - At most ONE Evaluate per frame (the viewport's once-per-frame guard), so a
 *     NEW job starts only when no other started this frame; completions are
 *     unbounded (they record no commands).
 */

#ifndef LEVEL_EDITOR_CAMERA_CAPTURE_H
#define LEVEL_EDITOR_CAMERA_CAPTURE_H

#include "graphics/RITypes.h"
#include "math/MathTypes.h"
#include "system/Event.h"

#include "LevelEditorMCPServer.h" // cMCPToolResult

#include <list>
#include <string>
#include <utility>
#include <vector>

using namespace hpl;

class iEditorBase;

namespace hpl {
	class cViewport;
	class cCamera;
	class cWorld;
	class cPostEffectComposite;
	class iPostEffect;
	struct WorldDrawCtx;
}

//--------------------------------------------------------------------

// One capture request (virtual camera + image + tonemap overrides). Exposure
// and gamma default to the editor viewport's values so a plain capture matches
// what the user sees; raise them to inspect deliberately dark scenes.
struct cCaptureRequest
{
	cVector3f mvPos    = 0;
	cVector3f mvTarget = 0;
	float     mfFov    = 0;
	float     mfNear   = 0.05f;
	float     mfFar    = 1000.0f;
	int       mlWidth  = 1024;
	int       mlHeight = 576;
	bool      mbIncludeVisible = false;
	float     mfExposure = 1.0f; // linear multiplier applied before tonemapping
	// Display gamma after encode (>1 lifts shadows). Fallback only — the MCP
	// path fills this from iEditorViewport::GetDisplayGamma() unless the caller
	// passed an explicit 'gamma'.
	float     mfGamma    = 1.0f;
};

class cLevelEditorCameraCapture
{
public:
	cLevelEditorCameraCapture(iEditorBase* apEditor);
	~cLevelEditorCameraCapture();

	// Queue a capture from the given virtual camera. Returns a job id (>=0) to
	// pair the eventual result with the parked MCP promise, or -1 if capture is
	// unavailable (headless setup failed). Runs on the MAIN thread.
	int Enqueue(const cCaptureRequest& aRequest);

	// Per-frame pump, driven by cLevelEditor::OnPostRender (see class comment).
	void Pump(float afFrameTime);

	// Pop one finished capture (jobId + its MCP result). Returns false when the
	// completed queue is empty. cLevelEditor drains these into the MCP server.
	bool PopCompleted(int& alJobId, cMCPToolResult& aResult);

	bool IsAvailable() const { return mpViewport != NULL; }

private:
	// Idle:      queued, not yet rendered — Pump promotes it to Warming.
	// Warming:   rendering the SAME static pose for kCaptureAccumFrames consecutive
	//            frames so the per-viewport temporal histories (direct-lighting
	//            denoiser, ReSTIR DI reservoirs) + newly-spawned surfels converge.
	//            The readback copy is recorded only on the final warm frame.
	// Staged:    final frame's readback copy recorded; waiting for the GPU to reach
	//            the submit's timeline value (mStageValue).
	// Completed: GPU done, PNG result built; ready to hand to the MCP server.
	enum eCaptureState { eCaptureState_Idle, eCaptureState_Warming, eCaptureState_Staged, eCaptureState_Completed };

	struct cCaptureJob
	{
		int            mlId        = -1;
		cVector3f      mvPos       = 0;
		cVector3f      mvTarget    = 0;
		float          mfFov       = 0;
		float          mfNear      = 0;
		float          mfFar       = 0;
		int            mlWidth     = 0;
		int            mlHeight    = 0;
		// When set, ComputeVisibleEntities() fills mvVisibleIds at pose time and the
		// result appends a {"visible":[...]} text block next to the PNG.
		bool           mbIncludeVisible = false;
		std::vector<int> mvVisibleIds;
		// Tonemap overrides applied to the capture's tonemap effect at pose time
		// (re-applied per job so one job's override never leaks into the next).
		float          mfExposure  = 1.0f;
		float          mfGamma     = 1.0f;
		eCaptureState  mState      = eCaptureState_Idle;
		// Warm (accumulation) frames left to render before staging the readback.
		int            mWarmRemaining = 0;
		// Graphics-timeline value this job's submit signals; its readback copy is
		// complete once cGraphics::graphicsTimeline.completed() reaches it.
		uint64_t       mStageValue = 0;

		// Per-job GPU resources: an RGBA8_UNORM color target the viewport delivers
		// into (TRANSFER_SRC backs the readback copy) + a host-readable buffer the
		// copy lands in. All destroyed through cGraphics::graphicsDefer, so none is
		// freed while an in-flight frame still references it.
		RISharedPointer<RITexture>     mTargetTexture;
		RISharedPointer<RITextureView> mTargetView;
		RISharedPointer<RIBuffer>      mReadback;

		// Filled on Staged -> Completed; handed to the server on the next Pump.
		cMCPToolResult                 mResult;
	};

	// OnPostDelivery handler body: barrier the delivered target to COPY_SRC and
	// record the vkCmdCopyImageToBuffer into the active job's readback buffer.
	void RecordReadbackCopy(const WorldDrawCtx& ctx);

	// Frustum-test every editor entity against the job's (already-posed) camera and
	// record the visible ones' ids in aJob.mvVisibleIds. Called at pose time because
	// the camera is shared across jobs.
	void ComputeVisibleEntities(cCaptureJob& aJob);

	// SUBMITTED -> done: map the readback buffer, encode a PNG, base64 it and
	// build the MCP image content block. Frees the job's GPU resources.
	cMCPToolResult BuildResultFromReadback(cCaptureJob& aJob);

	// Free a job's per-job GPU resources (texture deferred to the freelist,
	// readback buffer disposed — safe once its fence has signalled).
	void FreeJobResources(cCaptureJob& aJob);

	/////////////////////////////////////
	// Data
	iEditorBase* mpEditor;

	cViewport*             mpViewport;
	cCamera*               mpCamera;
	cPostEffectComposite*  mpPostEffectComposite;
	iPostEffect*           mpPostEffectToneMap;

	std::list<cCaptureJob> mlstJobs;
	std::list<std::pair<int,cMCPToolResult>> mlstCompleted;

	int mlNextJobId;

	// frameIndex of our last warm Evaluate. Pump warms at most once per frame:
	// OnPostRender can fire more than once per frameIndex (skipped / not-yet-
	// submitted frames), and cViewport::Evaluate asserts on a second call in the
	// same frame. ~0u = never evaluated.
	uint32_t mLastEvalFrame;

	// The job Evaluated this frame — RecordReadbackCopy copies into its readback
	// buffer. Armed by Pump around Evaluate (cleared by the handler).
	cCaptureJob* mpActiveJob;

	EventHandler<const WorldDrawCtx&> mPostDeliveryHandler;
};

//--------------------------------------------------------------------

#endif // LEVEL_EDITOR_CAMERA_CAPTURE_H
