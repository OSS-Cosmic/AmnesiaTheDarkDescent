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

#ifndef HPL_ENGINE_INIT_VARS_H
#define HPL_ENGINE_INIT_VARS_H

#include "system/SystemTypes.h"
#include "math/MathTypes.h"
#include "graphics/GraphicsTypes.h"

namespace hpl {

	//---------------------------------------
	
    class cEngineInitVars
	{
	public:
		////////////////////////////////
		// Game
		class cEngineVars
		{
		public:
			cEngineVars() :
				mlUpdateRate(60)
			  {}

			  int mlUpdateRate;
		};
		cEngineVars mGame;

		////////////////////////////////
		// Graphics
		class cGraphicsVars
		{
		public:
			cGraphicsVars() :
				mvScreenSize(800,600),
				mlDisplay(0),
				mlScreenBpp(32),
				mbFullscreen(false),
				mbVsync(false),
				msWindowCaption(""),
				mvWindowPosition(-1),
				mRendererBackend(eRendererBackend_RayTraced),
				mRenderApi(eRenderApiPreference_Auto),
				mbAllowRuntimeBackendSwitch(false)
			{}

			cVector2l mvScreenSize;
			int mlDisplay;
			int mlScreenBpp;
			bool mbFullscreen;
			bool mbVsync;
			tString msWindowCaption;
			cVector2l mvWindowPosition;
			eRendererBackend mRendererBackend;
			// Which RHI to bring up. Auto takes the platform default (D3D12 on
			// Windows, Vulkan elsewhere); the game overrides it from --d3d12 /
			// --vulkan on the command line.
			eRenderApiPreference mRenderApi;
			// Editors only: bring the device up ray-tracing-capable whenever
			// the adapter allows it and build both lit renderers, so the
			// backend can be switched from the viewport menu without a
			// restart. The game leaves this false.
			bool mbAllowRuntimeBackendSwitch;
		};
		cGraphicsVars mGraphics;
		
		////////////////////////////////
		// Sound
		class cSoundVars
		{
		public:
			cSoundVars() :
				mlSoundDeviceID(-1),
				mbUseEnvironmentalAudio(false),
				mbUseHRTF(false),
				mbUseThreading(true),
				mbUseVoiceManagement(true),
				mbLowLevelLogging(false),
				mlStreamUpdateFreq(10),
				mlMaxChannels(32),
				mlMaxMonoChannelsHint(0),
				mlMaxStereoChannelsHint(0),
				mlStreamBufferSize(524288),
				mlStreamBufferCount(2)
			{}
				
			int	mlSoundDeviceID;
			bool mbUseEnvironmentalAudio;
			bool mbUseHRTF;
			bool mbUseThreading;
			bool mbUseVoiceManagement;
			bool mbLowLevelLogging;
			int mlStreamUpdateFreq;
			int mlMaxChannels;
			int mlMaxMonoChannelsHint;
			int mlMaxStereoChannelsHint;
			int mlStreamBufferSize;
			int mlStreamBufferCount;
		};
		cSoundVars mSound;			

	};

	//---------------------------------------

};

#endif // HPL_ENGINE_INIT_VARS_H
