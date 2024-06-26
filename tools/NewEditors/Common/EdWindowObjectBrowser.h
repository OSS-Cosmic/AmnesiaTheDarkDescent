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

#ifndef ED_WINDOW_OBJECT_BROWSER_H
#define ED_WINDOW_OBJECT_BROWSER_H

#include "../Common/EdScnObjCreatorPane.h"
#include "../Common/EdIndex.h"

//----------------------------------------------------------

class iEdBase;

class iEdObjectIndex;
class iEdObjectIndexEntry;

class cEdWindowObjectBrowser;

//----------------------------------------------------------

////////////////////////////////////////////////////////////
// BASE INDEX ENTRY FOR MESH OBJECTS
////////////////////////////////////////////////////////////

//----------------------------------------------------------

class iEdObjectIndexEntryMeshObject : public iEdObjectIndexEntry
{
public:
	iEdObjectIndexEntryMeshObject(iEdObjectIndexDir* apDir);

	bool CreateFromFile(const tWString& asFilename);
	bool CreateFromXmlElement(cXmlElement* apElement);

	const cVector3f& GetBVMin() { return mvBVMin; }
	const cVector3f& GetBVMax() { return mvBVMax; }
	int GetTriangleCount() { return mlTriangleCount; }

	virtual tWString& GetMeshFileName();

	void Save(cXmlElement* apElement);

	virtual cMeshEntity* CreateTempObject(cWorld* apWorld);
protected:
	void BuildThumbnail();

	cVector3f mvBVMin;
	cVector3f mvBVMax;
	int mlTriangleCount;

	tString msTempFileName;

	tWString msMeshFileName;
};


//----------------------------------------------------------

////////////////////////////////////////////////////////////
// OBJECT BROWSER WINDOW
////////////////////////////////////////////////////////////

//----------------------------------------------------------

class cEdWindowObjectBrowser : public iEdScnObjCreatorPane
{
public:
	cEdWindowObjectBrowser(iEdEditMode* apEditMode, const tWStringVec& avBaseDirs, bool abAddCategoryHeaders);
	~cEdWindowObjectBrowser();

	virtual iEdObjectIndexEntryMeshObject* GetSelectedObject();
	cMeshEntity* GetPreviewEntity() { return mpPreviewEntity; }

	void Reset();

	cMesh* GetSelectedMesh();
protected:
	void OnCreateLayout();

	bool ObjectSets_OnChange(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(ObjectSets_OnChange);

	bool ObjectList_OnChangeSelection(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(ObjectList_OnChangeSelection);

	bool Input_OnTextBoxEnter(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(Input_OnTextBoxEnter);

	bool Refresh_OnPressed(iWidget* apWidget, const cGuiMessageData& aData);
	kGuiCallbackDeclarationEnd(Refresh_OnPressed);

	void BuildObjectSetList();
	void BuildObjectSetListHelper(const tWString& asFolder, int alLevel);

	void BuildObjectList();
	void BuildObjectListHelper(const tWString& asBaseFolder, const tWString& asCurrentFolder, tWStringList& alstInvalidFiles);
	void ClearObjectList();
	virtual void UpdateObjectList();

	void WriteInvalidFileListToFile(tWString& asFolder, tWStringList& alstInvalidFiles);

	void UpdateObjectInfo();
	void AddEntriesInDirToList(iEdObjectIndexDir* apDir,std::vector<iEdObjectIndexEntryMeshObject*>& avEntries);

	iEdObjectIndex* CreateIndex(const tWString& asFolder);
	virtual iEdObjectIndex* CreateSpecificIndex(iEditor* apEditor, const tWString& asFolder)=0;

	virtual cMeshEntity* CreatePreviewEntity(iEdObjectIndexEntryMeshObject* apEntry)=0;

	////////////////////////////////////////////////////
	// Data
	tWStringVec mvBaseDirs;

	tWStringVec mvDirectories;

	// Layout stuff
	cWidgetGroup* mpSelectionGroup;
	cWidgetComboBox* mpObjectSets;
	cWidgetListBox* mpObjectList;
	cWidgetButton* mpButtonRefresh;

	cWidgetGroup* mpInfoGroup;
	cWidgetLabel* mvLabelBVSize[2];
	cWidgetLabel* mvLabelPolyCount[2];
	cWidgetLabel* mpLabelThumbnail;
	cWidgetImage* mpThumbnail;

	tWStringVec mvCategoryStrings;

	bool mbAddCategoryHeaders;

	cMeshEntity* mpPreviewEntity;

	std::map<tWString, iEdObjectIndex*> mmapObjectIndices;
	iEdObjectIndex* mpCurrentIndex;
	std::vector<iEdObjectIndexEntryMeshObject*> mvCurrentListedEntries;

	tIntVec mvDirIndices;

	static cWorld* mpTempWorld;
};

//----------------------------------------------------------

#endif // ED_WINDOW_OBJECT_BROWSER_H
