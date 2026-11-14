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

#ifndef LUX_SCRIPT_HANDLER_H
#define LUX_SCRIPT_HANDLER_H

//----------------------------------------------

#include "LuxBase.h"

//----------------------------------------------

class cLuxScriptHandler : public iLuxUpdateable
{
public:	
	cLuxScriptHandler();
	~cLuxScriptHandler();

	void OnStart();
	void Update(float afTimeStep);
	void Reset();

	void OnDraw(float afFrameTime);

private:
	iLowLevelSystem *mpLowLevelSystem;

	//Temp:
	static int mlRopeIdCount;

	//////////////////////////////////
	// Script functions

	// Helpers
	void InitScriptFunctions();
	void AddFunc(const tString& asFunc, void *apFuncPtr);

	static bool GetEntities(const tString& asName,tLuxEntityList &alstEntities, eLuxEntityType aType, int alSubType);
	static iLuxEntity* GetEntity(const tString& asName, eLuxEntityType aType, int alSubType);
	static iPhysicsBody* GetBodyInEntity(iLuxEntity* apEntity, const tString& asName);

	// Debug
	static void __stdcall Print(std::string& asString);
	static void __stdcall AddDebugMessage(std::string& asString, bool abCheckForDuplicates);
	/**
	 * Type can be "Low", "Medium" or "High"
	 */
	static void __stdcall ProgLog(std::string& asLevel, std::string& asMessage);
	static bool __stdcall ScriptDebugOn();


	// General
	static float __stdcall RandFloat(float afMin, float afMax);
	static int __stdcall RandInt(int alMin, int alMax);

	static bool __stdcall StringContains(std::string& asString, std::string& asSubString);
	static std::string& __stdcall StringSub(std::string& asString, int alStart, int alCount);

	//Function syntax: Func(string &in asTimer)
	static void __stdcall AddTimer(std::string& asName, float afTime, std::string& asFunction);
	static void __stdcall RemoveTimer(std::string& asName);
	static float __stdcall GetTimerTimeLeft(std::string& asName);

	static void __stdcall SetLocalVarInt(std::string& asName, int alVal);
	static void __stdcall SetLocalVarFloat(std::string& asName, float afVal);
	static void __stdcall SetLocalVarString(std::string& asName, const std::string& asVal);
	
	static void __stdcall AddLocalVarInt(std::string& asName, int alVal);
	static void __stdcall AddLocalVarFloat(std::string& asName, float afVal);
	static void __stdcall AddLocalVarString(std::string& asName, std::string& asVal);
	
	static int __stdcall GetLocalVarInt(std::string& asName);
	static float __stdcall GetLocalVarFloat(std::string& asName);
	static std::string& __stdcall GetLocalVarString(std::string& asName);

	static void __stdcall SetGlobalVarInt(std::string& asName, int alVal);
	static void __stdcall SetGlobalVarFloat(std::string& asName, float afVal);
	static void __stdcall SetGlobalVarString(std::string& asName, const std::string& asVal);

	static void __stdcall AddGlobalVarInt(std::string& asName, int alVal);
	static void __stdcall AddGlobalVarFloat(std::string& asName, float afVal);
	static void __stdcall AddGlobalVarString(std::string& asName, std::string& asVal);

	static int __stdcall GetGlobalVarInt(std::string& asName);
	static float __stdcall GetGlobalVarFloat(std::string& asName);
	static std::string& __stdcall GetGlobalVarString(std::string& asName);

	// Game
	static void __stdcall StartCredits(std::string& asMusic, bool abLoopMusic, std::string& asTextCat, std::string& asTextEntry, int alEndNum);
	static void __stdcall AddKeyPart(int alKeyPart);

	static void __stdcall StartDemoEnd();

	// Saving
	static void __stdcall AutoSave();
	/**
	 * Callback syntax: MyFunc(string &in asName, int alCount), Count is 0 on first checkpoint load!
	 */
	static void __stdcall CheckPoint(std::string& asName,std::string& asStartPos ,std::string& asCallback, std::string &asDeathHintCat, std::string &asDeathHintEntry);

	// Map
	static void __stdcall ChangeMap(std::string& asMapName, std::string& asStartPos, std::string& asStartSound, std::string& asEndSound);
	static void __stdcall ClearSavedMaps();
	
	/**
	 * This caches all current textures and models and they are not released until destroy is called. If there is already cached data it is destroyed.
	 */
	static void __stdcall CreateDataCache();
	static void __stdcall DestroyDataCache();

	/**
	 * Looked up in lang as ("Levels", asNameEntry)
	 */
	static void __stdcall SetMapDisplayNameEntry(std::string& asNameEntry);

	static void __stdcall SetSkyBoxActive(bool abActive);
	static void __stdcall SetSkyBoxTexture(std::string& asTexture);
	static void __stdcall SetSkyBoxColor(float afR, float afG, float afB, float afA);

	static void __stdcall SetFogActive(bool abActive);
	static void __stdcall SetFogColor(float afR, float afG, float afB, float afA);
	static void __stdcall SetFogProperties(float afStart, float afEnd, float afFalloffExp, bool abCulling);
	
	/**
	 * If alRandomNum > 1, then it will randomize between 1 and alRandom for each LoadScreen giving entry the suffix XX (eg 01). If <=1 then no suffix is added
	 */
	static void __stdcall SetupLoadScreen(std::string &asTextCat, std::string &asTextEntry, int alRandomNum, std::string &asImageFile);
	
	
	// Effect
	static void __stdcall FadeIn(float afTime);
	static void __stdcall FadeOut(float afTime);
	static void __stdcall FadeImageTrailTo(float afAmount, float afSpeed);
	static void __stdcall FadeSepiaColorTo(float afAmount, float afSpeed);
	static void __stdcall FadeRadialBlurTo(float afSize, float afSpeed);
	static void __stdcall SetRadialBlurStartDist(float afStartDist);
	static void __stdcall StartEffectFlash(float afFadeIn, float afWhite, float afFadeOut);
	static void __stdcall StartEffectEmotionFlash(std::string &asTextCat, std::string &asTextEntry, std::string &asSound);
	static void __stdcall SetInDarknessEffectsActive(bool abX);
	
	//This adds a voice + effect to be played. It is okay to call this many times in order to play many voices in a row. The EffectVoiceOverCallback is not called until ALL voices have finished.
	static void __stdcall AddEffectVoice(	std::string& asVoiceFile, std::string& asEffectFile,
											std::string& asTextCat, std::string& asTextEntry, bool abUsePostion, 
											std::string& asPosEnitity, float afMinDistance, float afMaxDistance);
	static void __stdcall StopAllEffectVoices(float afFadeOutTime);
	static bool __stdcall GetEffectVoiceActive();
	/**
	* Syntax: void Func();
	*/
	static void __stdcall SetEffectVoiceOverCallback(std::string& asFunc);
	static bool __stdcall GetFlashbackIsActive();
		
	static void __stdcall StartPlayerSpawnPS(std::string& asSPSFile);
	static void __stdcall StopPlayerSpawnPS();

	static void __stdcall PlayGuiSound(std::string& asSoundFile, float afVolume);

	static void __stdcall StartScreenShake(float afAmount, float afTime, float afFadeInTime,float afFadeOutTime);
	

	// Insanity
	static void __stdcall SetInsanitySetEnabled(std::string& asSet, bool abX);
	static void __stdcall StartRandomInsanityEvent();
	static void __stdcall StartInsanityEvent(std::string& asEventName);
	static void __stdcall StopCurrentInsanityEvent();
	static bool __stdcall InsanityEventIsActive();
	

	static void __stdcall UnlockAchievement(std::string& asName);

	// Player
	static void __stdcall SetPlayerActive(bool abActive);
	static void __stdcall ChangePlayerStateToNormal();
	static void __stdcall SetPlayerCrouching(bool abCrouch);
	static void __stdcall AddPlayerBodyForce(float afX, float afY, float afZ, bool abUseLocalCoords);
	static void __stdcall ShowPlayerCrossHairIcons(bool abX);
	
	static void __stdcall SetPlayerPos(float afX, float afY, float afZ);
	static float __stdcall GetPlayerPosX();
	static float __stdcall GetPlayerPosY();
	static float __stdcall GetPlayerPosZ();
	
	static void __stdcall SetPlayerSanity(float afSanity);
	static void __stdcall AddPlayerSanity(float afSanity);
	static float __stdcall GetPlayerSanity();
	static float __stdcall GetPlayerLightLevel();
	static bool __stdcall GetPlayerInDarkness();
	static void __stdcall SetPlayerHealth(float afHealth);
	static void __stdcall AddPlayerHealth(float afHealth);
	static float __stdcall GetPlayerHealth();
	static void __stdcall SetPlayerLampOil(float afOil);
	static void __stdcall AddPlayerLampOil(float afOil);
	static float __stdcall GetPlayerLampOil();

	static float __stdcall GetPlayerSpeed();
	static float __stdcall GetPlayerYSpeed();

	static float __stdcall GetPlayerBodyPitch();
	static float __stdcall GetPlayerBodyYaw();
	static float __stdcall GetPlayerCameraPitch();
	static float __stdcall GetPlayerCameraYaw();

	static void __stdcall MovePlayerForward(float afAmount);

	static void __stdcall SetPlayerPermaDeathSound(std::string& asSound);

	static void __stdcall SetSanityDrainDisabled(bool abX);
	static void __stdcall GiveSanityBoost();
	static void __stdcall GiveSanityBoostSmall();
	static void __stdcall GiveSanityDamage(float afAmount, bool abUseEffect);
	/**
	 * Type can be: BloodSplat, Claws or Slash.
	 */
	static void __stdcall GivePlayerDamage(float afAmount, std::string& asType, bool abSpinHead, bool abLethal);
	static void __stdcall FadePlayerFOVMulTo(float afX, float afSpeed);
	static void __stdcall FadePlayerAspectMulTo(float afX, float afSpeed);
	static void __stdcall FadePlayerRollTo(float afX, float afSpeedMul, float afMaxSpeed);
	static void __stdcall MovePlayerHeadPos(float afX, float afY, float afZ, float afSpeed, float afSlowDownDist);
	static void __stdcall StartPlayerLookAt(std::string& asEntityName, float afSpeedMul, float afMaxSpeed, std::string &asAtTargetCallback);
	static void __stdcall StopPlayerLookAt();
	static void __stdcall SetPlayerMoveSpeedMul(float afMul);
	static void __stdcall SetPlayerRunSpeedMul(float afMul);
	static void __stdcall SetPlayerLookSpeedMul(float afMul);
	static void __stdcall SetPlayerJumpForceMul(float afMul);
	static void __stdcall SetPlayerJumpDisabled(bool abX);
	static void __stdcall SetPlayerCrouchDisabled(bool abX);
	static void __stdcall TeleportPlayer(std::string &asStartPosName);
	static void __stdcall SetLanternActive(bool abX, bool abUseEffects);
	static bool __stdcall GetLanternActive();
	static void __stdcall SetLanternDisabled(bool abX);
	static void __stdcall SetPlayerFallDamageDisabled(bool abX);
	/**
	 * Syntax: MyFun(bool abLit)
	 */
	static void __stdcall SetLanternLitCallback(std::string &asCallback);
	/**
	* if time is <=0 then the life time is calculated based on string length.
	*/
	static void __stdcall SetMessage(std::string &asTextCategory, std::string &asTextEntry, float afTime);
	static void __stdcall SetDeathHint(std::string &asTextCategory, std::string &asTextEntry);
	/**
	 * This must be called directly before player is killed! The variable as soon as player dies too.
	 */
	static void __stdcall DisableDeathStartSound();


	// Journal
	static void __stdcall AddNote(std::string& asNameAndTextEntry, std::string& asImage);
	static void __stdcall AddDiary(std::string& asNameAndTextEntry, std::string& asImage);
	/**
	 * Only called in the pickup diary callback! If true the journal displays the entry else not.
	 */
	static void __stdcall ReturnOpenJournal(bool abOpenJournal);
	

	// Quest
	static void __stdcall AddQuest(std::string& asName, std::string& asNameAndTextEntry);
	static void __stdcall CompleteQuest(std::string& asName, std::string& asNameAndTextEntry);
	static bool __stdcall QuestIsCompleted(std::string& asName);
	static bool __stdcall QuestIsAdded(std::string& asName);
	static void __stdcall SetNumberOfQuestsInMap(int alNumberOfQuests);

	// Hint
	/**
	* if time is <=0 then the life time is calculated based on string length.
	*/
	static void __stdcall GiveHint(std::string& asName, std::string& asMessageCat, std::string& asMessageEntry, float afTimeShown);
	static void __stdcall RemoveHint(std::string& asName);
	static void __stdcall BlockHint(std::string& asName);
	static void __stdcall UnBlockHint(std::string& asName);
	
	// Inventory
	static void __stdcall ExitInventory();
	static void __stdcall SetInventoryDisabled(bool abX);
	/**
	* if life time is <=0 then the life time is calculated based on string length.
	*/
	static void __stdcall SetInventoryMessage(std::string &asTextCategory, std::string &asTextEntry, float afTime);
	
	static void __stdcall GiveItem(std::string& asName, std::string& asType, std::string& asSubTypeName, std::string& asImageName, float afAmount);
	static void __stdcall RemoveItem(std::string& asName);
	static bool __stdcall HasItem(std::string& asName);

	//This is meant to be used for debug mostly as it creates the actual item and then destroys i.
	static void __stdcall GiveItemFromFile(std::string& asName, std::string& asFileName);
	
	/**
	* Callback syntax: MyFunc(string &in asItemA, string &in asItemB)
	*/
	static void __stdcall AddCombineCallback(std::string& asName, std::string& asItemA, std::string& asItemB, std::string& asFunction, bool abAutoDestroy);
	static void __stdcall RemoveCombineCallback(std::string& asName);

	/**
	 * Callback syntax: MyFunc(string &in asItem, string &in asEntity)
	 */
	static void __stdcall AddUseItemCallback(std::string& asName, std::string& asItem, std::string& asEntity, std::string& asFunction, bool abAutoDestroy);
	static void __stdcall RemoveUseItemCallback(std::string& asName);

	// Engine data
	static void __stdcall PreloadParticleSystem(std::string& asPSFile);
	static void __stdcall PreloadSound(std::string& asSoundFile);

	static void __stdcall CreateParticleSystemAtEntity(std::string& asPSName, std::string& asPSFile, std::string& asEntity, bool abSavePS);
	static void __stdcall CreateParticleSystemAtEntityExt(	std::string& asPSName, std::string& asPSFile, std::string& asEntity, bool abSavePS, float afR, float afG, float afB, float afA,
															bool abFadeAtDistance, float afFadeMinEnd, float afFadeMinStart, float afFadeMaxStart, float afFadeMaxEnd);
	static void __stdcall DestroyParticleSystem(std::string& asName);

	//asEntity can be "Player". If abSaveSound = true the sound is never attached to the entity! Also note that saving should on be used on looping sounds!
	static void __stdcall PlaySoundAtEntity(std::string& asSoundName, std::string& asSoundFile, std::string& asEntity, float afFadeTime, bool abSaveSound);
	static void __stdcall FadeInSound(std::string& asSoundName, float afFadeTime, bool abPlayStart);
	static void __stdcall StopSound(std::string& asSoundName, float afFadeTime);
	
	static void __stdcall PlayMusic(std::string& asMusicFile, bool abLoop, float afVolume, float afFadeTime, int alPrio, bool abResume);
	static void __stdcall StopMusic(float afFadeTime, int alPrio);
	
	static void __stdcall FadeGlobalSoundVolume(float afDestVolume, float afTime);
	static void __stdcall FadeGlobalSoundSpeed(float afDestSpeed, float afTime);
	
	static void __stdcall SetLightVisible(std::string& asLightName, bool abVisible);
	/**
	 * -1 for color or radius means keeping the value.
	 */
	static void __stdcall FadeLightTo(std::string& asLightName, float afR, float afG, float afB, float afA, float afRadius, float afTime);
	static void __stdcall SetLightFlickerActive(std::string& asLightName, bool abActive);



	// Entity properties
	static void __stdcall SetEntityActive(std::string& asName, bool abActive);
	static void __stdcall SetEntityVisible(std::string& asName, bool abVisible);
	static bool __stdcall GetEntityExists(std::string& asName);
	static void __stdcall SetEntityPos(std::string& asName, float afX, float afY, float afZ);
	static float __stdcall GetEntityPosX(std::string& asName);
	static float __stdcall GetEntityPosY(std::string& asName);
	static float __stdcall GetEntityPosZ(std::string& asName);
	/**
	* CrossHair can be: Default (uses default), Grab, Push, Ignite, Pick, LevelDoor, Ladder
	 */
	static void __stdcall SetEntityCustomFocusCrossHair(std::string& asName, std::string &asCrossHair);
	static void __stdcall CreateEntityAtArea(std::string& asEntityName, std::string& asEntityFile, std::string& asAreaName, bool abFullGameSave);
	static void __stdcall ReplaceEntity(std::string& asName, std::string& asBodyName, std::string& asNewEntityName, std::string& asNewEntityFile, bool abFullGameSave);
	static void __stdcall PlaceEntityAtEntity(std::string& asName, std::string& asTargetEntity, std::string& asTargetBodyName, bool abUseRotation);
	/**
	* Callback syntax: MyFunc(string &in entity, int alState) state: 1=looking, -1=not looking
	*/
	static void __stdcall SetEntityPlayerLookAtCallback(std::string& asName, std::string& asCallback, bool abRemoveWhenLookedAt);
	/**
	* Callback syntax: MyFunc(string &in entity)
	*/
	static void __stdcall SetEntityPlayerInteractCallback(std::string& asName, std::string& asCallback, bool abRemoveOnInteraction);
	/**
	 * Callback syntax: MyFunc(string &in entity, string &in type). Type depends on entity type and includes: "OnPickup", "Break", "OnIgnite", etc
	 */
	static void __stdcall SetEntityCallbackFunc(std::string& asName, std::string& asCallback);
	/**
	* A callback called when ever the connection state changes (button being switched on). Syntax: void Func(string &in EntityName, int alState). alState: -1=off, 0=between 1=on
	*/
	static void __stdcall SetEntityConnectionStateChangeCallback(std::string& asName, std::string& asCallback);
	static void __stdcall SetEntityInteractionDisabled(std::string& asName, bool abDisabled);
	/**
	 * This function does NOT support asterix!
	 */
	static bool __stdcall GetEntitiesCollide(std::string& asEntityA, std::string& asEntityB);

	static void __stdcall SetPropEffectActive(std::string& asName, bool abActive, bool abFadeAndPlaySounds);
	static void __stdcall SetPropActiveAndFade(std::string& asName, bool abActive, float afFadeTime);
	static void __stdcall SetPropStaticPhysics(std::string& asName, bool abX);
	static bool __stdcall GetPropIsInteractedWith(std::string& asName);
	/**
	 * Rotates the prop up to a set speed. If OffsetArea = "", then center of body is used.
	 */
	static void __stdcall RotatePropToSpeed(std::string& asName, float afAcc, float afGoalSpeed, float afAxisX, float afAxisY, float afAxisZ, bool abResetSpeed, std::string& asOffsetArea);	
	static void __stdcall StopPropMovement(std::string& asName);	
	
	static void __stdcall AttachPropToProp(std::string& asPropName, std::string& asAttachName, std::string& asAttachFile, float afPosX, float afPosY, float afPosZ, float afRotX, float afRotY, float afRotZ);
	static void __stdcall AddAttachedPropToProp(std::string& asPropName, std::string& asAttachName, std::string& asAttachFile, float afPosX, float afPosY, float afPosZ, float afRotX, float afRotY, float afRotZ);
	static void __stdcall RemoveAttachedPropFromProp(std::string& asPropName, std::string& asAttachName);

	
	static void __stdcall SetLampLit(std::string& asName, bool abLit, bool abEffects);
	static void __stdcall SetSwingDoorLocked(std::string& asName, bool abLocked, bool abEffects);
	static void __stdcall SetSwingDoorClosed(std::string& asName, bool abClosed, bool abEffects);
	static void __stdcall SetSwingDoorDisableAutoClose(std::string& asName, bool abDisableAutoClose);
	static bool __stdcall GetSwingDoorLocked(std::string &asName);
	static bool __stdcall GetSwingDoorClosed(std::string &asName);
	/**
	 * -1 = angle is close to 0, 1=angle is 70% or higher of max, 0=inbetween -1 and 1.
	 */
	static int __stdcall GetSwingDoorState(std::string &asName);
	static void __stdcall SetLevelDoorLocked(std::string& asName, bool abLocked);
	static void __stdcall SetLevelDoorLockedSound(std::string& asName, std::string& asSound);
	static void __stdcall SetLevelDoorLockedText(std::string& asName, std::string& asTextCat, std::string& asTextEntry);
	/**
	* State: 0=not stuck 1 = at max -1= at min
	*/
	static void __stdcall SetPropObjectStuckState(std::string& asName, int alState);
	
	static void __stdcall SetWheelAngle(std::string& asName, float afAngle, bool abAutoMove);
	static void __stdcall SetWheelStuckState(std::string& asName, int alState, bool abEffects);
	static void __stdcall SetLeverStuckState(std::string& asName, int alState, bool abEffects);
	static void __stdcall SetWheelInteractionDisablesStuck(std::string& asName, bool abX);
	static void __stdcall SetLeverInteractionDisablesStuck(std::string& asName, bool abX);
	static int __stdcall GetLeverState(std::string& asName);
	
	static void __stdcall SetMultiSliderStuckState(std::string& asName, int alStuckState, bool abEffects);
	/**
	 * Called when state changes Syntax: MyFunc(string &in asEntity, int alState)
	 */
	static void __stdcall SetMultiSliderCallback(std::string& asName, std::string& asCallback);

	static void __stdcall SetButtonSwitchedOn(std::string& asName, bool abSwitchedOn, bool abEffects);
	static void __stdcall SetAllowStickyAreaAttachment(bool abX);
	static void __stdcall AttachPropToStickyArea(std::string& asAreaName, std::string& asProp);
	static void __stdcall AttachBodyToStickyArea(std::string& asAreaName, std::string& asBody);
	static void __stdcall DetachFromStickyArea(std::string& asAreaName);
	static void __stdcall SetNPCAwake(std::string& asName, bool abAwake, bool abEffects);
	static void __stdcall SetNPCFollowPlayer(std::string& asName, bool abX);

	static void __stdcall SetEnemyDisabled(std::string& asName, bool abDisabled);
	static void __stdcall SetEnemyIsHallucination(std::string& asName, bool abX);
	static void __stdcall FadeEnemyToSmoke(std::string& asName, bool abPlaySound);
	static void __stdcall ShowEnemyPlayerPosition(std::string& asName);
	static void __stdcall AlertEnemyOfPlayerPresence(std::string& asName);
	static void __stdcall SetEnemyDisableTriggers(std::string& asName, bool abX);
	static void __stdcall AddEnemyPatrolNode(std::string& asName, std::string& asNodeName, float afWaitTime, std::string& asAnimation);
	static void __stdcall ClearEnemyPatrolNodes(std::string& asEnemyName);
	static void __stdcall SetEnemySanityDecreaseActive(std::string& asName, bool abX);
	static void __stdcall TeleportEnemyToNode(std::string & asEnemyName, std::string & asNodeName, bool abChangeY);
	static void __stdcall TeleportEnemyToEntity(std::string & asName, std::string & asTargetEntity, std::string & asTargetBody, bool abChangeY);

	/**
	 * Pose can be "Biped" or "Quadruped"
	 */
	static void __stdcall ChangeManPigPose(std::string& asName, std::string& asPoseType);
	
	static void __stdcall SetTeslaPigFadeDisabled(std::string& asName, bool abX);
	static void __stdcall SetTeslaPigSoundDisabled(std::string& asName, bool abX);
	static void __stdcall SetTeslaPigEasyEscapeDisabled(std::string& asName, bool abX);
	static void __stdcall ForceTeslaPigSighting(std::string& asName);

	static std::string& __stdcall GetEnemyStateName(std::string& asName);

	static void __stdcall SetPropHealth(std::string& asName, float afHealth);
	static void __stdcall AddPropHealth(std::string& asName, float afHealth);
	static float __stdcall GetPropHealth(std::string& asName);
	static void __stdcall ResetProp(std::string& asName);
	/**
	 * Callback syntax: MyFunc(string &in asProp)
	 */
	static void __stdcall PlayPropAnimation(std::string& asProp, std::string& asAnimation, float afFadeTime, bool abLoop, std::string &asCallback);

	
	/**
	 * State is 0 -1 where 0 is close and 1 is open. Any intermediate value is also valid!
	 */
	static void __stdcall SetMoveObjectState(std::string& asName, float afState);
	static void __stdcall SetMoveObjectStateExt(std::string& asName, float afState, float afAcc, float afMaxSpeed, float afSlowdownDist, bool abResetSpeed);


	static void __stdcall AddPropForce(std::string& asName, float afX, float afY, float afZ, std::string& asCoordSystem);
	static void __stdcall AddPropImpulse(std::string& asName, float afX, float afY, float afZ, std::string& asCoordSystem);
	static void __stdcall AddBodyForce(std::string& asName, float afX, float afY, float afZ, std::string& asCoordSystem);
	static void __stdcall AddBodyImpulse(std::string& asName, float afX, float afY, float afZ, std::string& asCoordSystem);
	/**
	* Do not use this on joints in SwingDoors, Levers, Wheels, etc where the joint is part of an interaction. That will make the game crash.
	*/
	static void __stdcall BreakJoint(std::string& asName);
	static void __stdcall SetBodyMass(std::string& asName, float afMass);
	static float __stdcall GetBodyMass(std::string& asName);

	// Parent can have asterix in name but mot child! Entity callbacks alStates = 1=only enter, -1=only leave 0=both. Syntax: void Func(string &in asParent, string &in asChild, int alState). alState: 1=enter, -1=leave.
	static void __stdcall AddEntityCollideCallback(std::string& asParentName, std::string& asChildName, std::string& asFunction, bool abDeleteOnCollide, int alStates);
	// Parent can have asterix in name but mot child!
	static void __stdcall RemoveEntityCollideCallback(std::string& asParentName, std::string& asChildName);


    // Entity connections
	static void __stdcall InteractConnectPropWithRope(	std::string& asName, std::string& asPropName, std::string& asRopeName, bool abInteractOnly,
														float afSpeedMul, float afToMinSpeed, float afToMaxSpeed,
														bool abInvert, int alStatesUsed);
	/**
	 * This one should only be used if there must be an exact correspondance to prope "amount" and the moveobject open amount. It is best used for Wheel-door connections!
	 */
	static void __stdcall InteractConnectPropWithMoveObject(std::string& asName, std::string& asPropName, std::string& asMoveObjectName, bool abInteractOnly,
															bool abInvert, int alStatesUsed);
	/**
	 * Callback Syntax: MyFunc(string &in asConnectionName, string &in asMainEntity, string &in asConnectEntity, int alState). State is what is sent to connection entity and will be inverted if abInvertStateSent=true!
	 */
	static void __stdcall ConnectEntities(std::string& asName, std::string& asMainEntity, std::string& asConnectEntity, bool abInvertStateSent, int alStatesUsed, std::string& asCallbackFunc);


	// TEMP
	/*static void __stdcall CreateRope(	string& asName, 
										string& asStartArea, string& asEndArea, 
										string& asStartBody, string& asEndBody,
										float afMinTotalLength, float afMaxTotalLength,
										float afSegmentLength, float afDamping,
										float afStrength, float afStiffness,
										string& asMaterial, float afRadius,
										float afLengthTileAmount, float afLengthTileSize,
										string& asSound,float afSoundStartSpeed, float afSoundStopSpeed,
										bool abAutoMove, float afAutoMoveAcc, float afAutoMoveMaxSpeed
										);*/

	//////////////////////////////
	// Math
	static float __stdcall ScriptSin(float afX);
	static float __stdcall ScriptCos(float afX);
	static float __stdcall ScriptTan(float afX);
	static float __stdcall ScriptAsin(float afX);
	static float __stdcall ScriptAcos(float afX);
	static float __stdcall ScriptAtan(float afX);
	static float __stdcall ScriptAtan2(float afX, float afY);
	static float __stdcall ScriptSqrt(float afX);
	static float __stdcall ScriptPow(float afBase, float afExp);
	static float __stdcall ScriptMin(float afA, float afB);
	static float __stdcall ScriptMax(float afA, float afB);
	static float __stdcall ScriptClamp(float afX, float afMin, float afMax);
	static float __stdcall ScriptAbs(float afX);

	//////////////////////////////
	// Parsing
	static int __stdcall ScriptStringToInt(std::string& asString);
	static float __stdcall ScriptStringToFloat(std::string& asString);
	static bool __stdcall ScriptStringToBool(std::string& asString);
};

//----------------------------------------------


#endif // LUX_SCRIPT_HANDLER_H
