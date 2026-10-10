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

#include "scene/AnimationState.h"

#include "math/Math.h"
#include "graphics/Animation.h"
#include "resources/AnimationManager.h"

#include "system/LowLevelSystem.h"


namespace hpl {

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------
	
	cAnimationState::cAnimationState(cAnimation* apAnimation, const tString &asName,
									cAnimationManager *apAnimationManager)
	{
		mpAnimation = apAnimation;

		mpAnimationManager = apAnimationManager;

		mfLength = mpAnimation->GetLength();
		msName = asName;

		mbActive = false;

		mfTimePos = 0;
		mfWeight = 1;
		mfSpeed = 1.0f;
		mfBaseSpeed = 1.0f;
		mfTimePos = 0;
		mfPrevTimePos=0;

		mbLoop =false;
		mbPaused = false;

		mfSpecialEventTime =0;

		mfFadeStep=0;
		mfFadeSpeed=0;

		mbCanBlend = true;
	}

	//-----------------------------------------------------------------------

	cAnimationState::~cAnimationState()
	{
		STLDeleteAll(mvEvents);

		//if no animation manger, then it means that the animation should not be destroyed by the state.
		if(mpAnimationManager)
		{
			mpAnimationManager->Destroy(mpAnimation);
		}
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cAnimationState::Update(float afTimeStep)
	{
		//Update animation
		AddTimePosition(afTimeStep);

		//Fading
		if(mfFadeStep!=0)
		{
			mfWeight += mfFadeStep*afTimeStep;
			
			if(mfFadeStep<0 && mfWeight<=0)
			{
				mfWeight =0;
				mbActive = false;
				mfFadeStep =0;
			}
			else if(mfFadeStep>0 && mfWeight>=1)
			{
				mfWeight =1;
				mfFadeStep =0;
			}
		}

		//Speed fading (only set by FadeInSpeed/FadeOutSpeed)
		if(mfFadeSpeed!=0)
		{
			mfSpeed += mfFadeSpeed * afTimeStep;

			if(mfSpeed<0)
			{
				mfSpeed =0;
				mfFadeSpeed =0;
			}
			else if(mfSpeed>1.0f)
			{
				mfSpeed =1;
				mfFadeSpeed =0;
			}
		}
	}

	//-----------------------------------------------------------------------

	bool cAnimationState::IsFading()
	{
		return mfFadeStep!=0;
	}

	
	//-----------------------------------------------------------------------
	
	bool cAnimationState::IsOver()
	{
		if(mbLoop) return false;

		return mfTimePos >= mfLength;
	}

	//-----------------------------------------------------------------------

	void cAnimationState::FadeIn(float afTime)
	{
		mfFadeStep = 1.0f / std::abs(afTime);
	}

	void cAnimationState::FadeOut(float afTime)
	{
		mfFadeStep = -1.0f / std::abs(afTime);
	}

	//-----------------------------------------------------------------------

	void cAnimationState::FadeInSpeed(float afTime)
	{
		if(afTime == 0.0f)
		{
			mfSpeed = 1;
			mfFadeSpeed = 0;
		}
		else
		{
			mfFadeSpeed = 1.0f / std::abs(afTime);
		}
	}

	void cAnimationState::FadeOutSpeed(float afTime)
	{
		if(afTime == 0.0f)
		{
			mfSpeed = 0;
			mfFadeSpeed = 0;
		}
		else
		{
			mfFadeSpeed = -1.0f / std::abs(afTime);
		}
	}
	
	//-----------------------------------------------------------------------
	
	void cAnimationState::SetLength(float afLength)
	{
		mfLength = afLength;
	}
	
	float cAnimationState::GetLength()
	{
		return mfLength;
	}

	//-----------------------------------------------------------------------


	void cAnimationState::SetWeight(float afWeight)
	{
		mfWeight = afWeight;
	}
	float cAnimationState::GetWeight()
	{
		return mfWeight;
	}

	//-----------------------------------------------------------------------

	void cAnimationState::SetSpeed(float afSpeed)
	{
		mfSpeed = afSpeed;
	}
	float cAnimationState::GetSpeed()
	{
		return mfSpeed;
	}
	
	//-----------------------------------------------------------------------

	void cAnimationState::SetBaseSpeed(float afSpeed)
	{
		mfBaseSpeed = afSpeed;
	}
	float cAnimationState::GetBaseSpeed()
	{
		return mfBaseSpeed;
	}

	//-----------------------------------------------------------------------

	void cAnimationState::SetTimePosition(float afPosition)
	{
		if(mbLoop)
		{
			mfTimePos = cMath::Wrap(afPosition,0,mfLength);
		}
		else
		{
			mfTimePos = cMath::Clamp(afPosition, 0, mfLength);
		}
#ifdef AMFP
		// A jump must not make the event check fire every event in between.
		mfPrevTimePos = mfTimePos;
#endif
	}

	float cAnimationState::GetTimePosition()
	{
		return mfTimePos;
	}

	float cAnimationState::GetPreviousTimePosition()
	{
		return mfPrevTimePos;
	}

	//-----------------------------------------------------------------------

	void cAnimationState::SetRelativeTimePosition(float afPosition)
	{
		SetTimePosition(afPosition * mfLength);
	}

	float cAnimationState::GetRelativeTimePosition()
	{
		return mfTimePos / mfLength;
	}
	
	//-----------------------------------------------------------------------

	bool cAnimationState::IsActive()
	{
		return mbActive;
	}
	void cAnimationState::SetActive(bool abActive)
	{
		if(mbActive == abActive) return;
		
		mbActive = abActive;

		//Should this really be here?
		mbPaused = false;
		mfFadeStep =0;
	}

	//-----------------------------------------------------------------------
	
	bool cAnimationState::IsLooping()
	{
		return mbLoop;
	}
	void cAnimationState::SetLoop(bool abLoop)
	{
		mbLoop = abLoop;
	}

	//-----------------------------------------------------------------------

	bool cAnimationState::IsPaused()
	{	
		return mbPaused;
	}
	
	void cAnimationState::SetPaused(bool abPaused)
	{
		mbPaused = abPaused;
	}

	//-----------------------------------------------------------------------

	bool cAnimationState::IsAfterSpecialEvent()
	{
		return mfTimePos > mfSpecialEventTime;
	}
	
	bool cAnimationState::IsBeforeSpecialEvent()
	{
		return mfTimePos < mfSpecialEventTime;	
	}

	//-----------------------------------------------------------------------

	void cAnimationState::AddTimePosition(float afAdd)
	{
		if(mbPaused) return;
		
		mfPrevTimePos = mfTimePos;

		mfTimePos += afAdd*mfSpeed*mfBaseSpeed;

#ifdef AMFP
		// SetTimePosition would overwrite mfPrevTimePos.
		if(mbLoop)
			mfTimePos = cMath::Wrap(mfTimePos,0,mfLength);
		else
			mfTimePos = cMath::Clamp(mfTimePos, 0, mfLength);
#else
		SetTimePosition(mfTimePos);
#endif
	}
	
	//-----------------------------------------------------------------------

	cAnimation* cAnimationState::GetAnimation()
	{
		return mpAnimation;
	}

	//-----------------------------------------------------------------------

	cAnimationEvent *cAnimationState::CreateEvent()
	{
		cAnimationEvent *pEvent = hplNew( cAnimationEvent, () );
		pEvent->mfTime =0;
		pEvent->mType = eAnimationEventType_LastEnum;
		pEvent->msValue = "";
		mvEvents.push_back(pEvent);

		return pEvent;
	}

	cAnimationEvent *cAnimationState::GetEvent(int alIdx)
	{
		return mvEvents[alIdx];
	}
	
	int cAnimationState::GetEventNum()
	{
		return (int)mvEvents.size();
	}

	//-----------------------------------------------------------------------

	void cAnimationState::AddTransition(int alAnimId, int alPreviousAnimId, float afMinTime, float afMaxTime)
	{
		mvTransitions.push_back(cAnimationTransition(alAnimId,alPreviousAnimId, afMinTime, afMaxTime));
	}

	cAnimationTransition* cAnimationState::GetTransitionFromPrevAnim(int alPreviousAnimId, float afPreviousTimePos)
	{
		cAnimationTransition *pTransOut = NULL;

		for(size_t i=0; i<mvTransitions.size(); ++i)
		{
			cAnimationTransition &trans = mvTransitions[i];

			if(trans.mlPreviousAnimId == alPreviousAnimId || trans.mlPreviousAnimId<0)
			{
				//Check if within time limits
				if(trans.mfMinTime>=0 && trans.mfMaxTime>=0 && afPreviousTimePos>=0)
				{
					if(afPreviousTimePos < trans.mfMinTime || afPreviousTimePos > trans.mfMaxTime) continue;
				}

				pTransOut = &trans;
				if(trans.mlPreviousAnimId>=0 || alPreviousAnimId<0) break; //if not a default transition, we know we got the right one.
			}
		}

		return pTransOut;
	}

	cAnimationTransition* cAnimationState::GetTransition(int alIdx)
	{
		return &mvTransitions[alIdx];
	}

	int cAnimationState::GetTransitionNum()
	{
		return (int)mvTransitions.size();
	}

	//-----------------------------------------------------------------------
}
