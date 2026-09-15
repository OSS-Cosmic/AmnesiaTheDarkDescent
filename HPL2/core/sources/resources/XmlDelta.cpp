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

#include "resources/XmlDelta.h"

#include "resources/XmlHelper.h"
#include "scene/LightParameters.h"
#include "system/LowLevelSystem.h"
#include "system/Platform.h"
#include "system/String.h"

#include <tinyxml2.h>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace hpl {

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// HELPERS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	// The two spellings of the per-object variable block: maps use
	// <UserVariables>, .ent files use <UserDefinedVariables>.
	static const char* gsUserVarNames[] = { "UserVariables", "UserDefinedVariables" };

	static tinyxml2::XMLElement* FindUserVarElement(tinyxml2::XMLElement* apElement)
	{
		for(size_t i=0; i<sizeof(gsUserVarNames)/sizeof(gsUserVarNames[0]); ++i)
		{
			tinyxml2::XMLElement* pVars = apElement->FirstChildElement(gsUserVarNames[i]);
			if(pVars) return pVars;
		}
		return NULL;
	}

	static tinyxml2::XMLElement* GetOrCreateUserVarElement(tinyxml2::XMLElement* apElement)
	{
		tinyxml2::XMLElement* pVars = FindUserVarElement(apElement);
		if(pVars) return pVars;

		pVars = apElement->GetDocument()->NewElement(gsUserVarNames[0]);
		apElement->InsertEndChild(pVars);
		return pVars;
	}

	//-----------------------------------------------------------------------

	// Copies every attribute of apSource onto apDest, except any listed in
	// apSkip (a NULL-terminated array).
	static void CopyAttributes(	tinyxml2::XMLElement* apDest, const tinyxml2::XMLElement* apSource,
								const char** apSkip)
	{
		for(const tinyxml2::XMLAttribute* pAttr = apSource->FirstAttribute(); pAttr; pAttr = pAttr->Next())
		{
			bool bSkip = false;
			for(int i=0; apSkip && apSkip[i]; ++i)
			{
				if(tString(pAttr->Name()) == apSkip[i]) { bSkip = true; break; }
			}
			if(bSkip) continue;

			apDest->SetAttribute(pAttr->Name(), pAttr->Value());
		}
	}

	//-----------------------------------------------------------------------

	// The element holding the object categories: <MapContents> for a map,
	// <ModelData> for an .ent. Falls back to the passed root so a delta can also
	// be applied to a bare contents element.
	static tinyxml2::XMLElement* GetContentsElement(tinyxml2::XMLElement* apTargetRoot)
	{
		tinyxml2::XMLElement* pContents = apTargetRoot->FirstChildElement("MapContents");
		if(pContents) return pContents;

		pContents = apTargetRoot->FirstChildElement("ModelData");
		if(pContents) return pContents;

		return apTargetRoot;
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PATCH CONTEXT
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	// Renderers an object loads for: its RendererMask attribute, else the light
	// element default (Overdrive lights load only on Overdrive), else all.
	static unsigned int GetElementRendererMask(const tinyxml2::XMLElement* apElement)
	{
		if(apElement->Attribute("RendererMask"))
			return SanitizeRendererMask(static_cast<unsigned int>(apElement->IntAttribute("RendererMask", 0)));
		const cLightElementInfo info = GetLightElementInfo(apElement->Value());
		return info.mbValid ? GetDefaultLightRendererMask(info) : kRendererMaskAll;
	}

	//-----------------------------------------------------------------------

	// Lazily-built lookup over the base document, so a delta with a handful of
	// operations does not rescan a 3 MB map for every one of them.
	class cXmlDeltaContext
	{
	public:
		cXmlDeltaContext(tinyxml2::XMLElement* apTargetRoot)
			: mpTargetRoot(apTargetRoot), mpContents(GetContentsElement(apTargetRoot)), mbNamesBuilt(false)
		{
		}

		tinyxml2::XMLElement* GetContents() { return mpContents; }
		tinyxml2::XMLElement* GetTargetRoot() { return mpTargetRoot; }

		//////////////////////////////////////
		// Category element, optionally created if missing.
		tinyxml2::XMLElement* GetCategory(const tString& asCategory, bool abCreate)
		{
			tinyxml2::XMLElement* pCat = mpContents->FirstChildElement(asCategory.c_str());
			if(pCat==NULL && abCreate)
			{
				pCat = mpContents->GetDocument()->NewElement(asCategory.c_str());
				mpContents->InsertEndChild(pCat);
			}
			return pCat;
		}

		//////////////////////////////////////
		// Object by (category, ID). NULL if the category or the ID is missing.
		tinyxml2::XMLElement* GetObject(const tString& asCategory, int alID)
		{
			tIDMap& mapIDs = GetIDMap(asCategory);
			tIDMap::iterator it = mapIDs.find(alID);
			return it==mapIDs.end() ? NULL : it->second;
		}

		//////////////////////////////////////
		// Keep the lazy index in step with what we mutate.
		// Call before the element is deleted. Builds the name index first so the
		// base name counts are taken before anything is removed.
		void OnObjectRemoved(const tString& asCategory, int alID, tinyxml2::XMLElement* apElement)
		{
			if(mmapCategories.count(asCategory)) mmapCategories[asCategory].erase(alID);
			BuildNames();
			tString sName = cString::ToLowerCase(GetAttributeString(apElement, "Name", ""));
			if(sName=="") return;
			tElementVec& vSame = mmapNames[sName];
			vSame.erase(std::remove(vSame.begin(), vSame.end(), apElement), vSame.end());
			mvAdded.erase(std::remove(mvAdded.begin(), mvAdded.end(), apElement), mvAdded.end());
		}

		void OnObjectAdded(const tString& asCategory, int alID, const tString& asName, tinyxml2::XMLElement* apElement)
		{
			if(mmapCategories.count(asCategory)) mmapCategories[asCategory][alID] = apElement;
			BuildNames();
			if(asName!="") mmapNames[cString::ToLowerCase(asName)].push_back(apElement);
			mvAdded.push_back(apElement);
		}

		bool IsNameInUse(const tString& asName)
		{
			if(asName=="") return false;
			BuildNames();
			std::map<tString, tElementVec>::const_iterator it = mmapNames.find(cString::ToLowerCase(asName));
			return it!=mmapNames.end() && it->second.empty()==false;
		}

		//////////////////////////////////////
		// True when apNew may share asName with the objects already using it: it
		// carries an explicit RendererMask, and no renderer it loads for ends up
		// with the name more often than the base document had it. The original
		// kept for Standard and its replacement added for Overdrive load as one
		// object per backend, so the name stays unambiguous at runtime; a name the
		// base file already used twice may stay at two. Masks are read as they
		// are when the object is added, so the <Modify> that narrows the original
		// must come before the <Add>.
		bool CanShareNameByRendererMask(const tString& asName, const tinyxml2::XMLElement* apNew)
		{
			if(apNew->Attribute("RendererMask")==NULL || GetElementRendererMask(apNew)==0) return false;
			return FitsBaseNameCount(asName, apNew, 1);
		}

		//////////////////////////////////////
		// A later <Modify> can widen a mask after the <Add> was accepted. Drops
		// every added object whose name is now loaded more often than the base
		// file had it for some renderer.
		void DropAddedObjectsWithOverlappingMasks(cXmlDeltaStats& aStats)
		{
			for(size_t i=0; i<mvAdded.size();)
			{
				tinyxml2::XMLElement* pAdded = mvAdded[i];
				tString sName = GetAttributeString(pAdded, "Name", "");
				if(sName=="" || FitsBaseNameCount(sName, pAdded, 0)) { ++i; continue; }

				Error("Map delta: added object '%s' shares its name with an object loaded for the same renderer. Dropping it.\n",
						sName.c_str());
				tinyxml2::XMLElement* pCat = pAdded->Parent()->ToElement();
				OnObjectRemoved(pCat->Value(), GetAttributeInt(pAdded, "ID", -1), pAdded);
				pCat->DeleteChild(pAdded);
				--aStats.mlAdded;
				++aStats.mlSkipped;
			}
		}

	private:
		typedef std::map<int, tinyxml2::XMLElement*> tIDMap;
		typedef std::vector<tinyxml2::XMLElement*> tElementVec;

		// alExtra objects about to join the name (1 for a pending <Add>, 0 when
		// apObject is already indexed) are counted for every renderer in
		// apObject's mask.
		bool FitsBaseNameCount(const tString& asName, const tinyxml2::XMLElement* apObject, size_t alExtra)
		{
			BuildNames();
			const tString sLowerName = cString::ToLowerCase(asName);
			const tElementVec& vSame = mmapNames[sLowerName];
			const size_t lAllowed = std::max<size_t>(mmapBaseNameCounts[sLowerName], 1);
			const unsigned int lMask = GetElementRendererMask(apObject);

			static const unsigned int vBits[] = { kRendererMaskStandard, kRendererMaskOverdrive };
			for(size_t i=0; i<sizeof(vBits)/sizeof(vBits[0]); ++i)
			{
				if((lMask & vBits[i])==0) continue;
				size_t lCount = alExtra;
				for(size_t j=0; j<vSame.size(); ++j)
				{
					if(GetElementRendererMask(vSame[j]) & vBits[i]) ++lCount;
				}
				if(lCount > lAllowed) return false;
			}
			return true;
		}

		tIDMap& GetIDMap(const tString& asCategory)
		{
			std::map<tString, tIDMap>::iterator it = mmapCategories.find(asCategory);
			if(it!=mmapCategories.end()) return it->second;

			tIDMap& mapIDs = mmapCategories[asCategory];
			tinyxml2::XMLElement* pCat = mpContents->FirstChildElement(asCategory.c_str());
			if(pCat)
			{
				for(tinyxml2::XMLElement* pObj = pCat->FirstChildElement(); pObj; pObj = pObj->NextSiblingElement())
				{
					int lID = GetAttributeInt(pObj, "ID", -1);
					if(lID>=0) mapIDs[lID] = pObj;
				}
			}
			return mapIDs;
		}

		void BuildNames()
		{
			if(mbNamesBuilt) return;
			mbNamesBuilt = true;

			for(tinyxml2::XMLElement* pCat = mpContents->FirstChildElement(); pCat; pCat = pCat->NextSiblingElement())
			{
				for(tinyxml2::XMLElement* pObj = pCat->FirstChildElement(); pObj; pObj = pObj->NextSiblingElement())
				{
					tString sName = cString::ToLowerCase(GetAttributeString(pObj, "Name", ""));
					if(sName=="") continue;
					mmapNames[sName].push_back(pObj);
					++mmapBaseNameCounts[sName];
				}
			}
		}

		tinyxml2::XMLElement* mpTargetRoot;
		tinyxml2::XMLElement* mpContents;

		std::map<tString, tIDMap> mmapCategories;

		bool mbNamesBuilt;
		std::map<tString, tElementVec> mmapNames;
		std::map<tString, size_t> mmapBaseNameCounts;
		tElementVec mvAdded;
	};

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// OPERATIONS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	// Resolves the object an op points at, and checks the optional Name / GUID
	// witnesses. A mismatch means the base map has changed under the delta, so
	// the op is refused rather than silently applied to the wrong object.
	static tinyxml2::XMLElement* ResolveOpTarget(	cXmlDeltaContext& aContext, tinyxml2::XMLElement* apOp,
													const tString& asOpName, tString& asCategory, int& alID)
	{
		asCategory = GetAttributeString(apOp, "Category", "");
		alID = GetAttributeInt(apOp, "ID", -1);

		if(asCategory=="" || alID<0)
		{
			Warning("Map delta: <%s> needs both a Category and an ID.\n", asOpName.c_str());
			return NULL;
		}

		tinyxml2::XMLElement* pObj = aContext.GetObject(asCategory, alID);
		if(pObj==NULL)
		{
			Warning("Map delta: <%s> target %s ID %d does not exist in the base file.\n",
					asOpName.c_str(), asCategory.c_str(), alID);
			return NULL;
		}

		tString sWantedName = GetAttributeString(apOp, "Name", "");
		if(sWantedName!="")
		{
			tString sActualName = GetAttributeString(pObj, "Name", "");
			if(cString::ToLowerCase(sActualName) != cString::ToLowerCase(sWantedName))
			{
				Warning("Map delta: <%s> target %s ID %d is named '%s', delta expected '%s'. Skipping.\n",
						asOpName.c_str(), asCategory.c_str(), alID, sActualName.c_str(), sWantedName.c_str());
				return NULL;
			}
		}

		tString sWantedGUID = GetAttributeString(apOp, "GUID", "");
		if(sWantedGUID!="")
		{
			tString sActualGUID = GetAttributeString(pObj, "GUID", "");
			if(sActualGUID!="" && cString::ToLowerCase(sActualGUID) != cString::ToLowerCase(sWantedGUID))
			{
				Warning("Map delta: <%s> target %s ID %d has GUID '%s', delta expected '%s'. Skipping.\n",
						asOpName.c_str(), asCategory.c_str(), alID, sActualGUID.c_str(), sWantedGUID.c_str());
				return NULL;
			}
		}

		return pObj;
	}

	//-----------------------------------------------------------------------

	// A removed static object must also leave any combine group it was part of,
	// or cWorldLoaderHplMap::CreateStaticObjectCombo warns once per missing id.
	static void RemoveIDFromStaticObjectCombos(cXmlDeltaContext& aContext, int alID)
	{
		tinyxml2::XMLElement* pCombos = aContext.GetContents()->FirstChildElement("StaticObjectCombos");
		if(pCombos==NULL) return;

		for(tinyxml2::XMLElement* pCombo = pCombos->FirstChildElement(); pCombo; pCombo = pCombo->NextSiblingElement())
		{
			tString sObjIds = GetAttributeString(pCombo, "ObjIds", "");
			if(sObjIds=="") continue;

			tIntVec vObjIds;
			cString::GetIntVec(sObjIds, vObjIds);

			tString sNew = "";
			bool bFound = false;
			for(size_t i=0; i<vObjIds.size(); ++i)
			{
				if(vObjIds[i]==alID) { bFound = true; continue; }
				if(sNew!="") sNew += " ";
				sNew += cString::ToString(vObjIds[i]);
			}

			if(bFound) SetAttributeString(pCombo, "ObjIds", sNew);
		}
	}

	//-----------------------------------------------------------------------

	// Every loader prefers an object's file index over its literal path
	// attribute whenever the index is present (see CreateStaticObjectEntity,
	// CreateDecal and LoadEntity in WorldLoaderHplMap.cpp). So repointing an
	// existing object at a different mesh, entity or material only takes effect
	// once the competing index attribute is gone -- otherwise the delta is
	// silently ignored at load time.
	static void DropSupersededFileIndex(tinyxml2::XMLElement* apObj, tinyxml2::XMLElement* apSetAttr)
	{
		if(apSetAttr->Attribute("Filename") || apSetAttr->Attribute("MeshFilename"))
			apObj->DeleteAttribute("FileIndex");

		if(apSetAttr->Attribute("Material"))
			apObj->DeleteAttribute("MaterialIndex");
	}

	//-----------------------------------------------------------------------

	static void ApplyVarOps(tinyxml2::XMLElement* apTarget, tinyxml2::XMLElement* apOp)
	{
		for(tinyxml2::XMLElement* pSub = apOp->FirstChildElement(); pSub; pSub = pSub->NextSiblingElement())
		{
			tString sOp = pSub->Value();

			//////////////////////////////
			// Upsert one <Var>, leaving the rest of the block untouched
			if(sOp=="SetVar")
			{
				tString sName = GetAttributeString(pSub, "Name", "");
				if(sName=="")
				{
					Warning("Map delta: <SetVar> without a Name.\n");
					continue;
				}
				tString sValue = GetAttributeString(pSub, "Value", "");

				tinyxml2::XMLElement* pVars = GetOrCreateUserVarElement(apTarget);
				tinyxml2::XMLElement* pVar = NULL;
				for(tinyxml2::XMLElement* pIt = pVars->FirstChildElement("Var"); pIt; pIt = pIt->NextSiblingElement("Var"))
				{
					if(GetAttributeString(pIt, "Name", "") == sName) { pVar = pIt; break; }
				}

				if(pVar==NULL)
				{
					pVar = pVars->GetDocument()->NewElement("Var");
					SetAttributeString(pVar, "Name", sName);
					pVars->InsertEndChild(pVar);
				}
				SetAttributeString(pVar, "Value", sValue);
			}
			//////////////////////////////
			// Drop one <Var>
			else if(sOp=="RemoveVar")
			{
				tString sName = GetAttributeString(pSub, "Name", "");
				tinyxml2::XMLElement* pVars = FindUserVarElement(apTarget);
				if(sName=="" || pVars==NULL) continue;

				for(tinyxml2::XMLElement* pIt = pVars->FirstChildElement("Var"); pIt; pIt = pIt->NextSiblingElement("Var"))
				{
					if(GetAttributeString(pIt, "Name", "") == sName)
					{
						pVars->DeleteChild(pIt);
						break;
					}
				}
			}
		}
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	bool ApplyXmlDelta(	tinyxml2::XMLElement* apTargetRoot, tinyxml2::XMLElement* apDeltaRoot,
						int& alNextAddID, cXmlDeltaStats& aStats)
	{
		if(apTargetRoot==NULL || apDeltaRoot==NULL) return false;

		int lVersion = GetAttributeInt(apDeltaRoot, "Version", 1);
		if(lVersion > 1)
		{
			Error("Map delta '%s' has format version %d, this build understands 1.\n",
					GetAttributeString(apDeltaRoot, "Name", "?").c_str(), lVersion);
			return false;
		}

		cXmlDeltaContext context(apTargetRoot);

		for(tinyxml2::XMLElement* pOp = apDeltaRoot->FirstChildElement(); pOp; pOp = pOp->NextSiblingElement())
		{
			tString sOp = pOp->Value();

			////////////////////////////////////
			// Attribute overrides on <MapData> / the .ent root: fog, skybox, ...
			if(sOp=="SetMapData")
			{
				CopyAttributes(apTargetRoot, pOp, NULL);
				++aStats.mlModified;
			}
			////////////////////////////////////
			// Variable ops straight on the root (an .ent's <UserDefinedVariables>)
			else if(sOp=="SetVar" || sOp=="RemoveVar")
			{
				// Wrap so ApplyVarOps sees it as a one-entry child list.
				tinyxml2::XMLDocument tempDoc;
				tinyxml2::XMLElement* pWrapper = tempDoc.NewElement("Modify");
				tempDoc.InsertEndChild(pWrapper);
				pWrapper->InsertEndChild(pOp->DeepClone(&tempDoc));

				ApplyVarOps(apTargetRoot, pWrapper);
				++aStats.mlModified;
			}
			////////////////////////////////////
			// Drop an existing object
			else if(sOp=="Remove")
			{
				tString sCategory; int lID;
				tinyxml2::XMLElement* pObj = ResolveOpTarget(context, pOp, "Remove", sCategory, lID);
				if(pObj==NULL) { ++aStats.mlSkipped; continue; }

				if(sCategory=="StaticObjects") RemoveIDFromStaticObjectCombos(context, lID);

				context.OnObjectRemoved(sCategory, lID, pObj);
				context.GetCategory(sCategory, false)->DeleteChild(pObj);

				++aStats.mlRemoved;
			}
			////////////////////////////////////
			// Patch attributes and user variables of an existing object
			else if(sOp=="Modify")
			{
				tString sCategory; int lID;
				tinyxml2::XMLElement* pObj = ResolveOpTarget(context, pOp, "Modify", sCategory, lID);
				if(pObj==NULL) { ++aStats.mlSkipped; continue; }

				static const char* vSkip[] = { "ID", NULL };
				for(tinyxml2::XMLElement* pSub = pOp->FirstChildElement("SetAttr"); pSub;
					pSub = pSub->NextSiblingElement("SetAttr"))
				{
					CopyAttributes(pObj, pSub, vSkip);
					DropSupersededFileIndex(pObj, pSub);
				}

				//////////////////////////////
				// Attribute removal, for when an editor re-save drops an attribute
				// the base file had. ID is never removable: it is the key every
				// other operation matches on.
				for(tinyxml2::XMLElement* pSub = pOp->FirstChildElement("RemoveAttr"); pSub;
					pSub = pSub->NextSiblingElement("RemoveAttr"))
				{
					tString sAttrName = GetAttributeString(pSub, "Name", "");
					if(sAttrName=="" || sAttrName=="ID")
					{
						Warning("Map delta: <RemoveAttr> needs a Name, and cannot remove ID.\n");
						continue;
					}
					pObj->DeleteAttribute(sAttrName.c_str());
				}

				ApplyVarOps(pObj, pOp);

				++aStats.mlModified;
			}
			////////////////////////////////////
			// Splice in new objects
			else if(sOp=="Add")
			{
				tString sCategory = GetAttributeString(pOp, "Category", "");
				if(sCategory=="")
				{
					Warning("Map delta: <Add> without a Category.\n");
					++aStats.mlSkipped;
					continue;
				}

				tinyxml2::XMLElement* pCat = context.GetCategory(sCategory, true);

				// After="ID" places the added objects, in order, right behind that
				// object instead of at the end of the category. Loaders that keep
				// children by position (an .ent's lights) then find a replacement
				// where the object it stands in for was.
				tinyxml2::XMLElement* pInsertAfter = NULL;
				if(pOp->Attribute("After"))
				{
					pInsertAfter = context.GetObject(sCategory, GetAttributeInt(pOp, "After", -1));
					if(pInsertAfter==NULL)
						Warning("Map delta: <Add After=\"%s\"> names no object in %s. Appending instead.\n",
								GetAttributeString(pOp, "After", "").c_str(), sCategory.c_str());
				}

				for(tinyxml2::XMLElement* pNew = pOp->FirstChildElement(); pNew; pNew = pNew->NextSiblingElement())
				{
					// FileIndex/MaterialIndex refer to the base map's index tables,
					// which a delta must not renumber. Added objects carry literal
					// paths instead -- every loader accepts them when the index is
					// absent or negative.
					if(pNew->Attribute("FileIndex") || pNew->Attribute("MaterialIndex"))
					{
						Warning("Map delta: added '%s' uses FileIndex/MaterialIndex; use a literal Filename/MeshFilename/Material instead. Skipping.\n",
								GetAttributeString(pNew, "Name", "?").c_str());
						++aStats.mlSkipped;
						continue;
					}

					tString sName = GetAttributeString(pNew, "Name", "");
					if(context.IsNameInUse(sName) && context.CanShareNameByRendererMask(sName, pNew)==false)
					{
						Error("Map delta: added object name '%s' is already used in the base map. Skipping.\n", sName.c_str());
						++aStats.mlSkipped;
						continue;
					}

					tinyxml2::XMLNode* pClone = pNew->DeepClone(pCat->GetDocument());
					tinyxml2::XMLElement* pAdded = pClone->ToElement();

					int lNewID = alNextAddID++;
					SetAttributeInt(pAdded, "ID", lNewID);

					if(pInsertAfter)
					{
						pCat->InsertAfterChild(pInsertAfter, pAdded);
						pInsertAfter = pAdded;
					}
					else
					{
						pCat->InsertEndChild(pAdded);
					}
					context.OnObjectAdded(sCategory, lNewID, sName, pAdded);

					++aStats.mlAdded;
				}
			}
			////////////////////////////////////
			// Unknown
			else
			{
				Warning("Map delta: unknown operation <%s>.\n", sOp.c_str());
				++aStats.mlSkipped;
			}
		}

		context.DropAddedObjectsWithOverlappingMasks(aStats);

		return true;
	}

	//-----------------------------------------------------------------------

	int GetXmlDeltaPriority(tinyxml2::XMLElement* apDeltaRoot)
	{
		return GetAttributeInt(apDeltaRoot, "Priority", 0);
	}

	//-----------------------------------------------------------------------

	unsigned long long HashFileSet(const tWStringVec& avFiles)
	{
		if(avFiles.empty()) return 0;

		const unsigned long long lPrime = 1099511628211ULL;
		unsigned long long lHash = 14695981039346656037ULL;

		for(size_t i=0; i<avFiles.size(); ++i)
		{
			FILE *pFile = cPlatform::OpenFile(avFiles[i], _W("rb"));
			if(pFile==NULL) continue;

			char vBuffer[4096];
			size_t lRead;
			while((lRead = fread(vBuffer, 1, sizeof(vBuffer), pFile)) > 0)
			{
				for(size_t j=0; j<lRead; ++j)
				{
					lHash ^= (unsigned char)vBuffer[j];
					lHash *= lPrime;
				}
			}
			fclose(pFile);
		}

		return lHash;
	}

	//-----------------------------------------------------------------------
}
