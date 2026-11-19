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

#include "impl/MouseSDL.h"

#if USE_SDL2
#include "SDL2/SDL.h"
#else
#include "SDL/SDL.h"
#endif

#include "impl/LowLevelInputSDL.h"
#include "math/Math.h"

#include "system/LowLevelSystem.h"
#include "system/Platform.h"

namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cMouseSDL::cMouseSDL(cLowLevelInputSDL *apLowLevelInputSDL) : iMouse("SDL Portable Mouse")
	{
		mvMButtonArray.resize(eMouseButton_LastEnum);
		mvMButtonArray.assign(mvMButtonArray.size(),false);

		mpLowLevelInputSDL = apLowLevelInputSDL;

		mvMouseRelPos = cVector2l(0,0);
		mvMouseAbsPos = cVector2l(0,0);

		mvTickRelAccum = cVector2l(0,0);
		mvFrameRelAccum = cVector2l(0,0);
		mbFrameRelRead = false;

		mbWheelUpMoved = false;
		mbWheelDownMoved = false;

		mbFirstTime = true;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	void cMouseSDL::Update()
	{
		///////////////////////////////////////
		//On first update make sure to clear all previous events.
		if(mbFirstTime)
		{
			//Clear all relative movement
			for(int i=0; i<10; ++i)
			{
				SDL_PumpEvents();
				int lX,lY;
				SDL_GetRelativeMouseState(&lX, &lY);
			}
			mbFirstTime = false;
		}
		
		//mvMouseRelPos = cVector2f(0,0);
		mbWheelUpMoved = false;
		mbWheelDownMoved = false;

		std::list<SDL_Event>::iterator it = mpLowLevelInputSDL->mlstEvents.begin();
		for(; it != mpLowLevelInputSDL->mlstEvents.end(); ++it)
		{
			SDL_Event *pEvent = &(*it);

			if(	pEvent->type != SDL_MOUSEMOTION && 
				pEvent->type != SDL_MOUSEBUTTONDOWN &&
#if SDL_VERSION_ATLEAST(2, 0, 0)
                pEvent->type != SDL_MOUSEWHEEL &&
#endif
				pEvent->type != SDL_MOUSEBUTTONUP)
			{
				continue;
			}

			if(pEvent->type == SDL_MOUSEMOTION)
			{
				mvMouseAbsPos = cVector2l(pEvent->motion.x,pEvent->motion.y);


				Uint8 buttonState = pEvent->motion.state;

				//Set button here as well just to be sure
				/*if(buttonState & SDL_BUTTON(1)) mvMButtonArray[eMouseButton_Left] = true;
				if(buttonState & SDL_BUTTON(2)) mvMButtonArray[eMouseButton_Middle] = true;
				if(buttonState & SDL_BUTTON(3)) mvMButtonArray[eMouseButton_Right] = true;*/
			}
#if SDL_VERSION_ATLEAST(2, 0, 0)
            else if(pEvent->type == SDL_MOUSEWHEEL)
            {
                if (pEvent->wheel.y > 0) {
                    mvMButtonArray[eMouseButton_WheelUp] = true;
                    mbWheelUpMoved = true;
                } else {
                    mvMButtonArray[eMouseButton_WheelDown] = true;
                    mbWheelDownMoved = true;
                }
                break;
            }
#endif
			else
			{
				bool bButtonIsDown = pEvent->type==SDL_MOUSEBUTTONDOWN;

				//if(pEvent->button.button == SDL_BUTTON_WHEELUP)Log(" Wheel %d!\n",bButtonIsDown);

				switch(pEvent->button.button)
				{
					case SDL_BUTTON_LEFT: mvMButtonArray[eMouseButton_Left] = bButtonIsDown;break;
					case SDL_BUTTON_MIDDLE: mvMButtonArray[eMouseButton_Middle] = bButtonIsDown;break;
					case SDL_BUTTON_RIGHT: mvMButtonArray[eMouseButton_Right] = bButtonIsDown;break;
					case SDL_BUTTON_X1: mvMButtonArray[eMouseButton_Button6] = bButtonIsDown;break;
					case SDL_BUTTON_X2: mvMButtonArray[eMouseButton_Button7] = bButtonIsDown;break;
#if !SDL_VERSION_ATLEAST(2, 0, 0)
					case SDL_BUTTON_WHEELUP: 
						mvMButtonArray[eMouseButton_WheelUp] = bButtonIsDown;
						if(bButtonIsDown) mbWheelUpMoved = true;
						break;
					case SDL_BUTTON_WHEELDOWN: 
						mvMButtonArray[eMouseButton_WheelDown] = bButtonIsDown;
						if(bButtonIsDown) mbWheelDownMoved = true;
						break;
#endif
				}
			}
		}

		if(mbWheelDownMoved)	mvMButtonArray[eMouseButton_WheelDown] = true;
		else					mvMButtonArray[eMouseButton_WheelDown] = false;
		if(mbWheelUpMoved)		mvMButtonArray[eMouseButton_WheelUp] = true;
		else					mvMButtonArray[eMouseButton_WheelUp] = false;
		
		int lX,lY; 
		SDL_GetRelativeMouseState(&lX, &lY);
		cVector2l vRel(lX,lY);

		//Movement since the previous tick (including what FrameUpdate sampled in between)
		mvMouseRelPos = mvTickRelAccum + vRel;
		mvTickRelAccum = cVector2l(0,0);

		mvFrameRelAccum += vRel;
	}

	//-----------------------------------------------------------------------

	void cMouseSDL::FrameUpdate()
	{
		//Let the first tick update flush old movement first
		if(mbFirstTime) return;

		//Nobody used the movement last frame: drop it so it does not pile up (e.g. while in a menu).
		if(mbFrameRelRead == false) mvFrameRelAccum = cVector2l(0,0);
		mbFrameRelRead = false;

		//Update SDL's state without taking any events from the queue (the engine pump handles those).
		SDL_PumpEvents();

		int lX,lY;
		SDL_GetRelativeMouseState(&lX, &lY);
		cVector2l vRel(lX,lY);

		mvTickRelAccum += vRel;
		mvFrameRelAccum += vRel;

		//Newest absolute position (for cursors)
		int lAbsX,lAbsY;
		SDL_GetMouseState(&lAbsX, &lAbsY);
		mvMouseAbsPos = cVector2l(lAbsX,lAbsY);
	}

	//-----------------------------------------------------------------------

	cVector2l cMouseSDL::GetFrameRelPosition()
	{
		cVector2l vPos = mvFrameRelAccum;
		mvFrameRelAccum = cVector2l(0,0);
		mbFrameRelRead = true;
		return vPos;
	}
	
	//-----------------------------------------------------------------------
	
	bool cMouseSDL::ButtonIsDown(eMouseButton mButton)
	{
		return mvMButtonArray[mButton];
	}

	//-----------------------------------------------------------------------

	cVector2l cMouseSDL::GetAbsPosition()
	{
		return mvMouseAbsPos;
	}
	
	//-----------------------------------------------------------------------

	cVector2l cMouseSDL::GetRelPosition()
	{
		cVector2l vPos = mvMouseRelPos;
		mvMouseRelPos = cVector2l(0,0);
		
		return vPos;
	}

	//-----------------------------------------------------------------------
	
	/////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	/////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	//-----------------------------------------------------------------------

}
