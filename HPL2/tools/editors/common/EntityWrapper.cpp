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

#include "EntityWrapper.h"

#include "EditorBaseClasses.h"
#include "EditorWorld.h"

#include "EditorGrid.h"
#include "EditorClipPlane.h"

#include "EditorEditModeSelect.h"

#include "EditorActionEntity.h"
#include "EditorActionSelection.h"

#include "EditorWindowViewport.h"

#include "EditorHelper.h"

#include "EntityWrapperCompoundObject.h"

#include "EntityIcon.h"
#include "EngineEntity.h"
#include "EditorFileWatcher.h"

#include "resources/XmlHelper.h"
#include "system/Hasher.h"
#include <tinyxml2.h>

#include <algorithm>

//------------------------------------------------------------------

tVertexVec iEntityWrapper::mvArrowQuads[4] =
{
	tVertexVec(4),
	tVertexVec(4),
	tVertexVec(4),
	tVertexVec(4)
};

//------------------------------------------------------------------

tString GUIDToHex(unsigned long long alGUID)
{
	if(alGUID==0) return "";

	char buf[17];
	snprintf(buf, sizeof(buf), "%016llx", alGUID);
	return buf;
}

unsigned long long GUIDFromHex(const tString& asHex)
{
	return asHex.empty() ? 0 : strtoull(asHex.c_str(), NULL, 16);
}


iPropVal::iPropVal(iProp* apProp)
{
	mpProp = apProp;
}

void iPropVal::Save(tinyxml2::XMLElement* apElement)
{
	if(mpProp->IsSaved()) 
		SaveSpecific(apElement);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PROPERTY INT
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iPropVal* cPropInt::CreateValue()
{
	return hplNew(cPropValInt,(this));
}

cPropValInt::cPropValInt(iProp* apProp) : iPropVal(apProp)
{
	cPropInt* pProp = (cPropInt*)apProp;
	Set(pProp->GetDefault());
}

void cPropValInt::GetFromEntity(iEntityWrapper* apEnt)
{
	apEnt->GetProperty(mpProp->GetID(), mlVal);
}

void cPropValInt::SetToEntity(iEntityWrapper* apEnt)
{
	apEnt->SetProperty(mpProp->GetID(), mlVal);
}

void cPropValInt::Load(tinyxml2::XMLElement* apElement)
{
	mlVal = GetAttributeInt(apElement, mpProp->GetName(), mlVal);
}

void cPropValInt::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	// An object for every renderer leaves RendererMask out, so maps without
	// renderer-specific objects save as before.
	if(mpProp->GetID()==eObjInt_RendererMask && mlVal==static_cast<int>(hpl::kRendererMaskAll))
		return;
	SetAttributeInt(apElement, mpProp->GetName(), mlVal);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PROPERTY FLOAT
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iPropVal* cPropFloat::CreateValue()
{
	return hplNew(cPropValFloat,(this));
}

cPropValFloat::cPropValFloat(iProp* apProp) : iPropVal(apProp)
{
	cPropFloat* pProp = (cPropFloat*)apProp;
	Set(pProp->GetDefault());
}

void cPropValFloat::GetFromEntity(iEntityWrapper* apEnt)
{
	apEnt->GetProperty(mpProp->GetID(), mfVal);
}

void cPropValFloat::SetToEntity(iEntityWrapper* apEnt)
{
	apEnt->SetProperty(mpProp->GetID(), mfVal);
}

void cPropValFloat::Load(tinyxml2::XMLElement* apElement)
{
	mfVal = GetAttributeFloat(apElement, mpProp->GetName(), mfVal);
}

void cPropValFloat::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	SetAttributeFloat(apElement, mpProp->GetName(), mfVal);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PROPERTY BOOL
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iPropVal* cPropBool::CreateValue()
{
	return hplNew(cPropValBool,(this));
}

cPropValBool::cPropValBool(iProp* apProp) : iPropVal(apProp)
{
	cPropBool* pProp = (cPropBool*)apProp;
	Set(pProp->GetDefault());
}

void cPropValBool::GetFromEntity(iEntityWrapper* apEnt)
{
	apEnt->GetProperty(mpProp->GetID(), mbVal);
}

void cPropValBool::SetToEntity(iEntityWrapper* apEnt)
{
	apEnt->SetProperty(mpProp->GetID(), mbVal);
}

void cPropValBool::Load(tinyxml2::XMLElement* apElement)
{
	mbVal = GetAttributeBool(apElement, mpProp->GetName(), mbVal);
}

void cPropValBool::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	SetAttributeBool(apElement, mpProp->GetName(), mbVal);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PROPERTY STRING
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iPropVal* cPropStr::CreateValue()
{
	return hplNew(cPropValStr,(this));
}

cPropValStr::cPropValStr(iProp* apProp) : iPropVal(apProp)
{
	cPropStr* pProp = (cPropStr*)apProp;
	Set(pProp->GetDefault());
}

void cPropValStr::GetFromEntity(iEntityWrapper* apEnt)
{
	apEnt->GetProperty(mpProp->GetID(), msVal);
}

void cPropValStr::SetToEntity(iEntityWrapper* apEnt)
{
	apEnt->SetProperty(mpProp->GetID(), msVal);
}

void cPropValStr::Load(tinyxml2::XMLElement* apElement)
{
	msVal = GetAttributeString(apElement, mpProp->GetName(), msVal);
}

void cPropValStr::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	SetAttributeString(apElement, mpProp->GetName(), msVal);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PROPERTY VEC2F
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iPropVal* cPropVec2f::CreateValue()
{
	return hplNew(cPropValVec2f,(this));
}

cPropValVec2f::cPropValVec2f(iProp* apProp) : iPropVal(apProp)
{
	cPropVec2f* pProp = (cPropVec2f*)apProp;
	Set(pProp->GetDefault());
}

void cPropValVec2f::GetFromEntity(iEntityWrapper* apEnt)
{
	apEnt->GetProperty(mpProp->GetID(), mvVal);
}

void cPropValVec2f::SetToEntity(iEntityWrapper* apEnt)
{
	apEnt->SetProperty(mpProp->GetID(), mvVal);
}

void cPropValVec2f::Load(tinyxml2::XMLElement* apElement)
{
	mvVal = GetAttributeVector2f(apElement, mpProp->GetName(), mvVal);
}

void cPropValVec2f::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	SetAttributeVector2f(apElement, mpProp->GetName(), mvVal);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PROPERTY VEC3F
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iPropVal* cPropVec3f::CreateValue()
{
	return hplNew(cPropValVec3f,(this));
}

cPropValVec3f::cPropValVec3f(iProp* apProp) : iPropVal(apProp)
{
	cPropVec3f* pProp = (cPropVec3f*)apProp;
	Set(pProp->GetDefault());
}

void cPropValVec3f::GetFromEntity(iEntityWrapper* apEnt)
{
	apEnt->GetProperty(mpProp->GetID(), mvVal);
}

void cPropValVec3f::SetToEntity(iEntityWrapper* apEnt)
{
	apEnt->SetProperty(mpProp->GetID(), mvVal);
}

void cPropValVec3f::Load(tinyxml2::XMLElement* apElement)
{
	mvVal = GetAttributeVector3f(apElement, mpProp->GetName(), mvVal);
}

void cPropValVec3f::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	SetAttributeVector3f(apElement, mpProp->GetName(), mvVal);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PROPERTY COLOR
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iPropVal* cPropCol::CreateValue()
{
	return hplNew(cPropValCol,(this));
}

cPropValCol::cPropValCol(iProp* apProp) : iPropVal(apProp)
{
	cPropCol* pProp = (cPropCol*)apProp;
	Set(pProp->GetDefault());
}

void cPropValCol::GetFromEntity(iEntityWrapper* apEnt)
{
	apEnt->GetProperty(mpProp->GetID(), mVal);
}

void cPropValCol::SetToEntity(iEntityWrapper* apEnt)
{
	apEnt->SetProperty(mpProp->GetID(), mVal);
}

void cPropValCol::Load(tinyxml2::XMLElement* apElement)
{
	mVal = GetAttributeColor(apElement, mpProp->GetName(), mVal);
}

void cPropValCol::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	SetAttributeColor(apElement, mpProp->GetName(), mVal);
}

//------------------------------------------------------------------
//------------------------------------------------------------------
///////////////////////////////////////////////////////////////////
// ENTITY WRAPPER TYPE
///////////////////////////////////////////////////////////////////
//------------------------------------------------------------------
//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// CONSTRUCTORS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iEntityWrapperType::iEntityWrapperType(int alID, const tWString& asName, const tString& asXmlElementName)
{
	mlID = alID;
	msName = asName;
	msXmlElementName = asXmlElementName;

	mbVisible = true;
	mbActive = true;

	mpWorld = NULL;

	mvProperties.resize(ePropCopyStep_LastEnum);

	mbCanTranslate = true;
	mbCanRotate = true;
	mScaleType = eScaleType_Normal;
	mbRenameable = true;
	mbCloneable = true;
	mbDeletable = true;

	AddInt(eObjInt_ID, "ID", -1, ePropCopyStep_PreEnt);
	AddInt(eObjInt_RendererMask, "RendererMask", static_cast<int>(hpl::kRendererMaskAll));

	AddString(eObjStr_Name, "Name", cString::To8Char(msName), ePropCopyStep_PreEnt);
	AddString(eObjStr_Tag, "Tag");
	AddString(eObjStr_GUID, "GUID", "", ePropCopyStep_PreEnt);

	AddBool(eObjBool_Active, "Active");

	AddVec3f(eObjVec3f_Position, "WorldPos", 0, ePropCopyStep_PreEnt);
	AddVec3f(eObjVec3f_Rotation, "Rotation", 0, ePropCopyStep_PreEnt);
	AddVec3f(eObjVec3f_Scale, "Scale", 1, ePropCopyStep_PreEnt);
}

iEntityWrapperType::~iEntityWrapperType()
{
	for(int i=0;i<(int)mvProperties.size();++i)
		STLDeleteAll(mvProperties[i]);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PUBLIC METHODS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iEntityWrapperData* iEntityWrapperType::CreateData()
{
	iEntityWrapperData* pData = CreateSpecificData();
	
	return pData;
}

void iEntityWrapperType::CreateValuesInData(iEntityWrapperData* apData)
{
	for(int i=0;i<(int)mvProperties.size();++i)
	{
		tPropList& lstProps = mvProperties[i];
		tPropListIt it = lstProps.begin();
		for(;it!=lstProps.end();++it)
		{
			iProp* pProp = *it;
			apData->AddValue((ePropCopyStep)i, pProp->CreateValue());
		}
	}
}

//------------------------------------------------------------------

void iEntityWrapperType::SetWorld(iEditorWorld* apWorld)
{
	mpWorld = apWorld;
	OnSetWorld(apWorld);
}

//------------------------------------------------------------------

tString iEntityWrapperType::ToString()
{
	return cString::To8Char(GetName()) + " <" + GetXmlElementName()+">";
}

//------------------------------------------------------------------

bool iEntityWrapperType::IsAppropriateType(tinyxml2::XMLElement* apElement)
{
	return msXmlElementName==apElement->Value();
}

bool iEntityWrapperType::IsAppropriateDefaultType(tinyxml2::XMLElement* apElement)
{
	return false;
}

//------------------------------------------------------------------

void iEntityWrapperType::SetVisible(bool abX)
{
	if(mbVisible==abX) 
		return; 
	
	mbVisible = abX;
	mpWorld->SetVisibilityUpdated();
}

//------------------------------------------------------------------

void iEntityWrapperType::SetActive(bool abX)
{
	if(mbActive==abX)
		return;

	mbActive = abX;
	mpWorld->SetVisibilityUpdated();
}

//------------------------------------------------------------------

cPropInt* iEntityWrapperType::GetPropInt(int alID)
{
	return (cPropInt*)GetPropByTypeAndID(eVariableType_Int, alID);
}

//------------------------------------------------------------------

cPropFloat* iEntityWrapperType::GetPropFloat(int alID)
{
	return (cPropFloat*)GetPropByTypeAndID(eVariableType_Float, alID);
}

//------------------------------------------------------------------

cPropBool* iEntityWrapperType::GetPropBool(int alID)
{
	return (cPropBool*)GetPropByTypeAndID(eVariableType_Bool, alID);
}

//------------------------------------------------------------------

cPropStr* iEntityWrapperType::GetPropString(int alID)
{
	return (cPropStr*)GetPropByTypeAndID(eVariableType_String, alID);
}

//------------------------------------------------------------------

cPropVec2f* iEntityWrapperType::GetPropVec2f(int alID)
{
	return (cPropVec2f*)GetPropByTypeAndID(eVariableType_Vec2, alID);
}

//------------------------------------------------------------------

cPropVec3f* iEntityWrapperType::GetPropVec3f(int alID)
{
	return (cPropVec3f*)GetPropByTypeAndID(eVariableType_Vec3, alID);
}

//------------------------------------------------------------------

cPropCol* iEntityWrapperType::GetPropColor(int alID)
{
	return (cPropCol*)GetPropByTypeAndID(eVariableType_Color, alID);
}

//------------------------------------------------------------------

iProp* iEntityWrapperType::GetPropByTypeAndID(eVariableType aType, int alID)
{
	for(int i=0;i<(int)mvProperties.size();++i)
	{
		tPropList& lstProps = mvProperties[i];
		tPropListIt it = lstProps.begin();
		for(;it!=lstProps.end();++it)
		{
			iProp* pProp = *it;
			if(pProp->GetID()==alID && pProp->GetType()==aType)
				return pProp;
		}
	}

	return NULL;
}

//------------------------------------------------------------------

iProp* iEntityWrapperType::GetPropByName(const tString& asName)
{
	for(int i=0;i<(int)mvProperties.size();++i)
	{
		tPropList& lstProps = mvProperties[i];
		tPropListIt it = lstProps.begin();
		for(;it!=lstProps.end();++it)
		{
			iProp* pProp = *it;
			if(pProp->GetName()==asName)
				return pProp;
		}
	}

	return NULL;
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PROTECTED METHODS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

void iEntityWrapperType::AddInt(int alPropID, const tString& asName, int alDefault, ePropCopyStep aStep, bool abSave)
{
	AddProperty(hplNew(cPropInt,(alPropID, asName, alDefault, abSave)), aStep);
}

//------------------------------------------------------------------

void iEntityWrapperType::AddFloat(int alPropID, const tString& asName, float afDefault, ePropCopyStep aStep, bool abSave)
{
	AddProperty(hplNew(cPropFloat,(alPropID, asName, afDefault, abSave)), aStep);
}

//------------------------------------------------------------------

void iEntityWrapperType::AddBool(int alPropID, const tString& asName, bool abDefault, ePropCopyStep aStep, bool abSave)
{
	AddProperty(hplNew(cPropBool,(alPropID, asName, abDefault, abSave)), aStep);
}

//------------------------------------------------------------------

void iEntityWrapperType::AddString(int alPropID, const tString& asName, const tString& asDefault, ePropCopyStep aStep, bool abSave)
{
	AddProperty(hplNew(cPropStr,(alPropID, asName, asDefault, abSave)), aStep);
}

//------------------------------------------------------------------

void iEntityWrapperType::AddVec2f(int alPropID, const tString& asName, const cVector2f& avDefault, ePropCopyStep aStep, bool abSave)
{
	AddProperty(hplNew(cPropVec2f,(alPropID, asName, avDefault, abSave)), aStep);
}

//------------------------------------------------------------------

void iEntityWrapperType::AddVec3f(int alPropID, const tString& asName, const cVector3f& avDefault, ePropCopyStep aStep, bool abSave)
{
	AddProperty(hplNew(cPropVec3f,(alPropID, asName, avDefault, abSave)), aStep);
}

void iEntityWrapperType::AddColor(int alPropID, const tString& asName, const cColor& aDefault, ePropCopyStep aStep, bool abSave)
{
	AddProperty(hplNew(cPropCol,(alPropID, asName, aDefault, abSave)), aStep);
}

//------------------------------------------------------------------

void iEntityWrapperType::AddProperty(iProp* apProp, ePropCopyStep aStep)
{
	mvProperties[aStep].push_back(apProp);
}

//------------------------------------------------------------------


//------------------------------------------------------------------
//------------------------------------------------------------------
///////////////////////////////////////////////////////////////////
// ENTITY WRAPPER DATA
///////////////////////////////////////////////////////////////////
//------------------------------------------------------------------
//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// CONSTRUCTORS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iEntityWrapperData::iEntityWrapperData(iEntityWrapperType *apType)
{
	mpType = apType;
	mvValues.resize(ePropCopyStep_LastEnum);
	apType->CreateValuesInData(this);

	mlParentID = -1;
	mlCompoundID = -1;

	mpExtData = NULL;

	///////////////////////////////////////////////////////////////////////////
	// Base name for entities is the associated XML element string, unless tweaked
	SetName(mpType->GetXmlElementName());

	mpType->GetWorld()->CreateDataCallback(this);
}

iEntityWrapperData::~iEntityWrapperData()
{
	for(int i=0;i<(int)mvValues.size();++i)
		STLDeleteAll(mvValues[i]);

	mpType->GetWorld()->DestroyDataCallback(this);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PUBLIC METHODS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

void iEntityWrapperData::CopyFromEntity(iEntityWrapper* apEnt)
{
	////////////////////////////////////////////
	// Get property values from Entity
	for(int i=0;i<ePropCopyStep_LastEnum; ++i)
	{
		tPropValList& lstValues = mvValues[i];
		tPropValListIt it = lstValues.begin();
		for(;it!=lstValues.end();++it)
		{
			iPropVal* pVal = *it;
			pVal->GetFromEntity(apEnt);
		}
	}

	mlParentID = apEnt->GetParentID();
	mlCompoundID = apEnt->GetCompoundObjectID();

	//////////////////////////////////
	// Get children ID's
	tEntityWrapperList& lstChildren = apEnt->GetChildren();
	tEntityWrapperListIt it = lstChildren.begin();
	for(;it!=lstChildren.end();++it)
	{
		iEntityWrapper* pChild = *it;
		mvChildIDs.push_back(pChild->GetID());
	}

	mpType->GetWorld()->CopyDataFromEntityCallback(this, apEnt);
}

//------------------------------------------------------------------

void iEntityWrapperData::CopyToEntity(iEntityWrapper* apEnt, int alCopyStep)
{
	////////////////////////////////////////////
	// Set property values to Entity
	tPropValList& lstValues = mvValues[alCopyStep];
	tPropValListIt it = lstValues.begin();
	for(;it!=lstValues.end();++it)
	{
		iPropVal* pVal = *it;
		pVal->SetToEntity(apEnt);
	}

	////////////////////////////////////////////
	// If all stuff has been deployed to the map already
	if(alCopyStep==ePropCopyStep_PostDeployAll)
	{
		iEditorWorld* pWorld = mpType->GetWorld();

		/////////////////////////////
		// Attach children
		iEntityWrapper* pParent = pWorld->GetEntity(mlParentID);
		if(pParent)	pParent->AttachChild(apEnt);
		for(int i=0;i<(int)mvChildIDs.size();++i)
		{
			iEntityWrapper* pChild = pWorld->GetEntity(mvChildIDs[i]);
			apEnt->AttachChild(pChild);
		}

		/////////////////////////////
		// Setup Compound if any
		cEntityWrapperCompoundObject* pCompound = (cEntityWrapperCompoundObject*)pWorld->GetEntity(mlCompoundID);
		if(pCompound) pCompound->AddComponent(apEnt);
	}

	mpType->GetWorld()->CopyDataToEntityCallback(this, apEnt, alCopyStep);
}

//------------------------------------------------------------------

bool iEntityWrapperData::Load(tinyxml2::XMLElement* apElement)
{
	///////////////////////////////////////////
	// Load values from XML element
	for(int i=0;i<ePropCopyStep_LastEnum; ++i)
	{
		tPropValList& lstValues = mvValues[i];
		tPropValListIt it = lstValues.begin();
		for(;it!=lstValues.end();++it)
		{
			iPropVal* pVal = *it;
			pVal->Load(apElement);
		}
	}

	mpType->GetWorld()->LoadDataCallback(this, apElement);

	LoadChildren(apElement);

	return true;
}

//------------------------------------------------------------------

bool iEntityWrapperData::Save(tinyxml2::XMLElement* apParentElement)
{
	tinyxml2::XMLElement* pElement = apParentElement->GetDocument()->NewElement(mpType->GetXmlElementName().c_str());
	apParentElement->InsertEndChild(pElement);
	mpType->GetWorld()->SaveDataCallback(this, pElement);

	return SaveSpecific(pElement);
}

bool iEntityWrapperData::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	///////////////////////////////////////////
	// Load values from XML element
	for(int i=0;i<ePropCopyStep_LastEnum; ++i)
	{
		tPropValList& lstValues = mvValues[i];
		tPropValListIt it = lstValues.begin();
		for(;it!=lstValues.end();++it)
		{
			iPropVal* pVal = *it;

			pVal->Save(apElement);
		}
	}

	SaveChildren(apElement);

	return true;
}

//------------------------------------------------------------------

iEntityWrapper* iEntityWrapperData::CreateEntity()
{
	///////////////////////////////////////////
	// Tries to create an Entity. If fails, destroy it
	iEntityWrapper* pEnt = CreateSpecificEntity();
	if(pEnt->Create())return pEnt;

	Error("Failed creation of Entity named %s of type %s.\n", GetName().c_str(), mpType->GetXmlElementName().c_str());

	hplDelete(pEnt);
	return NULL;
}

//------------------------------------------------------------------

void iEntityWrapperData::AddValue(ePropCopyStep aX, iPropVal* apVal)
{
	mvValues[aX].push_back(apVal);
}

//------------------------------------------------------------------

void iEntityWrapperData::SetInt(int alID, int alX)
{
	cPropValInt* pVal = (cPropValInt*)GetValue(alID, eVariableType_Int);
	if(pVal)
		pVal->Set(alX);
}

void iEntityWrapperData::SetFloat(int alID, float afX)
{
	cPropValFloat* pVal = (cPropValFloat*)GetValue(alID, eVariableType_Float);
	if(pVal)
		pVal->Set(afX);
}

void iEntityWrapperData::SetBool(int alID, bool abX)
{
	cPropValBool* pProp = (cPropValBool*)GetValue(alID, eVariableType_Bool);
	if(pProp)
		pProp->Set(abX);
}

void iEntityWrapperData::SetString(int alID, const tString& asX)
{
	cPropValStr* pProp = (cPropValStr*)GetValue(alID, eVariableType_String);
	if(pProp)
		pProp->Set(asX);
}

void iEntityWrapperData::SetVec2f(int alID, const cVector2f& avX)
{
	cPropValVec2f* pProp = (cPropValVec2f*)GetValue(alID, eVariableType_Vec2);
	if(pProp)
		pProp->Set(avX);
}

void iEntityWrapperData::SetVec3f(int alID, const cVector3f& avX)
{
	cPropValVec3f* pProp = (cPropValVec3f*)GetValue(alID, eVariableType_Vec3);
	if(pProp)
		pProp->Set(avX);
}

void iEntityWrapperData::SetColor(int alID, const cColor& aX)
{
	cPropValCol* pProp = (cPropValCol*)GetValue(alID, eVariableType_Color);
	if(pProp)
		pProp->Set(aX);
}

int iEntityWrapperData::GetInt(int alID)
{
	cPropValInt* pProp = (cPropValInt*)GetValue(alID, eVariableType_Int);
	if(pProp)return pProp->Get();
	return 0;
}

float iEntityWrapperData::GetFloat(int alID)
{
	cPropValFloat* pProp = (cPropValFloat*)GetValue(alID, eVariableType_Float);
	if(pProp) return pProp->Get();
	return 0;
}

bool iEntityWrapperData::GetBool(int alID)
{
	cPropValBool* pProp = (cPropValBool*)GetValue(alID, eVariableType_Bool);
	if(pProp)	return pProp->Get();
	else		return false;
}

//----------------------------------

//Variables that are to be returned, incase Prop Data is not found.
static const tString gsNullString ="";
static const cVector2f gvNullVector2(0);
static const cVector3f gvNullVector3(0);
static const cColor gNullColor(0,0);

//----------------------------------

const tString& iEntityWrapperData::GetString(int alID)
{
	cPropValStr* pProp = (cPropValStr*)GetValue(alID, eVariableType_String);
	if(pProp)	return pProp->Get();
	else		return gsNullString;
}

const cVector2f& iEntityWrapperData::GetVec2f(int alID)
{
	cPropValVec2f* pProp = (cPropValVec2f*)GetValue(alID, eVariableType_Vec2);
	if(pProp)	return pProp->Get();
	else		return gvNullVector2;
}

const cVector3f& iEntityWrapperData::GetVec3f(int alID)
{
	cPropValVec3f* pProp = (cPropValVec3f*)GetValue(alID, eVariableType_Vec3);
	if(pProp)	return pProp->Get();
	else		return gvNullVector3;
}

const cColor& iEntityWrapperData::GetColor(int alID)
{
	cPropValCol* pProp = (cPropValCol*)GetValue(alID, eVariableType_Color);
	if(pProp)	return pProp->Get();
	else		return gNullColor;
}

iProp* iEntityWrapperData::GetProperty(int alID, eVariableType aType)
{
	iPropVal* pVal = GetValue(alID, aType);

	if(pVal)
		return pVal->GetProperty();

	return NULL;
}

iPropVal* iEntityWrapperData::GetValue(int alID, eVariableType aType)
{
	///////////////////////////////////////////
	// Search for value and return it
	for(int i=0;i<ePropCopyStep_LastEnum; ++i)
	{
		tPropValList& lstValues = mvValues[i];
		tPropValListIt it = lstValues.begin();
		for(;it!=lstValues.end();++it)
		{
			iPropVal* pVal = *it;
			iProp* pProp = pVal->GetProperty();
			if(pProp->GetID()==alID && pProp->GetType()==aType)
				return pVal;
		}
	}

	return NULL;
}

//--------------------------------------------------------------

int iEntityWrapperData::GetID()
{
	return GetInt(eObjInt_ID);
}

unsigned long long iEntityWrapperData::GetGUID()
{
	return GUIDFromHex(GetString(eObjStr_GUID));
}

const tString& iEntityWrapperData::GetName()
{
	return GetString(eObjStr_Name);
}

void iEntityWrapperData::SetID(int alX)
{
	SetInt(eObjInt_ID, alX);
}

void iEntityWrapperData::SetGUID(unsigned long long alX)
{
	SetString(eObjStr_GUID, GUIDToHex(alX));
}

void iEntityWrapperData::SetName(const tString& asX)
{
	SetString(eObjStr_Name, asX);
}

//------------------------------------------------------------------
//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PROTECTED METHODS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

void iEntityWrapperData::LoadChildren(tinyxml2::XMLElement* apElement)
{
	tinyxml2::XMLElement* pChildrenElement = apElement->FirstChildElement("Children");
	if(pChildrenElement==NULL)
		return;

	for(tinyxml2::XMLElement* pChild = pChildrenElement->FirstChildElement(); pChild != NULL; pChild = pChild->NextSiblingElement())
	{
		if(tString(pChild->Value())!="Child")
			continue;

		int lChildID = GetAttributeInt(pChild, "ID",-1);
		if(lChildID!=-1)
			mvChildIDs.push_back(lChildID);
	}
}

//------------------------------------------------------------------

void iEntityWrapperData::SaveChildren(tinyxml2::XMLElement* apElement)
{
	if(mvChildIDs.empty())
		return;

	tinyxml2::XMLElement* pChildrenElement = apElement->GetDocument()->NewElement("Children");
	apElement->InsertEndChild(pChildrenElement);
	for(int i=0;i<(int)mvChildIDs.size();++i)
	{
		tinyxml2::XMLElement* pChildElement = pChildrenElement->GetDocument()->NewElement("Child");
		pChildrenElement->InsertEndChild(pChildElement);
		SetAttributeInt(pChildElement, "ID", mvChildIDs[i]);
	}
}

//------------------------------------------------------------------
//------------------------------------------------------------------
///////////////////////////////////////////////////////////////////
// ENTITY WRAPPER
///////////////////////////////////////////////////////////////////
//------------------------------------------------------------------
//------------------------------------------------------------------

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// CONSTRUCTORS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iEntityWrapper::iEntityWrapper(iEntityWrapperData* apData)
{
	mpData = apData;
	mpType = mpData->GetType();

	mlGUID = 0;

	mpUserData = NULL;
	mpExtData = NULL;

	mpIcon = NULL;
	mpEngineEntity = NULL;

	mpParent = NULL;
	mpCompoundObject = NULL;

	mbSelected = false;
	mbCulledByPlane = false;
	mlRendererMask = static_cast<int>(hpl::kRendererMaskAll);

	mbEntityUpdated=true;
	mbTranslationUpdated=true;
	mbRotationUpdated=true;
	mbScaleUpdated=true;

	mmtxTranslate = cMatrixf::Identity;
	mmtxRotate = cMatrixf::Identity;
	mmtxScale = cMatrixf::Identity;

	mlLastTransformedAxis = -1;

	mbVisible = true;
}

iEntityWrapper::~iEntityWrapper()
{
	GetEditorWorld()->DestroyEntityWrapperCallback(this);

	if(mpIcon)
		hplDelete(mpIcon);

	if(mpEngineEntity)
		hplDelete(mpEngineEntity);

	if(GetEditorWorld()->IsClearingEntities()==false)
	{
		if(mpCompoundObject)
			mpCompoundObject->RemoveComponent(this);

		if(mpParent)
			mpParent->DetachChild(this);

		ClearChildren();
	}
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PUBLIC METHODS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

bool iEntityWrapper::Create()
{
	mpData->CopyToEntity(this, ePropCopyStep_PreEnt);
	mpIcon = CreateIcon();
	if(CreateEngineEntity()==false &&
		mpIcon==NULL && 
		mpEngineEntity==NULL && mpType->AllowNullEngineEntity()==false)
	{
		Error("Could not create entity '%s' id: %d of type '%s'\n", 
				msName.c_str(), 
				mlID, 
				mpType->GetXmlElementName().c_str());
                
		return false;
	}

	mpData->CopyToEntity(this, ePropCopyStep_PostEnt);

	UpdateEntity();

	// Start watching the files this entity depends on for live reload.
	// (Prefab edits need no per-entity registration: the prefab cache
	// broadcasts and the WORLD reloads matching instances by filename scan.)
	RegisterFileWatches();

	return true;
}

//------------------------------------------------------------------

void iEntityWrapper::OnPostDeployAll(bool abDestroyData)
{
	if(mpData)
	{
		mpData->CopyToEntity(this, ePropCopyStep_PostDeployAll);

		UpdateEntity();

		if(abDestroyData)
		{
			hplDelete(mpData);
			mpData = NULL;
		}
	}
}

//------------------------------------------------------------------

iEditorWorld* iEntityWrapper::GetEditorWorld()
{ 
	return mpType->GetWorld(); 
}

//------------------------------------------------------------------

iEntityWrapperData* iEntityWrapper::CreateCopyData()
{
	iEntityWrapperData* pCopyData = mpType->CreateData();
	pCopyData->CopyFromEntity(this);

	return pCopyData;
}

//------------------------------------------------------------------

tIntVec iEntityWrapper::GetCloneIDs()
{
	tIntVec vEntityID;
	vEntityID.push_back(GetEditorWorld()->GetFreeID());

	return vEntityID;
}

//------------------------------------------------------------------

iEntityWrapper* iEntityWrapper::Clone(const tIntVec& avIDs, bool abDeleteData)
{
	iEntityWrapperData* pCloneData = CreateCopyData();

	pCloneData->SetID(avIDs.back());
	pCloneData->SetGUID(0); // clones get a fresh GUID (assigned by CreateEntityWrapperFromData)
	pCloneData->SetName(GetEditorWorld()->GenerateName(msName));

	SetUpCloneData(pCloneData, avIDs);
	
	iEntityWrapper* pClone = GetEditorWorld()->CreateEntityWrapperFromData(pCloneData);
	pClone->OnPostDeployAll(abDeleteData);

	return pClone;
}

//------------------------------------------------------------------

void iEntityWrapper::SetParent(iEntityWrapper* apEntity, bool abDetachFromOld)
{
	if(apEntity==mpParent) return;

	if(abDetachFromOld && mpParent)
		mpParent->DetachChild(this);

	mpParent = apEntity;
}

//------------------------------------------------------------------

int iEntityWrapper::GetParentID()
{
	int lParentID = -1;
	if(mpParent) lParentID = mpParent->GetID();

	return lParentID;
}

//------------------------------------------------------------------

void iEntityWrapper::AttachChild(iEntityWrapper* apEntity)
{
	if(apEntity==NULL) return;

	tEntityWrapperListIt it = find(mlstChildren.begin(), mlstChildren.end(), apEntity);
	if(it==mlstChildren.end())
	{
		apEntity->SetParent(this);
		mlstChildren.push_back(apEntity);
	}
}

//------------------------------------------------------------------

void iEntityWrapper::DetachChild(iEntityWrapper* apEntity)
{
	if(apEntity==NULL) return;

	tEntityWrapperListIt it = find(mlstChildren.begin(), mlstChildren.end(), apEntity);
	if(it!=mlstChildren.end())
	{
		iEntityWrapper* pChild = *it;
		if(pChild->GetParent()==this)
			pChild->SetParent(NULL,false);

		mlstChildren.erase(it);
	}
}

//------------------------------------------------------------------

bool iEntityWrapper::HasChild(iEntityWrapper* apEntity)
{
	if(apEntity==NULL) return false;

	tEntityWrapperListIt it = find(mlstChildren.begin(), mlstChildren.end(), apEntity);
	return it==mlstChildren.end();
}

//------------------------------------------------------------------

void iEntityWrapper::ClearChildren()
{
	tEntityWrapperListIt it = mlstChildren.begin();
	for(;it!=mlstChildren.end();++it)
	{
		(*it)->SetParent(NULL, false);
	}
	mlstChildren.clear();
}

//------------------------------------------------------------------

void iEntityWrapper::SaveChildren(tinyxml2::XMLElement* apElement)
{
	if(mlstChildren.empty()==false)
	{
		tinyxml2::XMLElement* pXmlChildren = apElement->GetDocument()->NewElement("Children");
		apElement->InsertEndChild(pXmlChildren);
		tEntityWrapperListIt itChildren = mlstChildren.begin();
		for(;itChildren!=mlstChildren.end(); ++itChildren)
		{
			iEntityWrapper* pChild = *itChildren;
			tinyxml2::XMLElement* pXmlChild = pXmlChildren->GetDocument()->NewElement("Child");
			pXmlChildren->InsertEndChild(pXmlChild);

			SetAttributeInt(pXmlChild, "ID", pChild->GetID());
		}
	}
}

//------------------------------------------------------------------

void iEntityWrapper::SetAbsPosition(const cVector3f& avPosition)
{
	if(mvPosition == avPosition) return;

	mvPosition = avPosition;

	mbEntityUpdated = true;
	mbTranslationUpdated = true;

	if(mpCompoundObject)
		mpCompoundObject->SetComponentsUpdated();
}

//------------------------------------------------------------------

void iEntityWrapper::SetAbsScale(const cVector3f& avScale, int alAxis)
{
	if(mvScale == avScale) return;

	mvScale = avScale;

	mlLastTransformedAxis = alAxis;

	mbEntityUpdated = true;
	mbScaleUpdated = true;

	if(mpCompoundObject)
		mpCompoundObject->SetComponentsUpdated();
}

//------------------------------------------------------------------

void iEntityWrapper::SetAbsRotation(const cVector3f& avRotation)
{
	if(mvRotation==avRotation) return;

	mvRotation = avRotation;
    
	mbEntityUpdated = true;
	mbRotationUpdated = true;

	if(mpCompoundObject)
		mpCompoundObject->SetComponentsUpdated();
}

//------------------------------------------------------------------

const cMatrixf& iEntityWrapper::GetWorldMatrix()
{
	UpdateMatrix();
	return mmtxTransform;
}

//------------------------------------------------------------------

void iEntityWrapper::SetWorldMatrix(const cMatrixf& amtxX)
{
	cMatrixf mtxInvMatrix = cMath::MatrixInverse(amtxX);
	cVector3f vScale = cVector3f(1.0f/mtxInvMatrix.GetRight().Length(),
									1.0f/mtxInvMatrix.GetUp().Length(),
									1.0f/mtxInvMatrix.GetForward().Length());

	cMatrixf mtxScale = cMath::MatrixScale(vScale);
	cMatrixf mtxRotate = cMath::MatrixMul(amtxX.GetRotation(), cMath::MatrixInverse(mtxScale));

	//Log("SetWorldMatrixCalled for %s\n", msName.c_str());

	SetAbsPosition(amtxX.GetTranslation());
	SetAbsRotation(cMath::MatrixToEulerAngles(mtxRotate, eEulerRotationOrder_XYZ));
	SetAbsScale(vScale);
}

//------------------------------------------------------------------

void iEntityWrapper::Draw(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, iEditorEditMode* apEditMode, bool abIsSelected, const cColor& aHighlightCol, const cColor& aDisabledCol)
{
	if(mpIcon)
		mpIcon->DrawIcon(apViewport, apFunctions, apEditMode, mbSelected, mvPosition, mbActive && mpType->IsActive(), aDisabledCol, GetIconTint());
	if(mpEngineEntity)
		mpEngineEntity->Draw(apViewport, apFunctions, abIsSelected, mbActive && mpType->IsActive(), aHighlightCol);

	if(mbSelected==false)
		return;

	if(mpParent)
	{
		//cBoundingVolume* pBV = mpParent->GetRenderBV();
		//const cVector3f& vPos = mpParent->GetPosition();
		//cVector3f vHalfSize = pBV->GetSize()*0.5f;

		//apFunctions->DebugDrawBoxMinMax(vPos-vHalfSize, vPos+vHalfSize, cColor(1,0,0,1));
		apFunctions->DebugDrawLine(mvPosition, mpParent->GetPosition(), cColor(1,0,0,1));
	}

	tEntityWrapperListIt itChildren = mlstChildren.begin();
	for(;itChildren!=mlstChildren.end();++itChildren)
	{
		iEntityWrapper* pEnt = *itChildren;
		apFunctions->DebugDrawLine(mvPosition, pEnt->GetPosition(), cColor(0,1,0,1));
	}
}

//------------------------------------------------------------------

void iEntityWrapper::DrawProgram(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, iGpuProgram* apProg, const cColor& aCol)
{
	if(mpEngineEntity)
		mpEngineEntity->DrawProgram(apViewport, apFunctions, apProg, aCol);
}

//------------------------------------------------------------------

void iEntityWrapper::SnapToGrid()
{
	cVector3f vNewPos;
	UpdateEntity();
	vNewPos = cEditorGrid::GetSnappedPosInSpace(mvPosition);

	SetAbsPosition(vNewPos);
}

//------------------------------------------------------------------

void iEntityWrapper::UpdateMatrix()
{
	if(mbEntityUpdated)
	{
		if(mbTranslationUpdated)
		{
			mbTranslationUpdated=false;
			mmtxTranslate = cMath::MatrixTranslate(GetPosition());
		}
		if(mbRotationUpdated)
		{
			mbRotationUpdated=false;
			mmtxRotate = cMath::MatrixRotate(GetRotation(),eEulerRotationOrder_XYZ);
		}
		if(mbScaleUpdated)
		{
			mbScaleUpdated=false;
			mmtxScale = cMath::MatrixScale(GetScale());
		}

		mmtxTransform = cMath::MatrixMul(mmtxTranslate, cMath::MatrixMul(mmtxRotate,mmtxScale));

		mbEntityUpdated = false;
	}
}

void iEntityWrapper::UpdateEntity()
{
	if(mpCompoundObject)
		mpCompoundObject->SetComponentsUpdated();
	
	UpdateMatrix();

	CheckCulledByClipPlanes(GetEditorWorld()->GetClipPlanes());

	if(mpEngineEntity)
	{
		mpEngineEntity->Update();
		mpEngineEntity->SetMatrix(mmtxTransform);
		mpEngineEntity->UpdateVisibility();
	}
}

//------------------------------------------------------------------

bool iEntityWrapper::IsVisible()
{
	return mpType->IsVisible() && mbCulledByPlane==false && mbVisible && (FiltersByEditorRenderer()==false || IsInEditorRenderer());
}

bool iEntityWrapper::IsInEditorRenderer()
{
	iEditorWorld* pWorld = GetEditorWorld();
	if(pWorld==NULL || pWorld->GetWorld()==NULL || pWorld->GetShowOtherRendererObjects()) return true;
	return (static_cast<unsigned>(mlRendererMask) & pWorld->GetWorld()->GetRendererMaskBit()) != 0;
}

void iEntityWrapper::SetRendererMask(int alMask)
{
	mlRendererMask = static_cast<int>(hpl::SanitizeRendererMask(static_cast<unsigned>(alMask)));
	UpdateVisibility();
	if(GetEditorWorld()) GetEditorWorld()->SetVisibilityUpdated();
}

void iEntityWrapper::UpdateVisibility()
{
	if(mpEngineEntity) mpEngineEntity->UpdateVisibility();
}

//------------------------------------------------------------------

bool iEntityWrapper::IsCulledByFrustum(cCamera* apCamera)
{
	return mpEngineEntity!=NULL && mpEngineEntity->IsCulledByFrustum(apCamera);
}

//------------------------------------------------------------------

void iEntityWrapper::CheckCulledByClipPlanes(tEditorClipPlaneVec& avPlanes)
{
	bool bCulled = false;
	for(int i=0;i<(int)avPlanes.size();++i)
	{
		cEditorClipPlane* pPlane = avPlanes[i];
		if(pPlane->IsActive()==false)
			continue;

		bCulled = bCulled || EntitySpecificCheckCulled(pPlane);
		if(bCulled) break;
	}

	if(mbCulledByPlane==bCulled)
		return;

	mbCulledByPlane = bCulled;
	
	OnSetCulled(mbCulledByPlane);
}

//------------------------------------------------------------------

bool iEntityWrapper::EntitySpecificCheckCulled(cEditorClipPlane* apPlane)
{
	return apPlane->PointIsOnCullingSide(mvPosition);
}

//------------------------------------------------------------------

void iEntityWrapper::OnSetActive(bool abX)
{
	if(mpEngineEntity)
		mpEngineEntity->Update();
}

void iEntityWrapper::OnSetVisible(bool abX)
{
	if(mpEngineEntity)
		mpEngineEntity->UpdateVisibility();
}

void iEntityWrapper::OnSetCulled(bool abX)
{
	if(mpEngineEntity)
		mpEngineEntity->UpdateVisibility();
}


//------------------------------------------------------------------

cMeshEntity* iEntityWrapper::GetMeshEntity()
{
	if(mpEngineEntity==NULL)
		return NULL;

	return mpEngineEntity->GetMeshEntity();
}

//------------------------------------------------------------------

cBoundingVolume* iEntityWrapper::GetRenderBV()
{
	if(mpEngineEntity)
		return mpEngineEntity->GetRenderBV();
	
	return NULL;
}

//------------------------------------------------------------------

cBoundingVolume* iEntityWrapper::GetPickBV(cEditorWindowViewport* apViewport)
{
	if(mpEngineEntity)
		return mpEngineEntity->GetPickBV(apViewport);
	if(mpIcon)
		return mpIcon->GetPickBV(apViewport);
	
	return NULL;
}

//------------------------------------------------------------------

bool iEntityWrapper::EntitySpecificFilterCheck(bool abPassAll, bool abPassType)
{
	return abPassAll || abPassType;
}

//------------------------------------------------------------------

void iEntityWrapper::SetSelected(bool abX)
{
	if(mbSelected==abX)
		return;

	mbSelected = abX;
	OnSetSelected(mbSelected);
}

//------------------------------------------------------------------

cRect2l iEntityWrapper::GetClipRectangle(cEditorWindowViewport* apViewport)
{
	cCamera* pCam = apViewport->GetCamera();
	cFrustum* pFrustum = pCam->GetFrustum();
	cRect2l clipRect;
	cMath::GetClipRectFromBV(clipRect, *(mpEngineEntity->GetPickBV(apViewport)), pFrustum, apViewport->GetGuiViewportSizeInt(), pFrustum->GetFOV()*0.5f);

	return clipRect;
}

//------------------------------------------------------------------

bool iEntityWrapper::CheckRayIntersect(cEditorWindowViewport* apViewport, cVector3f* apPos, tVector3fVec* apTriangle, float* apT)
{
	if(mpEngineEntity)
		return mpEngineEntity->CheckRayIntersect(apViewport, apPos, apTriangle, apT);
	if(mpIcon)
		return mpIcon->CheckRayIntersect(apViewport, apPos, apTriangle, apT);

	return false;
}

bool iEntityWrapper::Check2DBoxIntersect(cEditorWindowViewport* apViewport, const cRect2l& aBox, cVector3f* apPos)
{
	if(mpEngineEntity && mpEngineEntity->Check2DBoxIntersect(apViewport, aBox) ||
		mpIcon && mpIcon->Check2DBoxIntersect(apViewport, aBox))
	{
		if(apPos)
			*apPos = mvPosition;

		return true;
	}

	return false;

	/*if(cMath::CheckRectIntersection(aBox, GetClipRectangle(apViewport)))
	{
		if(apPos)
			*apPos = mvPosition;

		return true;
	}

	return false;
	*/
}

//------------------------------------------------------------------

cVector3f iEntityWrapper::GetViewSpacePosition(cEditorWindowViewport* apViewport)
{
	return cMath::MatrixMul(apViewport->GetCamera()->GetViewMatrix(), mvPosition);
}

//------------------------------------------------------------------

void iEntityWrapper::SetName(const tString& asName)
{
	msName = asName;

	//if(mpEngineEntity)
	//	mpEngineEntity->SetName(asName);
}

//------------------------------------------------------------------

iEditorAction* iEntityWrapper::CreateModifyPositionAction(cEditorEditModeSelect* apEditMode, const cVector3f& avPos)
{
	cVector3f vAdd = avPos - mvPosition;

	return hplNew(cEditorActionSelectionTranslate,(apEditMode, vAdd, false));
}

//------------------------------------------------------------------

iEditorAction* iEntityWrapper::CreateModifyRotationAction(cEditorEditModeSelect* apEditMode, const cVector3f& avRotation)
{
	bool bZeroRotation = true;
	for(int i=0;i<3;++i)
	{
		if(cMath::Abs(mvRotation.v[i]-avRotation.v[i])>kEpsilonf)
		{
			bZeroRotation = false;
			break;
		}
	}

	if(bZeroRotation)
		return NULL;

	return hplNew(cEditorActionSelectionRotate,(apEditMode,GetEditorWorld(),avRotation,false,false));
}

//------------------------------------------------------------------

iEditorAction* iEntityWrapper::CreateModifyScaleAction(cEditorEditModeSelect* apEditMode,const cVector3f& avScale)
{	
	cVector3f vAdd = avScale - mvScale;
	bool bZeroScale = true;
	for(int i=0;i<3;++i)
	{
		if(cMath::Abs(mvScale.v[i]-avScale.v[i])>kEpsilonf)
		{
			bZeroScale = false;
			break;
		}
	}

	if(bZeroScale)
		return NULL;

	return hplNew(cEditorActionSelectionScale,(apEditMode,GetEditorWorld(),vAdd,false));
}

//------------------------------------------------------------------

bool iEntityWrapper::IsActive()
{
	return mbActive;
}

//------------------------------------------------------------------

void iEntityWrapper::SetActive(bool abX)
{
	mbActive = abX;
	OnSetActive(abX);
}

//------------------------------------------------------------------

void iEntityWrapper::SetCompoundObject(cEntityWrapperCompoundObject* apObject, bool abDetachFromOld)
{
	if(apObject==mpCompoundObject)
		return;

	cEntityWrapperCompoundObject* pOldObject = mpCompoundObject;
	if(pOldObject && abDetachFromOld)
		pOldObject->RemoveComponent(this);

	mpCompoundObject = apObject;
}

//------------------------------------------------------------------

int iEntityWrapper::GetCompoundObjectID()
{
	int lCompoundID = -1;
	if(mpCompoundObject) lCompoundID = mpCompoundObject->GetID();

	return lCompoundID;
}

//------------------------------------------------------------------

bool iEntityWrapper::CreateEngineEntity()
{
	if(mpEngineEntity) hplDelete(mpEngineEntity);

	mpEngineEntity = CreateSpecificEngineEntity();
	if(mpEngineEntity)
	{
		if(mpEngineEntity->Create(cString::ToString(GetID()))==false)
		{
			hplDelete(mpEngineEntity);		
			mpEngineEntity = NULL;

			return false;
		}
	}

	return true;
}

//------------------------------------------------------------------

void iEntityWrapper::DestroyEngineEntity()
{
	if(mpEngineEntity)
	{
		hplDelete(mpEngineEntity);
		mpEngineEntity = NULL;
	}
}

//------------------------------------------------------------------

void iEntityWrapper::GetWatchedFiles(std::vector<tWString>& avOut)
{
	iEditorWorld* pWorld = GetEditorWorld();

	//////////////////////////////////////////////////////////
	// Top-level source file (.ent for entities, mesh for static objects).
	if(msFilename.empty()==false)
	{
		cFileSearcher* pFS = pWorld->GetEditor()->GetEngine()->GetResources()->GetFileSearcher();
		tWString sFull = pFS->GetFilePath(msFilename);
		if(sFull.empty()==false)
			avOut.push_back(sFull);
	}

	//////////////////////////////////////////////////////////
	// Full dependency chain: mesh -> materials -> textures. The resolved
	// resources already carry their absolute paths, so no lookup is needed.
	cEditorFileWatcher* pWatcher = pWorld->GetFileWatcher();
	if(pWatcher==NULL || pWatcher->GetWatchDependencies()==false)
		return;

	cMeshEntity* pMeshEnt = GetMeshEntity();
	cMesh* pMesh = pMeshEnt ? pMeshEnt->GetMesh() : NULL;
	if(pMesh==NULL)
		return;

	if(pMesh->GetFullPath().empty()==false)
		avOut.push_back(pMesh->GetFullPath());

	for(int i=0; i<pMesh->GetSubMeshNum(); ++i)
	{
		cSubMesh* pSub = pMesh->GetSubMesh(i);
		cMaterial* pMat = pSub ? pSub->GetMaterial() : NULL;
		if(pMat==NULL)
			continue;

		if(pMat->GetFullPath().empty()==false)
			avOut.push_back(pMat->GetFullPath());

		for(int t=0; t<eMaterialTexture_LastEnum; ++t)
		{
			Image* pImg = pMat->GetImage((eMaterialTexture)t);
			if(pImg && pImg->GetFullPath().empty()==false)
				avOut.push_back(pImg->GetFullPath());
		}
	}
}

//------------------------------------------------------------------

void iEntityWrapper::RegisterFileWatches()
{
	cEditorFileWatcher* pWatcher = GetEditorWorld()->GetFileWatcher();
	if(pWatcher==NULL)
		return;

	// Clean re-sync: drop any previous watches for this entity first, so this
	// is safe to call again after a filename change or a reload that altered
	// the dependency set.
	pWatcher->UnregisterEntity(mlID);

	std::vector<tWString> vFiles;
	GetWatchedFiles(vFiles);

	for(size_t i=0; i<vFiles.size(); ++i)
		pWatcher->RegisterEntity(mlID, vFiles[i]);
}

//------------------------------------------------------------------

void iEntityWrapper::Save(tinyxml2::XMLElement* apParentElement)
{
	if(apParentElement==NULL)
	{
		Error("Failed saving of entity named %s of type %s - no parent element found\n", 
				GetName().c_str(), mpType->GetXmlElementName().c_str());
		return;
	}

	/////////////////////////////////////////////////////////////////////////////
	// Check if the entity is inside the boundaries, if not add to warning list
	const cVector3f& vPos = GetPosition();
	const cVector3f& vMaxPos = GetEditorWorld()->GetMaxPosition();
	const cVector3f& vMinPos = GetEditorWorld()->GetMinPosition();
	for(int i=0;i<3;++i)
	{
		if(vPos.v[i]>vMaxPos.v[i] || vPos.v[i]<vMinPos.v[i])
			GetEditorWorld()->AddOutlierEntity(this);
	}


	iEntityWrapperData* pData = CreateCopyData();
	if(pData->Save(apParentElement)==false)
		Error("Failed saving of entity named %s of type %s\n", 
				pData->GetName().c_str(), mpType->GetXmlElementName().c_str());

	hplDelete(pData);
}

//------------------------------------------------------------------

bool iEntityWrapper::GetProperty(int alPropID, int& alX)
{
	switch(alPropID)
	{
	case eObjInt_ID:
		alX = GetID();
		break;
	case eObjInt_RendererMask:
		alX = GetRendererMask();
		break;
	default:
		return false;
	}
	
	return true;
}

bool iEntityWrapper::GetProperty(int alPropID, float& afX)
{
	return false;	
}

bool iEntityWrapper::GetProperty(int alPropID, bool& abX)
{
	switch(alPropID)
	{
	case eObjBool_Active:
		abX = IsActive();
		break;
	//case eObjBool_Visible:
	//	abX = IsVisible();
	//	break;
	default:
		return false;
	}

	return true;
}

bool iEntityWrapper::GetProperty(int alPropID, tString& asX)
{
	switch(alPropID)
	{
	case eObjStr_Name:
		asX = GetName();
		break;
	case eObjStr_Tag:
		asX = GetTag();
		break;
	case eObjStr_GUID:
		asX = GUIDToHex(mlGUID);
		break;
	default:
		return false;
	}

	return true;
}

bool iEntityWrapper::GetProperty(int alPropID, cVector2f& avX)
{
	return false;
}

bool iEntityWrapper::GetProperty(int alPropID, cVector3f& avX)
{
	switch(alPropID)
	{
	case eObjVec3f_Position:
		avX = GetPosition();
		break;
	case eObjVec3f_Rotation:
		avX = GetRotation();
		break;
	case eObjVec3f_Scale:
		avX = GetScale();
		break;
	default:
		return false;
	}
	return true;
}

bool iEntityWrapper::GetProperty(int alPropID, cColor& aX)
{
	return false;
}

//------------------------------------------------------------------

bool iEntityWrapper::SetProperty(int alPropID, const int& alX)
{
	switch(alPropID)
	{
	case eObjInt_ID:
		SetID(alX);
		break;
	case eObjInt_RendererMask:
		SetRendererMask(alX);
		break;
	default:
		return false;
	}
	
	return true;
}

bool iEntityWrapper::SetProperty(int alPropID, const float& afX)
{
	return false;	
}

bool iEntityWrapper::SetProperty(int alPropID, const bool& abX)
{
	switch(alPropID)
	{
	case eObjBool_Active:
		SetActive(abX);
		break;
	//case eObjBool_Visible:
	//	SetVisible(abX);
	//	break;
	default:
		return false;
	}

	return true;
}

bool iEntityWrapper::SetProperty(int alPropID, const tString& asX)
{
	switch(alPropID)
	{
	case eObjStr_Name:
		SetName(asX);
		break;
	case eObjStr_Tag:
		SetTag(asX);
		break;
	case eObjStr_GUID:
	{
		// 0/absent = no opinion: keep the current GUID (protects XML edits
		// that omit the attribute from wiping it)
		unsigned long long lGUID = GUIDFromHex(asX);
		if(lGUID!=0)
			mlGUID = lGUID;
		break;
	}
	default:
		return false;
	}

	return true;
}

bool iEntityWrapper::SetProperty(int alPropID, const cVector2f& avX)
{
	return false;
}

bool iEntityWrapper::SetProperty(int alPropID, const cVector3f& avX)
{
	switch(alPropID)
	{
	case eObjVec3f_Position:
		SetAbsPosition(avX);
		break;
	case eObjVec3f_Rotation:
		SetAbsRotation(avX);
		break;
	case eObjVec3f_Scale:
		SetAbsScale(avX);
		break;
	default:
		return false;
	}
	return true;
}

bool iEntityWrapper::SetProperty(int alPropID, const cColor& aX)
{
	return false;
}

//------------------------------------------------------------------

iEditorAction* iEntityWrapper::CreateSetPropertyActionInt(int alPropID, int alX)
{
	iProp* pProp = mpType->GetPropByTypeAndID(eVariableType_Int, alPropID);
	cEditorActionSetInt* pAction = hplNew(cEditorActionSetInt,(GetEditorWorld(), tIntList(1,GetID()), 
																							pProp, alX));
	return SetUpAction(pAction);
}

iEditorAction* iEntityWrapper::CreateSetPropertyActionFloat(int alPropID, float afX)
{
	iProp* pProp = mpType->GetPropByTypeAndID(eVariableType_Float, alPropID);
	cEditorActionSetFloat* pAction = hplNew(cEditorActionSetFloat,(GetEditorWorld(), tIntList(1,GetID()), 
																							pProp, afX));

	return SetUpAction(pAction);
}

iEditorAction* iEntityWrapper::CreateSetPropertyActionBool(int alPropID, bool abX)
{
	iProp* pProp = mpType->GetPropByTypeAndID(eVariableType_Bool, alPropID);
	cEditorActionSetBool* pAction = hplNew(cEditorActionSetBool,(GetEditorWorld(), tIntList(1,GetID()), 
																							pProp, abX));
	return SetUpAction(pAction);
}

iEditorAction* iEntityWrapper::CreateSetPropertyActionString(int alPropID, const tString& asX)
{
	iProp* pProp = mpType->GetPropByTypeAndID(eVariableType_String, alPropID);
	cEditorActionSetString* pAction = hplNew(cEditorActionSetString,(GetEditorWorld(), tIntList(1,GetID()), 
																							pProp, asX));
	return SetUpAction(pAction);
}

iEditorAction* iEntityWrapper::CreateSetPropertyActionVector2f(int alPropID, const cVector2f& avX)
{
	iProp* pProp = mpType->GetPropByTypeAndID(eVariableType_Vec2, alPropID);
	cEditorActionSetVec2f* pAction = hplNew(cEditorActionSetVec2f,(GetEditorWorld(), tIntList(1,GetID()), 
																							pProp, avX));
	return SetUpAction(pAction);
}

iEditorAction* iEntityWrapper::CreateSetPropertyActionVector3f(int alPropID, const cVector3f& avX)
{
	iProp* pProp = mpType->GetPropByTypeAndID(eVariableType_Vec3, alPropID);
	cEditorActionSetVec3f* pAction = hplNew(cEditorActionSetVec3f,(GetEditorWorld(), tIntList(1,GetID()), 
																							pProp, avX));
	return SetUpAction(pAction);
}

iEditorAction* iEntityWrapper::CreateSetPropertyActionColor(int alPropID, const cColor& aX)
{
	iProp* pProp = mpType->GetPropByTypeAndID(eVariableType_Color, alPropID);
	cEditorActionSetColor* pAction = hplNew(cEditorActionSetColor,(GetEditorWorld(), tIntList(1,GetID()), 
																							pProp, aX));
	return SetUpAction(pAction);
}

//------------------------------------------------------------------

///////////////////////////////////////////////////////////////////
// PROTECTED METHODS
///////////////////////////////////////////////////////////////////

//------------------------------------------------------------------

iEditorAction* iEntityWrapper::SetUpAction(iEditorAction* apAction)
{
	if(apAction && apAction->IsValidAction()==false)
	{
		Error("Entity %s with type %s created invalid action \"%s\"\n", GetName().c_str(), GetType()->ToString().c_str(), apAction->GetName().c_str());
		hplDelete(apAction);
		return NULL;
	}

	return apAction;
}

//------------------------------------------------------------------

void iEntityWrapper::DrawArrow(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, const cMatrixf& amtxTransform, float afLength, bool abKeepConstantSize, const cVector2f& avHeadSizeRatio, const cColor& aCol,float afOrthoConstant, float afPerspConstant)
{
	if(abKeepConstantSize)
	{
		cCamera *pCam = apViewport->GetCamera();
		cVector3f vViewPos;
		switch(pCam->GetProjectionType())
		{
		case eProjectionType_Orthographic:
			afLength = pCam->GetOrthoViewSize().x * afOrthoConstant;
			break;
		case eProjectionType_Perspective:
			vViewPos = cMath::MatrixMul(pCam->GetViewMatrix(), amtxTransform.GetTranslation());
			afLength = cMath::Abs(vViewPos.z)/afPerspConstant;
			break;
		default:
			break;
		}
	}

	float fY = avHeadSizeRatio.y * afLength;
	float fXZ = avHeadSizeRatio.x * afLength;

	mvArrowQuads[0][0].pos = cVector3f(fXZ,-fY,fXZ);
	mvArrowQuads[0][2].pos = 0;
	mvArrowQuads[0][3].pos = 0;
	mvArrowQuads[0][1].pos = cVector3f(-fXZ,-fY,fXZ);

	mvArrowQuads[1][0].pos = cVector3f(fXZ,-fY,-fXZ);
	mvArrowQuads[1][1].pos = 0;
	mvArrowQuads[1][2].pos = 0;
	mvArrowQuads[1][3].pos = cVector3f(-fXZ,-fY,-fXZ);

	mvArrowQuads[2][0].pos = cVector3f(fXZ,-fY,-fXZ);
	mvArrowQuads[2][2].pos = cVector3f(0,0,0);
	mvArrowQuads[2][3].pos = cVector3f(0,0,0);
	mvArrowQuads[2][1].pos = cVector3f(fXZ,-fY,fXZ);
	
	mvArrowQuads[3][0].pos = cVector3f(-fXZ,-fY,-fXZ);
	mvArrowQuads[3][1].pos = cVector3f(0,0,0);
	mvArrowQuads[3][2].pos = cVector3f(0,0,0);
	mvArrowQuads[3][3].pos = cVector3f(-fXZ,-fY,fXZ);

	cMatrixf mtxWorld = cMath::MatrixMul(amtxTransform, cMath::MatrixTranslate(cVector3f(0,afLength,0)));;

	DebugDraw::DebugDrawOptions options;
	options.m_transform = mtxWorld;

	apFunctions->DebugDrawLine(cVector3f(0,-afLength,0), 0, aCol, options);
	// Legacy GL_QUADS took perimeter order [0][1][2][3]; DebugDraw::DrawQuad
	// expects strip order, so pass [0][1][3][2].
	for(int i=0; i<4;++i)
		apFunctions->DrawQuad(mvArrowQuads[i][0].pos, mvArrowQuads[i][1].pos, mvArrowQuads[i][3].pos, mvArrowQuads[i][2].pos, aCol, options);
}

//------------------------------------------------------------------
//------------------------------------------------------------------
//------------------------------------------------------------------


//-------------------------------------------------------------------
//-------------------------------------------------------------------
//-------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////
// CUSTOM VAR ENTITY DATA
/////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

iEntityWrapperTypeUserDefinedEntity::iEntityWrapperTypeUserDefinedEntity(int alID, const tWString& asName, const tString& asElementName, cEditorUserClassSubType* apUserType, const tString& asVarElementName) : iEntityWrapperType(alID, asName, asElementName)
{
	mpUserType = apUserType;
	msVarElementName = asVarElementName;
}


//-------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////
// CUSTOM VAR ENTITY DATA
/////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

iEntityWrapperDataUserDefinedEntity::iEntityWrapperDataUserDefinedEntity(iEntityWrapperType* apType, int alDefID) : iEntityWrapperData(apType)
{
	iEntityWrapperTypeUserDefinedEntity* pType = (iEntityWrapperTypeUserDefinedEntity*)apType;
	mpClass = pType->GetUserType()->CreateInstance(eEditorVarCategory_Instance);
}

//-------------------------------------------------------------------

iEntityWrapperDataUserDefinedEntity::~iEntityWrapperDataUserDefinedEntity()
{
	if(mpClass)
		hplDelete(mpClass);
}

//-------------------------------------------------------------------

void iEntityWrapperDataUserDefinedEntity::CopyFromEntity(iEntityWrapper* apEnt)
{
	iEntityWrapperData::CopyFromEntity(apEnt);
	
	iEntityWrapperUserDefinedEntity* pEnt = (iEntityWrapperUserDefinedEntity*)apEnt;
	if(mpClass)
		hplDelete(mpClass);
	mpClass = pEnt->GetClass()->CreateCopy();
}

//-------------------------------------------------------------------

void iEntityWrapperDataUserDefinedEntity::CopyToEntity(iEntityWrapper* apEntity, int alX)
{
	iEntityWrapperData::CopyToEntity(apEntity, alX);

	if(alX==ePropCopyStep_PostEnt)
	{
		iEntityWrapperUserDefinedEntity* pEnt = (iEntityWrapperUserDefinedEntity*)apEntity;
		if(mpClass)
			pEnt->SetClass(mpClass->CreateCopy());
	}
}

//-------------------------------------------------------------------

bool iEntityWrapperDataUserDefinedEntity::Load(tinyxml2::XMLElement* apElement)
{
	iEntityWrapperData::Load(apElement);
	iEntityWrapperTypeUserDefinedEntity* pType = (iEntityWrapperTypeUserDefinedEntity*)mpType;

	bool bSuccess = true;
	tString sError;

	///////////////////////////////////////////
	// Load var values from element
	const tString& sVarsElement = pType->GetUserVarElementName();
	tinyxml2::XMLElement* pVars = apElement->FirstChildElement(sVarsElement.c_str());
	if(pVars)
	{
		if(mpClass)
			mpClass->Load(pVars);
		else
		{
			bSuccess = false;
			sError = "No class created";
		}
	}
	else
	{
		bSuccess = false;
		sError = "No " + sVarsElement + " element found";
	}

	if(bSuccess==false)
		Log("Error loading user variables for object with type %ls, named %s, with ID %d - %s.\n", mpType->GetName().c_str(), GetName().c_str(), GetID(), sError.c_str());

	return bSuccess;
}

//-------------------------------------------------------------------

bool iEntityWrapperDataUserDefinedEntity::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	iEntityWrapperData::SaveSpecific(apElement);
	iEntityWrapperTypeUserDefinedEntity* pType = (iEntityWrapperTypeUserDefinedEntity*)mpType;

	if(mpClass)
	{
		tinyxml2::XMLElement* pVarsElement = apElement->GetDocument()->NewElement(pType->GetUserVarElementName().c_str());
		apElement->InsertEndChild(pVarsElement);
		mpClass->Save(pVarsElement);
	}

	return true;
}

//-------------------------------------------------------------------
//-------------------------------------------------------------------
//-------------------------------------------------------------------


//-------------------------------------------------------------------

iEntityWrapperUserDefinedEntity::iEntityWrapperUserDefinedEntity(iEntityWrapperData* apData) : iEntityWrapper(apData)
{
	mpClass = NULL;
}

//-------------------------------------------------------------------

iEntityWrapperUserDefinedEntity::~iEntityWrapperUserDefinedEntity()
{
	if(mpClass)
		hplDelete(mpClass);
}

//-------------------------------------------------------------------

void iEntityWrapperUserDefinedEntity::SetClass(cEditorClassInstance* apClass)
{
	if(mpClass)
		hplDelete(mpClass);

	mpClass = apClass;

	///////////////////////////////////////////////////
	// Update entity with values in class instance
	for(int i=0; i<mpClass->GetVarInstanceNum();++i)
	{
		cEditorVarInstance* pVar = mpClass->GetVarInstance(i);
		SetVar(pVar->GetName(), pVar->GetValue());
	}
}

//-------------------------------------------------------------------

cEditorVarInstance* iEntityWrapperUserDefinedEntity::GetVar(const tWString& asVarName)
{
	return mpClass->GetVarInstance(asVarName);
}

//-------------------------------------------------------------------

void iEntityWrapperUserDefinedEntity::SetVar(const tWString& asName, const tWString& asValue)
{
	mpClass->SetVarValue(asName, asValue);
	OnSetVar(asName, asValue);
}

//-------------------------------------------------------------------

iEditorAction* iEntityWrapperUserDefinedEntity::CreateActionSetUserType()
{
	//iEditorAction* pAction = hplNew(cEditorActionUserDefinedObjectSetType,(GetEditorWorld(), mlID, 

	return NULL;
}

//-------------------------------------------------------------------

iEditorAction* iEntityWrapperUserDefinedEntity::CreateActionSetUserVariable(const tWString& asName, const tWString& asValue)
{
	iEditorAction* pAction = hplNew(cEditorActionUserDefinedObjectSetVariable,(GetEditorWorld(), mlID, asName, asValue));
	if(pAction && pAction->IsValidAction())
		return pAction;

	hplDelete(pAction);
	return NULL;
}

//-------------------------------------------------------------------
//-------------------------------------------------------------------
//-------------------------------------------------------------------

iEntityWrapperTypeAggregate::iEntityWrapperTypeAggregate(int alType, const tWString& asName, 
														 const tString& asElementName, const tString& asComponentElementName,
														 bool abDestroyComponentsOnDeletion) : iEntityWrapperType(alType, asName, asElementName)
														 
{
	msComponentElementName = asComponentElementName;
	mbDestroyComponentsOnDeletion = abDestroyComponentsOnDeletion;
}

//-------------------------------------------------------------------

iEntityWrapperDataAggregate::iEntityWrapperDataAggregate(iEntityWrapperType* apType) : iEntityWrapperData(apType)
{
}

iEntityWrapperDataAggregate::~iEntityWrapperDataAggregate()
{
	STLDeleteAll(mvComponentsData);
}

//-------------------------------------------------------------------

void iEntityWrapperDataAggregate::CopyFromEntity(iEntityWrapper* apEntity)
{
	iEntityWrapperData::CopyFromEntity(apEntity);

	mvComponentsData.clear();

	iEntityWrapperAggregate* pEnt = (iEntityWrapperAggregate*)apEntity;
	tEntityWrapperList& lstComponents = (tEntityWrapperList&)pEnt->GetComponents();
	tEntityWrapperListIt it = lstComponents.begin();

	for(;it!=lstComponents.end();++it)
	{
		iEntityWrapper* pEnt = *it;
		iEntityWrapperData* pData = pEnt->GetType()->CreateData();
		pData->CopyFromEntity(pEnt);

		mvComponentsData.push_back(pData);
	}
}

//-------------------------------------------------------------------

void iEntityWrapperDataAggregate::CopyToEntity(iEntityWrapper* apEntity, int alCopyStep)
{
	iEntityWrapperData::CopyToEntity(apEntity, alCopyStep);

	if(alCopyStep==ePropCopyStep_PostDeployAll)
	{
		iEntityWrapperAggregate* pAggregate = (iEntityWrapperAggregate*)apEntity;
		iEditorWorld* pWorld = mpType->GetWorld();
		for(int i=0;i<(int)mvComponentsData.size();++i)
		{
			iEntityWrapperData* pData = mvComponentsData[i];
			
			iEntityWrapper* pSubEntity = pWorld->GetEntity(pData->GetID());
			if(pSubEntity==NULL)
				pSubEntity = pWorld->CreateEntityWrapperFromData(pData);

			pAggregate->AddComponent(pSubEntity);
		}
		pAggregate->UpdateRelativeTransforms();
	}
}

//-------------------------------------------------------------------

bool iEntityWrapperDataAggregate::Load(tinyxml2::XMLElement* apElement)
{
	if(iEntityWrapperData::Load(apElement)==false)
		return false;

	iEditorWorld* pWorld = mpType->GetWorld();
	const tString& sComponentName = ((iEntityWrapperTypeAggregate*)mpType)->GetComponentElementName();

	for(tinyxml2::XMLElement* pObj = apElement->FirstChildElement(); pObj != NULL; pObj = pObj->NextSiblingElement())
	{
		if(tString(pObj->Value())!=sComponentName)
			continue;
		iEntityWrapper* pEnt = pWorld->GetEntity(GetAttributeInt(pObj, "ID", -1));
		if(pEnt)
		{
			iEntityWrapperData* pData = pEnt->CreateCopyData();
			mvComponentsData.push_back(pData);
		}
	}

	return true;
}

//-------------------------------------------------------------------

bool iEntityWrapperDataAggregate::SaveSpecific(tinyxml2::XMLElement* apElement)
{
	if(iEntityWrapperData::SaveSpecific(apElement)==false)
		return false;

	const tString& sComponentName = ((iEntityWrapperTypeAggregate*)mpType)->GetComponentElementName();

	for(int i=0; i<(int)mvComponentsData.size();++i)
	{
		tinyxml2::XMLElement* pComponentElement = apElement->GetDocument()->NewElement(sComponentName.c_str());
		apElement->InsertEndChild(pComponentElement);
		SetAttributeInt(pComponentElement, "ID", mvComponentsData[i]->GetID());
	}

	return true;
}

//-------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////////
//
/////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

iEntityWrapperAggregate::iEntityWrapperAggregate(iEntityWrapperData* apData) : iEntityWrapper(apData)
{
}

//-------------------------------------------------------------------

iEntityWrapperAggregate::~iEntityWrapperAggregate()
{
}

void iEntityWrapperAggregate::OnDestroy()
{
	iEntityWrapperTypeAggregate* pType = (iEntityWrapperTypeAggregate*)mpType;
	if(GetEditorWorld()->IsClearingEntities()==false)
	{
		tEntityWrapperList lstEntities = mlstComponents;
		
		iEditorWorld* pWorld = GetEditorWorld();
		tEntityWrapperListIt it = lstEntities.begin();
		for(;it!=lstEntities.end();++it)
		{
			iEntityWrapper* pEnt = *it;
			if(pType->GetDestroyComponentsOnDeletion())	pWorld->DestroyEntityWrapper(pEnt);
			else RemoveComponent(pEnt);
		}
	}
}

//-------------------------------------------------------------------

tIntVec iEntityWrapperAggregate::GetCloneIDs()
{
	tIntVec vIDs;

	iEditorWorld* pWorld = GetEditorWorld();
	for(int i=0;i<(int)mlstComponents.size()+1;++i)
		vIDs.push_back(pWorld->GetFreeID());

	return vIDs;
}

void iEntityWrapperAggregate::SetUpCloneData(iEntityWrapperData* apData, const tIntVec& avIDs)
{
	iEditorWorld* pWorld = GetEditorWorld();
	iEntityWrapperDataAggregate* pCloneData = (iEntityWrapperDataAggregate*)apData;

	tEntityWrapperList lstComponentClones;
	tEntityDataVec vComponentsData;
	tEntityWrapperListIt itComponents = mlstComponents.begin();
	for(int i=0;itComponents!=mlstComponents.end();++itComponents,++i)
	{
		iEntityWrapper* pComponent = *itComponents;
		iEntityWrapperData* pComponentData = pComponent->CreateCopyData();
		pComponentData->SetCompoundID(-1);
		pComponentData->SetID(avIDs[i]);
		pComponentData->SetName(pWorld->GenerateName(pComponentData->GetName()));

		vComponentsData.push_back(pComponentData);

		iEntityWrapper* pComponentClone = pWorld->CreateEntityWrapperFromData(pComponentData);
		lstComponentClones.push_back(pComponentClone);
	}
	
	itComponents = lstComponentClones.begin();
	for(;itComponents!=lstComponentClones.end();++itComponents)
	{
		iEntityWrapper* pComponent = *itComponents;
		pComponent->OnPostDeployAll(false);
	}
	mbComponentsUpdated = false;

	pCloneData->SetComponentsData(vComponentsData);
}

//-------------------------------------------------------------------

bool iEntityWrapperAggregate::IsCulledByFrustum(cCamera* apCamera)
{
	return false;
}

bool iEntityWrapperAggregate::CheckRayIntersect(cEditorWindowViewport* apViewport, cVector3f* apPos, tVector3fVec* apTriangle, float* apT)
{
	////////////////////////////////////////////////////
	// Check intersection against all component shapes
	bool bIntersect = false;

	//float fMinSqrDistance = 999999999.0f;
	cVector3f vIntersectionPos;
	cVector3f vMinIntersectionPos;
	float fMinT = 99999.0f;
	const cVector3f& vRayStart = apViewport->GetUnprojectedStart();

	// Iterate through components
	tEntityWrapperListIt it = mlstComponents.begin();
	for(;it!=mlstComponents.end();++it)
	{
		iEntityWrapper* pEnt = *it;
		if(pEnt->IsCulledByFrustum(apViewport->GetCamera()))
			continue;

		float fT;
		// Check intersection with each
		if(pEnt->CheckRayIntersect(apViewport, &vIntersectionPos, NULL, &fT))
		{
			bIntersect = true;
			if(fT<fMinT)
			{
				fMinT = fT;
				vMinIntersectionPos = vIntersectionPos;
			}
		}
	}

	// If there is intersection, return minimum distance intersection (if possible)
	if(bIntersect)
	{
		if(apPos) *apPos = vMinIntersectionPos;
		if(apT) *apT = fMinT;
	}

	return bIntersect;
}

//-------------------------------------------------------------------

bool iEntityWrapperAggregate::Check2DBoxIntersect(cEditorWindowViewport* apViewport, const cRect2l& aBox, cVector3f* apPos)
{
	////////////////////////////////////////////////////
	// Check intersection against all component shapes
	bool bIntersect = false;

	float fMinDistance = 99999999.0f;
	cVector3f vMinIntersectionPos;
	const cVector3f& vRayStart = apViewport->GetUnprojectedStart();

	//////////////////////////////////////////////////////
	// Iterate through components
	tEntityWrapperListIt it = mlstComponents.begin();
	for(;it!=mlstComponents.end();++it)
	{
		iEntityWrapper* pEnt = *it;
		//////////////////////////////////////////
		// Check if outside the screen
		if(pEnt->IsCulledByFrustum(apViewport->GetCamera()))
			continue;

		//////////////////////////////////////////
		// Check intersection with each
		cVector3f vIntersectionPos;
		if(pEnt->Check2DBoxIntersect(apViewport, aBox, &vIntersectionPos))
		{
			bIntersect = true;

			////////////////////////////////////////
			// Update min distance
			float fDistance = cMath::Vector3DistSqr(vRayStart, vIntersectionPos);
			if(fDistance < fMinDistance)
			{
				fMinDistance = fDistance;
				vMinIntersectionPos = vIntersectionPos;
			}
		}
	}

	///////////////////////////////////////////////////////////
	// If there is intersection, return minimum distance intersection (if possible)
	if(bIntersect && apPos)
		*apPos = vMinIntersectionPos;

	return bIntersect;
}


//-------------------------------------------------------------------

void iEntityWrapperAggregate::AddComponent(iEntityWrapper* apEntity)
{
	if(apEntity==NULL)
		return;

	tEntityWrapperListIt it = find(mlstComponents.begin(), mlstComponents.end(), apEntity);
	if(it!=mlstComponents.end())
		return;
	
	OnAddComponent(apEntity);
	mlstComponents.push_back(apEntity);

	SetComponentsUpdated();
}

//-------------------------------------------------------------------

void iEntityWrapperAggregate::RemoveComponent(iEntityWrapper* apEntity)
{
	if(apEntity==NULL)
		return;

	tEntityWrapperListIt it = find(mlstComponents.begin(), mlstComponents.end(), apEntity);
	if(it==mlstComponents.end())
		return;

	OnRemoveComponent(apEntity);
	mlstComponents.erase(it);

	SetComponentsUpdated();
}

//-------------------------------------------------------------------

void iEntityWrapperAggregate::UpdateRelativeTransforms()
{
	if(mbComponentsUpdated==false)
		return;

	mbComponentsUpdated = false;

	cVector3f vMax = -999999999.9f;
	cVector3f vMin = 999999999.9f;

	////////////////////////////////////////////
	// Set up restrictions
	mbCanTranslate=true;
	mbCanRotate=true;
	mScaleType = eScaleType_Normal;
	mbCloneable = true;
	mbDeletable = true;

	for(tEntityWrapperListIt it = mlstComponents.begin();it!=mlstComponents.end();++it)
	{
		iEntityWrapper* pEnt = *it;
		
		///////////////////////////////////////////////
		// This is used to calculate the center of the aggregate later
		cMath::ExpandAABB(vMin,vMax, pEnt->GetPosition(),pEnt->GetPosition());

		mbCanTranslate = mbCanTranslate && pEnt->CanTranslate();
		mbCanRotate = mbCanRotate && pEnt->CanRotate();
		if(CanScale())
		{
			if(pEnt->IsScalableGrouped()==false || pEnt->GetScaleType()==eScaleType_None)
				mScaleType = eScaleType_None;
		}
		mbCloneable = mbCloneable && pEnt->IsCloneable();
		mbDeletable = mbDeletable && pEnt->IsDeletable();
	}

	/////////////////////////////////////
	// Set Object center
	SetAbsPosition((vMin + vMax)*0.5f);

	/////////////////////////////////////
	// Set up entity local matrices
	UpdateMatrix();
	cMatrixf mtxInvAggregateMatrix = cMath::MatrixInverse(mmtxTransform);

	mlstRelMatrices.clear();
	for(tEntityWrapperListIt it = mlstComponents.begin();it!=mlstComponents.end();++it)
	{
		iEntityWrapper* pComponent = *it;

		const cMatrixf& mtxComponentWorldMatrix = pComponent->GetWorldMatrix();
		cMatrixf mtxRelativeComponentMatrix = cMath::MatrixMul(mtxInvAggregateMatrix, mtxComponentWorldMatrix);
		
		mlstRelMatrices.push_back(mtxRelativeComponentMatrix);
		OnUpdateRelativeTransform(pComponent, mlstRelMatrices.back());
	}
}

//-------------------------------------------------------------------

void iEntityWrapperAggregate::UpdateEntity()
{
 	iEntityWrapper::UpdateEntity();

	UpdateRelativeTransforms();
	UpdateMatrix();
	tEntityWrapperListIt itEnts = mlstComponents.begin();
	tMatrixfListIt itRelMtx = mlstRelMatrices.begin();

	// Parent (aggregate) scale from the 3x3 column lengths. A non-uniform parent scale
	// combined with a rotated component produces a SHEARED world matrix, and
	// SetWorldMatrix recovers scale via column lengths (which can't represent shear) —
	// so the component's absolute Scale would drift a little on every save. When the
	// parent scale is non-uniform we pin the component scale to the stable component-wise
	// product (parentScale (x) relScale), the same invariant the runtime loader uses.
	// This is an exact no-op for axis-aligned components, so common data is unchanged.
	cVector3f vParentScale(mmtxTransform.GetRight().Length(),
						   mmtxTransform.GetUp().Length(),
						   mmtxTransform.GetForward().Length());
	bool bParentScaleNonUniform =
		cMath::Abs(vParentScale.x-vParentScale.y)>1e-4f ||
		cMath::Abs(vParentScale.y-vParentScale.z)>1e-4f ||
		cMath::Abs(vParentScale.x-vParentScale.z)>1e-4f;

	for(;itEnts!=mlstComponents.end();++itEnts, ++itRelMtx)
	{
		iEntityWrapper* pEnt = *itEnts;
		cMatrixf mtxRelMatrix = *itRelMtx;

		pEnt->SetWorldMatrix(cMath::MatrixMul(mmtxTransform,mtxRelMatrix));

		if(bParentScaleNonUniform)
		{
			cVector3f vRelScale(mtxRelMatrix.GetRight().Length(),
								mtxRelMatrix.GetUp().Length(),
								mtxRelMatrix.GetForward().Length());
			pEnt->SetAbsScale(cVector3f(vParentScale.x*vRelScale.x,
										vParentScale.y*vRelScale.y,
										vParentScale.z*vRelScale.z));
		}

		pEnt->UpdateEntity();
	}

	mbComponentsUpdated = false;
}

//-------------------------------------------------------------------

/*
void iEntityWrapperAggregate::Draw(cEditorWindowViewport* apViewport, DebugDraw* apFunctions, iEditorEditMode* apEditMode, bool abIsSelected)
{
	if(abIsSelected==false)
		return;

	for(tEntityWrapperListIt it = mlstEntities.begin();it!=mlstEntities.end();++it)
	{
		iEntityWrapper* pEnt = *it;
		cBoundingVolume* pBV = pEnt->GetPickBV(NULL);

		apFunctions->DebugDrawBoxMinMax(pBV->GetMin(), pBV->GetMax(), cColor(1,0,0,1));
	}

}
*/

//-------------------------------------------------------------------

void iEntityWrapperAggregate::SetAbsPosition(const cVector3f& avPosition)
{
	iEntityWrapper::SetAbsPosition(avPosition);

	mbComponentsUpdated = false;
}

//-------------------------------------------------------------------

void iEntityWrapperAggregate::SetAbsRotation(const cVector3f& avRotation)
{
	iEntityWrapper::SetAbsRotation(avRotation);

	mbComponentsUpdated = false;
}

//-------------------------------------------------------------------

void iEntityWrapperAggregate::SetAbsScale(const cVector3f& avScale, int alAxis)
{
	iEntityWrapper::SetAbsScale(avScale, alAxis);

	tEntityWrapperListIt it = mlstComponents.begin();
	for(;it!=mlstComponents.end();++it)
	{
		iEntityWrapper* pComponent = *it;
		pComponent->SetLastTransformedAxis(alAxis);
	}

	mbComponentsUpdated = false;
}

//-------------------------------------------------------------------
//-------------------------------------------------------------------
//-------------------------------------------------------------------

