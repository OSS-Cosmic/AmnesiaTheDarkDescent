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

#include "scene/Scene.h"

#include "scene/Viewport.h"
#include "scene/Camera.h"
#include "scene/World.h"

#include "system/LowLevelSystem.h"
#include "system/String.h"
#include "system/Script.h"
#include "system/Platform.h"

#include "resources/Resources.h"
#include "resources/ScriptManager.h"
#include "resources/FileSearcher.h"
#include "resources/WorldLoaderHandler.h"

#include "graphics/DebugDraw.h"
#include "graphics/Graphics.h"
#include "graphics/Window.h"
#include "graphics/Renderer.h"
#include "graphics/PostEffectComposite.h"
#include "graphics/PostEffectHelpers.h"
#include "graphics/RIPogoBuffer.h"
#include "system/Hasher.h"

#include "sound/Sound.h"
#include "sound/LowLevelSound.h"
#include "sound/SoundHandler.h"

#include "gui/Gui.h"
#include "gui/GuiSet.h"

#include "physics/Physics.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cScene::cScene(cGraphics *apGraphics,cResources *apResources, cSound* apSound,cPhysics *apPhysics,
					cSystem *apSystem, cAI *apAI,cGui *apGui, cHaptic *apHaptic)
		: iUpdateable("HPL_Scene")
	{
		mpGraphics = apGraphics;
		mpResources = apResources;
		mpSound = apSound;
		mpPhysics = apPhysics;
		mpSystem = apSystem;
		mpAI = apAI;
		mpGui = apGui;
		mpHaptic = apHaptic;

		mpCurrentListener = NULL;

		// Re-apply the screen aspect to perspective cameras when the swapchain
		// changes size — CreateCamera bakes the aspect in once at creation.
		mScreenSizeChangedHandler = Event<const cVector2l&>::Handler(
			[this](const cVector2l& avSize){ OnScreenSizeChange(avSize); });
		mScreenSizeChangedHandler.Connect(Interface<cWindow>::Get()->OnScreenSizeChanged());
	}

	//-----------------------------------------------------------------------

	cScene::~cScene()
	{
		Log("Exiting Scene Module\n");
		Log("--------------------------------------------------------\n");

		STLDeleteAll(mlstViewports);
		STLDeleteAll(mlstWorlds);
		STLDeleteAll(mlstCameras);

		Log("--------------------------------------------------------\n\n");
	}

	//-----------------------------------------------------------------------

	void cScene::OnScreenSizeChange(const cVector2l& avSize)
	{
		if(avSize.y <= 0)
			return;

		const float fAspect = (float)avSize.x / (float)avSize.y;
		for(tCameraListIt it = mlstCameras.begin(); it != mlstCameras.end(); ++it)
		{
			cCamera *pCamera = *it;
			if(pCamera->GetProjectionType() == eProjectionType_Perspective)
				pCamera->SetAspect(fAspect);
		}
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	cViewport* cScene::CreateViewport(cCamera *apCamera, cWorld *apWorld, bool abPushFront)
	{
		cViewport *pViewport = hplNew ( cViewport, (this) );	

		pViewport->SetCamera(apCamera);
		pViewport->SetWorld(apWorld);
		pViewport->SetRenderer(mpGraphics->GetRenderer(eRenderer_Main));

		if (abPushFront) {
			mlstViewports.push_front(pViewport);
		} else {
			mlstViewports.push_back(pViewport);
		}

		return pViewport;
	}
	
	//-----------------------------------------------------------------------

	void cScene::DestroyViewport(cViewport* apViewPort)
	{
		STLFindAndDelete(mlstViewports, apViewPort);
	}

	//-----------------------------------------------------------------------

	bool cScene::ViewportExists(cViewport* apViewPort)
	{
		for(tViewportListIt it = mlstViewports.begin(); it != mlstViewports.end(); ++it)
		{
			if(apViewPort == *it) return true;
		}

		return false;
	}

	//-----------------------------------------------------------------------

	cViewport* cScene::GetPrimaryViewport()
	{
		// The viewport whose backbuffer is composited to the swapchain: the
		// single visible viewport whose Target is TargetSwapchain (the
		// CreateViewport default). TargetView viewports (editor panes /
		// previews) are standalone — delivered/sampled per their target.
		cViewport *pPrimary = NULL;
		for(cViewport *pViewPort : mlstViewports)
		{
			if(pViewPort->IsVisible()==false) continue;
			if(std::holds_alternative<cViewport::TargetSwapchain>(pViewPort->GetTarget())==false) continue;

			assert(pPrimary == NULL &&
				   "multiple visible swapchain-compositing viewports");
			pPrimary = pViewPort;
#ifdef NDEBUG
			break;
#endif
		}
		return pPrimary;
	}

	//-----------------------------------------------------------------------

	void cScene::SetCurrentListener(cViewport* apViewPort)
	{
		//If there was a previous listener make sure that world is not a listener.
		if(mpCurrentListener != NULL && ViewportExists(mpCurrentListener))
		{
			mpCurrentListener->SetIsListener(false);
			cWorld *pWorld = mpCurrentListener->GetWorld();
			if(pWorld && WorldExists(pWorld)) pWorld->SetIsSoundEmitter(false);
		}
		
		mpCurrentListener = apViewPort;
		if(mpCurrentListener)
		{
			mpCurrentListener->SetIsListener(true);
			cWorld *pWorld = mpCurrentListener->GetWorld();
			if(pWorld) pWorld->SetIsSoundEmitter(true);
		}
	}

	//-----------------------------------------------------------------------

	cCamera* cScene::CreateCamera(eCameraMoveMode aMoveMode)
	{
		cCamera *pCamera = hplNew( cCamera, () );
		pCamera->SetAspect(Interface<cWindow>::Get()->GetSizeF().x /
							Interface<cWindow>::Get()->GetSizeF().y);

		//Add Camera to list
		mlstCameras.push_back(pCamera);

		return pCamera;
	}


	//-----------------------------------------------------------------------

	void cScene::DestroyCamera(cCamera* apCam)
	{
		STLFindAndDelete(mlstCameras, apCam);
	}

	//-----------------------------------------------------------------------

	void cScene::Render(float afFrameTime, tFlag alFlags)
	{
		//Increase the frame count (do this at top, so render count is valid until this Render is called again!)
		iRenderer::IncRenderFrameCount();

		cGraphics::FrameContext* cntx = mpGraphics->GetActiveSet();

		// Publish each visible viewport's world GPU memory (light/fog/decal buffers,
		// TLAS, bindless object/material slots) ONCE per frame, before any viewport
		// renders — the data is view-independent and shared by every viewport showing
		// that world, so building it per-viewport in the loop below would be redundant
		// (and would rebuild the TLAS N times). cWorld owns + produces it; the
		// renderers only consume it.
		std::vector<cWorld*> preparedWorlds; // worlds are few (usually 1)
		for(cViewport *pViewPort : mlstViewports)
		{
			if(pViewPort->IsVisible()==false) continue;
			cWorld *pWorld = pViewPort->GetWorld();
			if(pWorld==NULL) continue;
			bool bAlready=false;
			for(cWorld *p : preparedWorlds) if(p==pWorld){ bAlready=true; break; }
			if(bAlready) continue;
			preparedWorlds.push_back(pWorld);
			pWorld->PrepareFrame(cntx);
		}

		for(cViewport *pViewPort : mlstViewports)
		{
			if(pViewPort->IsVisible()==false) continue;

			// WORLD DRAW + FEED + POST + DELIVERY — see cViewport::Evaluate.
			const bool worldRendered = pViewPort->Evaluate(cntx, afFrameTime, alFlags);

			// At most one visible viewport may target the swapchain; its GUI
			// block runs here (needs the scene's gui sets / Render3DGui).
			if(std::holds_alternative<cViewport::TargetSwapchain>(pViewPort->GetTarget()))
			{
				assert(pViewPort == GetPrimaryViewport());

				cCamera* pCamera = pViewPort->GetCamera();
				cFrustum* pFrustum = pCamera ? pCamera->GetFrustum() : NULL;

				// GUI block: open a rendering instance with color and, only when
				// GetDepthViewForExtent matches the full swapchain extent, depth;
				// render the GUIs, close it. When the world render
				// ran, the tail draw left the swapchain in COLOR_ATTACHMENT_OPTIMAL
				// with the composite inside — LOAD so the GUI overlays on top.
				// Otherwise CLEAR (also establishes UNDEFINED → COLOR transition).
				// Depth LOADs when that exact-extent view is present (the viewport's
				// renderer populated it earlier); otherwise the sets render the
				// no-depth pipeline variant.
				// The instance below covers the whole swapchain, so a depth
				// attachment that isn't swapchain-sized is illegal
				// (VUID-VkRenderingInfo-pNext-06079) — under an upscaler the
				// viewport's depth is render-extent sized. GuiSet asks the same
				// question with the same extent to pick its depth / no-depth
				// pipeline variant; Scene.cpp and GuiSet.cpp must stay keyed on this
				// same extent and predicate.
				struct RITextureView *pGuiDepthView =
					pViewPort->GetDepthViewForExtent(mpGraphics->swapchain->width,
													 mpGraphics->swapchain->height);

				// Color LOADs the world composite when present, else CLEARs (also
				// establishes the UNDEFINED → COLOR transition).
				RIRenderingAttachment color = {};
				color.view = *mpGraphics->swapchain->textureView(mpGraphics->swapchainIndex);
				color.loadOp = worldRendered ? RI_ATTACHMENT_LOAD_OP_LOAD
											  : RI_ATTACHMENT_LOAD_OP_CLEAR;
				color.storeOp = RI_ATTACHMENT_STORE_OP_STORE;

				RIRenderingAttachment depth = {};
				if(pGuiDepthView)
				{
					depth.view = *pGuiDepthView;
					depth.loadOp = RI_ATTACHMENT_LOAD_OP_LOAD;
					depth.storeOp = RI_ATTACHMENT_STORE_OP_STORE;
				}

				RIBeginRenderingDesc beginDesc = {};
				beginDesc.renderArea.width = (int16_t)mpGraphics->swapchain->width;
				beginDesc.renderArea.height = (int16_t)mpGraphics->swapchain->height;
				beginDesc.colorCount = 1;
				beginDesc.colors = &color;
				beginDesc.depthStencil = pGuiDepthView ? &depth : NULL;

				// GuiSet builds its pipelines for Interface<cGraphics>::Get()->swapchain.format / cGraphics::DepthFormat
				// (see GuiSet.cpp). If the attachments here ever change, update GuiSet to match.
				mpGraphics->primary.cmds[0].vk_d3d12_beginRendering( &mpGraphics->device, beginDesc );

				if(alFlags & tSceneRenderFlag_World)
				{
					START_TIMING(Render3DGui)
					Render3DGui(pViewPort, pFrustum, afFrameTime);
					STOP_TIMING(Render3DGui)
				}
				if(alFlags & tSceneRenderFlag_Gui)
				{
					START_TIMING(RenderGUI)
					RenderScreenGui(pViewPort, afFrameTime);
					STOP_TIMING(RenderGUI)
				}

				mpGraphics->primary.cmds[0].vk_d3d12_endRendering( &mpGraphics->device );
			}
		}
	}

	//-----------------------------------------------------------------------

	void cScene::PostUpdate(float afTimeStep)
	{
		//////////////////////////////////////
		//Update worlds
		tWorldListIt it = mlstWorlds.begin();
		for(; it != mlstWorlds.end(); ++it)
		{
			cWorld *pWorld = *it;
            if(pWorld->IsActive()) pWorld->Update(afTimeStep);
		}


		//////////////////////////////////////
		//Update listener position with current listener, if there is one.
		if(mpCurrentListener && mpCurrentListener->GetCamera())
		{
			cCamera* pCamera3D = mpCurrentListener->GetCamera();
			mpSound->GetLowLevel()->SetListenerAttributes(	pCamera3D->GetPosition(), cVector3f(0,0,0),
															pCamera3D->GetForward()*-1.0f, pCamera3D->GetUp());
		}
	}

	//-----------------------------------------------------------------------

	void cScene::Reset()
	{
	}

	//-----------------------------------------------------------------------

	cWorld* cScene::LoadWorld(const tString& asFile, tWorldLoadFlag aFlags)
	{
		///////////////////////////////////
		// Load the map file
		tWString asPath = mpResources->GetFileSearcher()->GetFilePath(asFile);
		if(asPath == _W(""))
		{
			if(cResources::GetCreateAndLoadCompressedMaps())
				asPath = mpResources->GetFileSearcher()->GetFilePath(cString::SetFileExt(asFile,"cmap"));
			
			if(asPath == _W(""))
			{
				Error("World '%s' doesn't exist\n",asFile.c_str());
				return NULL;
			}
		}

		cWorld* pWorld = mpResources->GetWorldLoaderHandler()->LoadWorld(asPath, aFlags);
		if(pWorld==NULL){
			Error("Couldn't load world from '%s'\n",cString::To8Char(asPath).c_str());
			return NULL;
		}

		return pWorld;
	}

	//-----------------------------------------------------------------------

	cWorld* cScene::CreateWorld(const tString& asName)
	{
		cWorld* pWorld = hplNew( cWorld, (asName,mpGraphics,mpResources,mpSound,mpPhysics,this,
										mpSystem,mpAI,mpHaptic) );

		mlstWorlds.push_back(pWorld);

		pWorld->SetRendererBackend(mpGraphics->GetRendererBackend());

		return pWorld;
	}

	//-----------------------------------------------------------------------

	void cScene::DestroyWorld(cWorld* apWorld)
	{
		STLFindAndDelete(mlstWorlds,apWorld);
	}

	//-----------------------------------------------------------------------

	bool cScene::WorldExists(cWorld* apWorld)
	{
		for(tWorldListIt it = mlstWorlds.begin(); it != mlstWorlds.end(); ++it)
		{
			if(apWorld == *it) return true;
		}

		return false;
	}
	
	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cScene::Render3DGui(cViewport *apViewPort,cFrustum *apFrustum,float afTimeStep)
	{
		if(apViewPort->GetCamera()==NULL) return;

		for(cGuiSet *pSet : apViewPort->GetGuiSets())
		{
			if(pSet->Is3D())
			{
				pSet->Render(apFrustum, apViewPort);
			}
		}
	}
	
	void cScene::RenderScreenGui(cViewport *apViewPort,float afTimeStep)
	{
		///////////////////////////////////////
		//Put all of the non 3D sets in to a sorted map
		typedef std::multimap<int, cGuiSet*> tPrioMap;
		tPrioMap mapSortedSets;

		for(cGuiSet *pSet : apViewPort->GetGuiSets())
		{
			if(pSet->Is3D()==false)
				mapSortedSets.insert(tPrioMap::value_type(pSet->GetDrawPriority(),pSet));
		}

		///////////////////////////////////////
		//Iterate and render all sets
		if(mapSortedSets.empty()) return;
		tPrioMap::iterator SortIt = mapSortedSets.begin();
		for(; SortIt != mapSortedSets.end(); ++SortIt)
		{
			cGuiSet *pSet = SortIt->second;
			
			//Log("Rendering gui '%s'\n", pSet->GetName().c_str());

			pSet->Render(NULL, apViewPort);
		}
	}

	//-----------------------------------------------------------------------
}
