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

#ifndef HPLEDITOR_EDITOR_THUMBNAIL_BUILDER_H
#define HPLEDITOR_EDITOR_THUMBNAIL_BUILDER_H

#include "../common/StdAfx.h"
#include "graphics/RITypes.h"
#include "system/Event.h"
#include "system/SHA1.h"

#include <functional>
#include <list>
#include <memory>

using namespace hpl;

//-------------------------------------------------------------------

class iEditorBase;

namespace hpl {
	struct cTexture;
	struct WorldDrawCtx;
	class Image;
}

//-------------------------------------------------------------------

// Async, GPU-resident thumbnail factory: the Build* calls only ENQUEUE.
// Each frame, Pump (driven from iEditorBase::OnDraw, inside the command-
// recording window) places the front job's entity in the builder's own world
// and Evaluates one persistent HEADLESS viewport (cRendererSimple, 128x128
// RGBA8_SRGB TargetView — never visible to cScene's loop). The viewport's
// OnPostDelivery event then records barriers + vkCmdCopyImage on the main
// command buffer, copying the delivered target into a per-thumbnail SAMPLED
// texture the GUI can sample directly (the editor-pane Image pattern) — no
// readback, no fences, no disk cache. The finished Image is handed to the
// requester's callback; the builder retains nothing.
class cEditorThumbnailBuilder
{
public:
	using tThumbnailReadyCallback = std::function<void(SharedResourceHandle<Image>)>;

	cEditorThumbnailBuilder(iEditorBase* apEditor);
	~cEditorThumbnailBuilder();

	// Enqueue a thumbnail job. The builder takes ownership of apEntity (it
	// must live in GetWorld()) and destroys it when the job completes or is
	// dropped. When the render finishes (a frame or two later), aOnReady
	// receives the finished Image — OWNERSHIP TRANSFERS to the caller, who
	// releases the resource by dropping the shared_ptr (GPU frees are
	// deferred via the cTexture deleter, so release at any time is safe).
	// The callback may be dropped without ever firing (PreBuild flushes the
	// queue, builder teardown) — it must not assume delivery.
	void BuildThumbnailFromMeshEntity(cMeshEntity* apEntity, tThumbnailReadyCallback aOnReady);
	void BuildThumbnailFromMesh(const tWString& asMeshFilename, tThumbnailReadyCallback aOnReady);
	// Synchronous CPU path (no GPU involved): load, downscale, save to disk
	// (legacy texture-browser flow).
	void BuildThumbnailFromImage(const tWString& asImageFilename, const tWString& asDestName);

	tString GetThumbnailNameFromFile(const tWString& asFile);
	tWString GetThumbnailNameFromFileW(const tWString& asFile);

	void VtxBufferAddNormals(const cMatrixf a_mtxTransform, cVertexBuffer *apVtxBuffer, cVector3f &avVecSum, float& afCount);
	void FocusCameraOnEntity(cMeshEntity* apEntity);

	// Legacy index hooks — the queue manages its own lifecycle now.
	void PreBuild();
	void PostBuild();
	void CleanUp();

	// Per-frame pump, driven by iEditorBase::OnDraw (see class comment).
	void Pump(float afFrameTime);
	bool HasPendingJobs() const { return mlstJobs.empty()==false; }

	// The builder's own world — job entities must be created here (isolated
	// from the editor temp world so nothing else leaks into thumbnails).
	cWorld* GetWorld() { return mpWorld; }

protected:
	// OnPostDelivery handler body: records the target -> cache-texture copy
	// (barriers + vkCmdCopyImage) for the job placed by this Pump. The
	// per-signal context supplies the command buffer (no RI-global reach).
	void RecordCacheCopy(const WorldDrawCtx& ctx);

	/////////////////////////////////////
	// Data
	iEditorBase* mpEditor;

	cViewport* mpViewport;
	cWorld* mpWorld;

	// 128x128 RGBA8_SRGB color target the viewport's TargetView delivers
	// into (sRGB attachment write = free linear->display encoding;
	// TRANSFER_SRC backs the cache copy).
	RISharedPointer<RITexture> mTargetTexture;
	RISharedPointer<RITextureView> mTargetView;
	// COLOR_ATTACHMENT view the delivery draw renders through (D3D12 RTV).
	RISharedPointer<RITextureView> mTargetAttachmentView;

	struct cThumbnailJob
	{
		cMeshEntity* mpEntity;
		tThumbnailReadyCallback mOnReady;
	};
	std::list<cThumbnailJob> mlstJobs;

	// Destination for the copy recorded by RecordCacheCopy — armed by Pump
	// around Evaluate.
	cTexture* mpActiveCopyDst;

	EventHandler<const WorldDrawCtx&> mPostDeliveryHandler;

	SHA1 mSha;
};

//-------------------------------------------------------------------

#endif //HPLEDITOR_EDITOR_THUMBNAIL_BUILDER_H

