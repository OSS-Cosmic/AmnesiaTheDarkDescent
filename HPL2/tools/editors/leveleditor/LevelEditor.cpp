/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or
 modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see
 <https://www.gnu.org/licenses/>.
 */

#include "hpl.h"
using namespace hpl;

#include "LevelEditor.h"
#include "LevelEditorCameraCapture.h"
#include "LevelEditorMCPServer.h"

#include <cstdlib> // getenv

#include "resources/XmlHelper.h"
#include <tinyxml2.h>

#include "../common/DirectoryHandler.h"
#include "../common/EditorFileWatcher.h"
#include "../common/PrefabManager.h"

#include "../common/EditorActionHandler.h"

#include "../common/EditorEditModeAreas.h"
#include "../common/EditorEditModeBillboards.h"
#include "../common/EditorEditModeCombine.h"
#include "../common/EditorEditModeDecals.h"
#include "../common/EditorEditModeEntities.h"
#include "../common/EditorEditModeFogAreas.h"
#include "../common/EditorEditModeLights.h"
#include "../common/EditorEditModeParticleSystems.h"
#include "../common/EditorEditModePrimitive.h"
#include "../common/EditorEditModeSelect.h"
#include "../common/EditorEditModeSounds.h"
#include "../common/EditorEditModeStaticObjects.h"

#include "../common/EditorWindowEditModeSidebar.h"
#include "../common/EditorWindowEntityEditBox.h"
#include "../common/EditorWindowEntitySearch.h"
#include "../common/EditorWindowFactory.h"
#include "../common/EditorWindowLowerToolbar.h"
#include "../common/EditorWindowOptions.h"
#include "../common/EditorWindowSelect.h"
#include "../common/EditorWindowStaticObjects.h"
#include "../common/EditorWindowTextureBrowser.h"
#include "../common/EditorWindowViewport.h"

#include "../common/EditorClipPlane.h"
#include "../common/EditorGrid.h"
#include "../common/EditorSelection.h"

#include "../common/EditorActionCompoundObject.h"
#include "../common/EditorHelper.h"
#include "../common/EditorUserClassDefinitionManager.h"
#include "../common/EntityWrapper.h"
#include "../common/EntityWrapperCompoundObject.h"

#include "../common/EngineEntity.h"

#include "LevelEditorActions.h"
#include "LevelEditorEntFileSession.h"
#include "LevelEditorWindowGroup.h"
#include "LevelEditorWindowHierarchy.h"
#include "LevelEditorWindowLevelSettings.h"
#include "LevelEditorWorld.h"

#include <algorithm>

unsigned int cLevelEditorGroup::mlGroupCounter = 1;

//--------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////
// GROUP
///////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------

cLevelEditorGroup::cLevelEditorGroup() {}

//--------------------------------------------------------------------

cLevelEditorGroup::cLevelEditorGroup(cLevelEditor *apEditor, unsigned int alID,
                                     const tString &asName) {
  mpEditor = apEditor;
  mlID = alID;
  msName = asName;
}

//--------------------------------------------------------------------

void cLevelEditorGroup::SetVisibility(bool abX) {
  mbVisibility = abX;

  tIntList lstEntityIDs;
  tEntityWrapperMapIt it = mpEditor->GetEditorWorld()->GetEntities().begin();

  for (; it != mpEditor->GetEditorWorld()->GetEntities().end(); ++it) {
    iEntityWrapper *pEnt = it->second;
    cLevelEditorEntityExtData *pData =
        (cLevelEditorEntityExtData *)pEnt->GetEntityExtData();

    if (pData->mlGroupID == mlID)
      pEnt->SetVisible(abX);
  }
}

//--------------------------------------------------------------------
//--------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////
// LEVEL EDITOR
///////////////////////////////////////////////////////////////////////
//--------------------------------------------------------------------
//--------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
///////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------

cLevelEditor::cLevelEditor() : iEditorBase(_W("Maps"), _W("*.map")) {
  mpMCPServer = NULL;
  mpCameraCapture = NULL;
  mbMCPEnabled = true;
  mlMCPPort = 8787;
  mpWindowHierarchy = NULL;
  mpScopedSession = NULL;
  mlScopedEntityID = -1;
  mpScopedEntityBox = NULL;
  mlPendingScopeEnter = -1;
  mbPendingScopeExit = false;
}

cLevelEditor::~cLevelEditor() {
  // Stop the MCP server first so no queued tool call runs against a
  // half-destroyed editor (its dtor joins the listener + worker threads, and
  // error-fulfils any parked deferred capture promises).
  if (mpMCPServer) {
    hplDelete(mpMCPServer);
    mpMCPServer = NULL;
  }

  // Then the camera capture (frees its headless viewport/camera/GPU resources;
  // the scene is still alive here — it is torn down later in ~iEditorBase).
  if (mpCameraCapture) {
    hplDelete(mpCameraCapture);
    mpCameraCapture = NULL;
  }

  // Ent-file editing sessions own headless cModelEditorWorlds that live in the
  // engine scene — tear them down here, while the scene is still alive (it is
  // destroyed later, in ~iEditorBase).
  ClearEntFileSessions();

  // (The prefab manager is owned by iEditorBase and destroyed there, AFTER the
  // world — entity wrappers hold resource handles into it. The MCP server is
  // already down at that point, so no queued tool call can touch it.)

  OnSaveConfig();
}

//--------------------------------------------------------------------

void cLevelEditor::ClearEntFileSessions() {
  // Map is being abandoned (new/load/dtor): tear down without flushing edits
  // and without the UI refresh a normal exit does (the caller resets the
  // world/UI).
  TeardownScope(false, false);
}

//--------------------------------------------------------------------

void cLevelEditor::TeardownScope(bool abSave, bool abRestoreUI) {
  if (mpScopedSession == NULL)
    return;

  // Drop the on-demand entity-properties box (if shown). Its edit-mode points
  // at the session Select mode we're about to free, so destroy it before the
  // session.
  if (mpScopedEntityBox) {
    mpScopedEntityBox->SetActive(false);
    cEditorWindowFactory::DestroyEditorWindow(
        (iEditorWindow *)mpScopedEntityBox);
    mpScopedEntityBox = NULL;
  }

  // User "Return to World": flush pending edits into the pending-ent cache (+
  // live reload of placed instances). Skipped on map reset/dtor.
  if (abSave)
    mpScopedSession->Save(true);

  // Drop selection first (empties it BEFORE the mode switch below, so the
  // session Select mode's OnSetCurrent(false) doesn't push a clear-action onto
  // the stack we are about to wipe). Fires OnSelectionChange -> hierarchy
  // clears its hosted box.
  GetSelection()->ClearEntities();

  // Leave the (now-doomed) session Select mode and restore the map's Select
  // mode BEFORE deleting the session, so SetCurrent(false) resolves against a
  // live world.
  SetCurrentEditMode(GetEditMode("Select"));

  // Destroy every scoped mode's helper window (each window's mpEditMode points into
  // the session we're about to free). SetCurrent(false) above already deactivated the
  // one that was current.
  const std::vector<iEditorEditMode *> &vScopeModes = mpScopedSession->GetEditModes();
  for (size_t i = 0; i < vScopeModes.size(); ++i)
    if (vScopeModes[i]->GetEditorWindow())
      cEditorWindowFactory::DestroyEditorWindow(vScopeModes[i]->GetEditorWindow());

  // The session's edit modes are about to be freed — rebuild the toolbar back
  // to the map modes so no button's user-data points at a dangling mode.
  if (mpEditModeSidebar)
    mpEditModeSidebar->RebuildButtons(GetEditModes(), false);

  // Session edits pushed world-bound actions onto the shared undo stack; clear
  // it so nothing in the history references the world we're about to delete.
  if (mpActionHandler)
    mpActionHandler->Reset();

  // Belt-and-suspenders: ensure no hosted sub-entity panel points into the
  // session.
  if (mpWindowHierarchy)
    mpWindowHierarchy->CloseScopeUI();

  hplDelete(mpScopedSession);
  mpScopedSession = NULL;
  mlScopedEntityID = -1;

  if (abRestoreUI) {
    if (mpWindowHierarchy)
      mpWindowHierarchy->RefreshTree();
    SetLayoutNeedsUpdate(true);
  }
}

//--------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
///////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------

void cLevelEditor::AppSpecificReset() {
  ///////////////////////////////////////
  // Clear state
  // This sets select as the current edit mode.
  SetCurrentEditMode(GetEditMode("Select"));

  ///////////////////////////////////////
  // Viewport Config
  // Reset all view ports to their presets
  for (int i = 0; i < 4; ++i) {
    mvViewports[i]->SetPreset((eEditorWindowViewportPreset)i);
  }

  ///////////////////////////////////////
  // Groups
  // Remove all previously created groups and create a "None" group.
  mmapGroups.clear();
  cLevelEditorGroup group(this, 0, "None");
  group.SetVisibility(true);
  mmapGroups.insert(pair<unsigned int, cLevelEditorGroup>(0, group));

  ///////////////////////////////////////
  // Pending MCP-defined entity files belong to the map that defined them —
  // a reset (new/load) without a save discards them.
  ClearPendingEntFiles();

  ///////////////////////////////////////
  // Ent-file editing sessions belong to the map being closed — exit any scope
  // and drop them so the next map builds fresh sessions on demand.
  ClearEntFileSessions();
}

//--------------------------------------------------------------------

iEditorWorld *cLevelEditor::GetActiveEditorWorld() {
  // THE canonical "which world does the shared edit machinery act on" accessor:
  // edit modes, edit boxes, selection (via cEditorSelection::GetActiveWorld),
  // actions and viewport picking all resolve through here. While scoped into a
  // placed .ent it returns the scope session's sub-entity world, else the map.
  if (mpScopedSession)
    return mpScopedSession->GetWorld();
  return GetEditorWorld();
}

//--------------------------------------------------------------------

void cLevelEditor::EnterEntFileScope(int alEntityID) {
  // Callback-safe: only record the request. The real work runs in OnUpdate ->
  // ProcessPendingScope (see the header note on widget-callback re-entrancy).
  mlPendingScopeEnter = alEntityID;
  mbPendingScopeExit = false;
}

//--------------------------------------------------------------------

void cLevelEditor::ExitEntFileScope() {
  // Callback-safe: only record the request (consumed in OnUpdate).
  mbPendingScopeExit = true;
  mlPendingScopeEnter = -1;
}

//--------------------------------------------------------------------

void cLevelEditor::ProcessPendingScope() {
  // Run at most one transition per frame, outside any widget callback. Exit
  // wins if both are somehow set (DoEnter also tears down any existing scope
  // first anyway).
  if (mbPendingScopeExit) {
    mbPendingScopeExit = false;
    TeardownScope(true, true);
  }

  if (mlPendingScopeEnter >= 0) {
    int lID = mlPendingScopeEnter;
    mlPendingScopeEnter = -1;
    DoEnterEntFileScope(lID);
  }
}

//--------------------------------------------------------------------

void cLevelEditor::DoEnterEntFileScope(int alEntityID) {
  iEntityWrapper *pEnt = mpEditorWorld->GetEntity(alEntityID);
  if (pEnt == NULL)
    return;

  // Only placed model (.ent) entities carry editable embedded sub-objects.
  tString sFile = pEnt->GetFilename();
  if (cString::ToLowerCase(cString::GetFileExt(sFile)) != "ent")
    return;

  // Re-scoping from an already-scoped state: leave the current scope first
  // (flush its edits), so there is only ever one transient session.
  if (mpScopedSession)
    TeardownScope(true, false);

  // Build the transient session and mount its sub-entities onto the placed
  // model's world transform (so their icons / pick volumes overlay it in the
  // map viewport).
  cLevelEditorEntFileSession *pSession =
      hplNew(cLevelEditorEntFileSession, (this, sFile));
  pSession->SetMountMatrix(pEnt->GetWorldMatrix());
  if (pSession->Load() == false) {
    hplDelete(pSession);
    ShowMessageBox(_W("Edit Contents"),
                   _W("Could not load this entity's .ent file for editing. See "
                      "the log for details."),
                   _W("OK"), _W(""), NULL, NULL);
    return;
  }

  // Clear the outgoing MAP selection BEFORE flipping the scope flag below.
  // Ordering is load-bearing: the selection resolver
  // (cEditorSelection::GetActiveWorld -> GetActiveEditorWorld) keys off
  // mpScopedSession, so it must still resolve to the map here for the placed
  // model to be de-highlighted (once mpScopedSession is set it would resolve to
  // the session, which does not own the map model, and the highlight would linger).
  GetSelection()->ClearEntities();

  mpScopedSession = pSession;
  mlScopedEntityID = alEntityID;

  // Fresh undo history for the scoped session.
  if (mpActionHandler)
    mpActionHandler->Reset();

  // Session modes are never added to the editor's mvEditModes, so OnInitLayout's
  // CreateWindow() loop never built their helper windows. Build one per mode here so
  // the Select "Object Type" filter panel + each creator's subtype panel (light type,
  // shape type, joint type, …) are available while scoped. Each is created inactive;
  // SetCurrentEditMode below activates only the mode that becomes current.
  const std::vector<iEditorEditMode *> &vScopeModes = pSession->GetEditModes();
  for (size_t i = 0; i < vScopeModes.size(); ++i) {
    iEditorWindow *pWin = vScopeModes[i]->CreateWindow();
    if (pWin)
      pWin->SetPosition(GetLayoutVec3f(eLayoutVec3_EditModeWindowPos));
  }

  // Viewport picking + icon drawing now follow the session world (its Select
  // mode becomes current); swap the toolbar to the session's modes + a Return
  // button.
  SetCurrentEditMode(pSession->GetSelectMode());
  if (mpEditModeSidebar)
    mpEditModeSidebar->RebuildButtons(pSession->GetEditModes(), true);

  if (mpWindowHierarchy)
    mpWindowHierarchy->RefreshTree();
  SetLayoutNeedsUpdate(true);
}

//--------------------------------------------------------------------

void cLevelEditor::ShowScopedEntityProperties() {
  // Sidebar "Entity" button. On-demand + toggle: only while scoped; a second
  // click (box already up) hides it.
  if (mpScopedSession == NULL)
    return;

  if (mpScopedEntityBox) {
    mpScopedEntityBox->SetActive(false);
    cEditorWindowFactory::DestroyEditorWindow(
        (iEditorWindow *)mpScopedEntityBox);
    mpScopedEntityBox = NULL;
    return;
  }

  iEntityWrapper *pEnt = mpEditorWorld->GetEntity(mlScopedEntityID);
  if (pEnt == NULL)
    return;

  // Free the shared right-pane slot: drop any session sub-entity selection
  // (which tears down the session Select mode's sub-entity box) so the two
  // never overlap.
  GetSelection()->ClearEntities();

  // Build the placed entity's OWN box (General/Entity), hosted directly by the
  // editor. The edit-mode arg is the session Select mode (used only as the
  // box's mpEditMode; property edits resolve against pEnt, a map-world
  // wrapper). Because the box's editor is this (scoped) editor, its Create()
  // suppresses the Edit Contents / Model Editor buttons. Position it in the
  // right-pane slot, mirroring CreateEditBoxWindow's tail.
  cEditorWindowEntityEditBox *pBox =
      pEnt->CreateEditBox(mpScopedSession->GetSelectMode());
  if (pBox == NULL)
    return;

  pBox->Init();
  AddWindow(pBox);
  pBox->SetPosition(GetLayoutVec3f(eLayoutVec3_EditModeWindowPos) +
                    cVector3f(0, 30, 0));
  pBox->SetActive(true);

  mpScopedEntityBox = pBox;
}

//--------------------------------------------------------------------

void cLevelEditor::LookAtEntity(int alEntityID) {
  iEntityWrapper *pEntity = mpEditorWorld->GetEntity(alEntityID);

  if (pEntity == NULL)
    return;

  for (int i = 0; i < 4; ++i) {
    mvViewports[i]->LookAtEntity(pEntity);
  }
}

//--------------------------------------------------------------------

void cLevelEditor::Command_Import() {
  ShowLoadFilePicker(mvLoadFilenames, msLastImportPath, this,
                     kGuiCallback(ImportFileCallback), _W("Exported objects"),
                     tWStringList(1, _W("*.expobj")));
}

//--------------------------------------------------------------------

void cLevelEditor::Command_Export() {
  SetFlags(eEditorFlag_PopUpActive, true);

  cGuiPopUpFilePicker *pPicker = mpSet->CreatePopUpSaveFilePicker(
      msLastImportFile, _W("Exported objects"), _W("*.expobj"), msLastSavePath,
      false, this, kGuiCallback(ExportFileCallback));
  pPicker->AddOnDestroyCallback(this, kGuiCallback(PopUpCloseCallback));
}

//--------------------------------------------------------------------

bool cLevelEditor::SaveToFile(const tWString &asPath) {
  if (asPath.empty())
    return false;
  msSaveFilename = asPath;
  Save();
  return true;
}

//--------------------------------------------------------------------

bool cLevelEditor::LoadFromFile(const tWString &asPath) {
  if (cPlatform::FileExists(asPath) == false)
    return false;
  mvLoadFilenames.clear();
  mvLoadFilenames.push_back(asPath);
  Load();
  return true;
}

//--------------------------------------------------------------------

int cLevelEditor::RefreshResourceIndex(const tWString &asSingleDir) {
  cResources *pResources = mpEngine->GetResources();
  size_t lBefore = pResources->GetFileSearcher()->GetAllFiles().size();

  if (asSingleDir.empty() == false) {
    if (cPlatform::FolderExists(asSingleDir) == false)
      return -1;
    pResources->AddResourceDir(asSingleDir, true);
  } else {
    // Replay the startup resource setup (iEditorBase::Init + lookup dirs).
    // AddDirectory is idempotent per (name, path), so re-adding existing
    // dirs only appends genuinely new files.
#ifdef USERDIR_RESOURCES
    pResources->LoadResourceDirsFile("resources.cfg",
                                     mpDirHandler->GetUserResourceDir());
#else
    tString sCustomResources =
        mpMainConfig->GetString("Directories", "ResourcesOverride", "");
    if (sCustomResources != "")
      pResources->LoadResourceDirsFile(sCustomResources);
    else
      pResources->LoadResourceDirsFile("resources.cfg");
#endif
    mpDirHandler->ForceRefreshLookupDirs();
  }

  size_t lAfter = pResources->GetFileSearcher()->GetAllFiles().size();
  return (int)(lAfter - lBefore);
}

//--------------------------------------------------------------------

// The pending in-memory .ent documents live in the iEditorBase-owned
// cPrefabManager; these keep their public names (used by the MCP tools + the
// entity load path) and just delegate. The manager also owns the prefab change
// broadcast the world subscribes to, so definition edits fan out live.

tinyxml2::XMLElement *
cLevelEditor::GetPendingEntFileRoot(const tString &asFile) {
  return GetPrefabManager()->GetPendingRoot(asFile);
}

//--------------------------------------------------------------------

void cLevelEditor::SetPendingEntFile(const tString &asPath,
                                     tinyxml2::XMLDocument *apDoc,
                                     bool abBroadcast) {
  GetPrefabManager()->SetPendingDoc(asPath, apDoc, abBroadcast);
}

//--------------------------------------------------------------------

int cLevelEditor::FlushPendingEntFiles() { return GetPrefabManager()->Flush(); }

//--------------------------------------------------------------------

int cLevelEditor::GetPendingEntFileDirtyCount() {
  return GetPrefabManager()->DirtyCount();
}

//--------------------------------------------------------------------

void cLevelEditor::ClearPendingEntFiles() {
  GetPrefabManager()->DiscardAllPending();
}

//--------------------------------------------------------------------

void cLevelEditor::OnPostSave() {
  // Harvest a live scoped ent-file session into its pending doc BEFORE flushing, so
  // edits made while still scoped (e.g. a light intensity change the 0.25s hierarchy
  // debounce hasn't caught yet) persist to the .ent on this map save. Save(false): no
  // broadcast needed here — the map .map is already written and we're about to flush
  // this doc to disk anyway; skip the ReloadEntities churn during the save flow.
  if (mpScopedSession && mpScopedSession->IsDirty())
    mpScopedSession->Save(false);

  int lWritten = FlushPendingEntFiles();
  if (lWritten > 0)
    Log("[MCP] flushed %d pending entity file(s) to disk on map save\n",
        lWritten);
}

//--------------------------------------------------------------------

void cLevelEditor::RestartMCPServer() {
  if (mpMCPServer) {
    hplDelete(mpMCPServer);
    mpMCPServer = NULL;
  }
  if (mbMCPEnabled) {
    mpMCPServer = hplNew(cLevelEditorMCPServer,
                         (this, "127.0.0.1", mlMCPPort, msMCPToken));
    if (mpMCPServer->Start() == false) {
      hplDelete(mpMCPServer);
      mpMCPServer = NULL;
    }
  }
  mpEngine->SetWaitIfAppOutOfFocus(mpMCPServer == NULL);
}

//--------------------------------------------------------------------

void cLevelEditor::OpenInModelEditor(const tString &asEntFile) {
  ////////////////////////////////////////////////////////////////////
  // The ModelEditor binary is packaged next to this one, so resolve it
  // relative to the editor working dir (which already ends in a separator).
  tWString sExe = GetWorkingDir() + _W("ModelEditor");
#if defined(_WIN32)
  sExe += _W(".exe");
#endif

  ////////////////////////////////////////////////////////////////////
  // Hand the ModelEditor the file to open plus this editor's MCP endpoint,
  // so it can push a live-reload notification back here on save. Only pass
  // the endpoint when the MCP server is actually enabled.
  tString sParams = asEntFile;
  if (mbMCPEnabled) {
    sParams += " --mcp-port " + cString::ToString(mlMCPPort);
    if (msMCPToken.empty() == false)
      sParams += " --mcp-token " + msMCPToken;
  }

  Log("[ModelEditor] launching '%ls' with params '%s'\n", sExe.c_str(),
      sParams.c_str());

  if (cPlatform::RunProgram(sExe, cString::To16Char(sParams)) == false)
    Error("[ModelEditor] failed to launch '%ls'\n", sExe.c_str());
}

//--------------------------------------------------------------------

void cLevelEditor::OpenInParticleEditor(const tString &asPsFile) {
  ////////////////////////////////////////////////////////////////////
  // The ParticleEditor binary is packaged next to this one, so resolve it
  // relative to the editor working dir (which already ends in a separator).
  tWString sExe = GetWorkingDir() + _W("ParticleEditor");
#if defined(_WIN32)
  sExe += _W(".exe");
#endif

  ////////////////////////////////////////////////////////////////////
  // Hand the ParticleEditor the file to open plus this editor's MCP endpoint,
  // so it can push a live-reload notification back here on save. Only pass
  // the endpoint when the MCP server is actually enabled.
  tString sParams = asPsFile;
  if (mbMCPEnabled) {
    sParams += " --mcp-port " + cString::ToString(mlMCPPort);
    if (msMCPToken.empty() == false)
      sParams += " --mcp-token " + msMCPToken;
  }

  Log("[ParticleEditor] launching '%ls' with params '%s'\n", sExe.c_str(),
      sParams.c_str());

  if (cPlatform::RunProgram(sExe, cString::To16Char(sParams)) == false)
    Error("[ParticleEditor] failed to launch '%ls'\n", sExe.c_str());
}

//--------------------------------------------------------------------

void cLevelEditor::GetMCPState(bool &abEnabled, int &alPort, tString &asToken,
                               tString &asStatus) {
  abEnabled = mbMCPEnabled;
  alPort = mlMCPPort;
  asToken = msMCPToken;

  if (mpMCPServer && mpMCPServer->IsRunning())
    asStatus = "listening on http://127.0.0.1:" + cString::ToString(mlMCPPort) +
               "/mcp";
  else if (mbMCPEnabled)
    asStatus = "enabled but not running (port in use?)";
  else
    asStatus = "disabled";
}

//--------------------------------------------------------------------

void cLevelEditor::ApplyMCPConfig(bool abEnabled, int alPort,
                                  const tString &asToken) {
  mbMCPEnabled = abEnabled;
  if (alPort > 0 && alPort <= 65535)
    mlMCPPort = alPort;
  msMCPToken = asToken;
  RestartMCPServer();
}

//--------------------------------------------------------------------

int cLevelEditor::GetMCPClientCount() {
  return cLevelEditorMCPServer::GetClientCount();
}

tString cLevelEditor::GetMCPClientLabel(int alIdx) {
  return cLevelEditorMCPServer::GetClientLabel(alIdx);
}

tString cLevelEditor::GetMCPClientSnippet(int alIdx) {
  return cLevelEditorMCPServer::BuildClientSnippet(alIdx, "127.0.0.1",
                                                   mlMCPPort, msMCPToken);
}

//--------------------------------------------------------------------

void cLevelEditor::AddGroup(unsigned int alID, const tString &asName) {
  cLevelEditorGroup group(this, alID, asName);
  group.SetVisibility(true);

  mmapGroups.insert(pair<unsigned int, cLevelEditorGroup>(alID, group));
}

//--------------------------------------------------------------------

cLevelEditorGroup &cLevelEditor::GetGroup(unsigned int alID) {
  return mmapGroups[alID];
}

//--------------------------------------------------------------------

void cLevelEditor::RemoveGroup(unsigned int alID) { mmapGroups.erase(alID); }

//--------------------------------------------------------------------

void cLevelEditor::SetUpClassDefinitions(
    cEditorUserClassDefinitionManager *apManager) {
  // eEditorVarCategory_Type is included so the MCP create_entity_file tool can
  // author .ent files through a headless cModelEditorWorld (its class instance
  // is created with the Type category, like the ModelEditor). One registration
  // carries all flags; registering the file twice would double-load it.
  apManager->RegisterDefFilename(eUserClassDefinition_Entity, "EntityTypes.cfg",
                                 eEditorVarCategory_Instance |
                                     eEditorVarCategory_EditorSetup |
                                     eEditorVarCategory_Type);
  apManager->RegisterDefFilename(eUserClassDefinition_Area, "AreaTypes.cfg",
                                 eEditorVarCategory_Instance |
                                     eEditorVarCategory_EditorSetup);
}

//--------------------------------------------------------------------

///////////////////////////////////////////////////////////////////////
// PROTECTED METHODS
///////////////////////////////////////////////////////////////////////

//--------------------------------------------------------------------

bool cLevelEditor::MainMenu_ItemClick(iWidget *apWidget,
                                      const cGuiMessageData &aData) {
  ////////////////////////////////
  // Update Delete, Clone and Compound menu items
  // Recognise the current mode as a Select mode by TYPE, not identity: while
  // scoped into a placed .ent the current mode is the session's own
  // cEditorEditModeSelect (bound to the session world, never in mvEditModes),
  // so an identity compare against GetEditMode("Select") would miss it and
  // silently drop Delete/Clone/Compound while scoped.
  // CreateDelete/CloneEntitiesAction use the mode's bound world, so this
  // correctly targets the scoped sub-entities.
  if (cEditorEditModeSelect *pEditMode =
          dynamic_cast<cEditorEditModeSelect *>(mpCurrentEditMode)) {
    tIntList lstEntityIDs = mpSelection->GetEntityIDs();

    if (apWidget == mpMainMenuDelete)
      AddAction(pEditMode->CreateDeleteEntitiesAction(lstEntityIDs));
    else if (apWidget == mpMainMenuClone)
      AddAction(pEditMode->CreateCloneEntitiesAction(lstEntityIDs));
    else if (apWidget == mpMainMenuCompound) {
      if (mpSelection->GetNumEntities() == 1 &&
          mpSelection->GetEntities().front()->GetTypeID() ==
              eEditorEntityType_Compound) {
        cEntityWrapperCompoundObject *pObj =
            (cEntityWrapperCompoundObject *)mpSelection->GetEntities().front();
        tIntList lstSubEntityIDs;
        cEditorHelper::GetIDsFromEntityList(pObj->GetComponents(),
                                            lstSubEntityIDs);
        // Active world so an un-compound while scoped hits the session world,
        // not the map.
        AddAction(hplNew(cEditorActionCompoundObjectRemoveEntities,
                         (GetActiveEditorWorld(), lstSubEntityIDs)));
      } else
        AddAction(pEditMode->CreateCompoundObjectAction());
    }
  }

  ///////////////////////////////////////////////
  // Menu Item "File.New"
  if (apWidget == mpMainMenuNew) {
    Command_New();
  }
  ///////////////////////////////////////////////
  // Menu Item "Save"
  else if (apWidget == mpMainMenuSave) {
    Command_Save();
  }
  ///////////////////////////////////////////////
  // Menu Item "File.Save As"
  else if (apWidget == mpMainMenuSaveAs) {
    Command_SaveAs();
  }
  ///////////////////////////////////////////////
  // Menu Item "File.Open"
  else if (apWidget == mpMainMenuLoad) {
    Command_Load();
  }
  ///////////////////////////////////////////////
  // Menu Item "File.Import"
  else if (apWidget == mpMainMenuImport) {
    Command_Import();
  }
  ///////////////////////////////////////////////
  // Menu Item "File.Export"
  else if (apWidget == mpMainMenuExport) {
    Command_Export();
  }
  ///////////////////////////////////////////////
  // Menu Item "File.Exit"
  else if (apWidget == mpMainMenuExit) {
    Command_Exit();
  }
  ///////////////////////////////////////////////
  // Menu Item "Edit.Entity Search"
  else if (apWidget == mpMainMenuSearch) {
    mpWindowSearch->SetActive(true);
  }
  ///////////////////////////////////////////////
  // Menu Item "Edit.Group Window"
  else if (apWidget == mpMainMenuGroup) {
    mpWindowGroup = hplNew(cLevelEditorWindowGroup, (this));
    mpWindowGroup->Init();
    mpWindowGroup->SetActive(true);
    AddWindow(mpWindowGroup);
  }
  ///////////////////////////////////////////////
  // Menu Item "Edit.Level settings"
  else if (apWidget == mpMainMenuLevelSettings) {
    iEditorWindow *pWindow = hplNew(cLevelEditorWindowLevelSettings, (this));
    pWindow->Init();
    pWindow->SetActive(true);
    AddWindow(pWindow);
  }
  ///////////////////////////////////////////////
  // Menu Item "Edit.Options"
  else if (apWidget == mpMainMenuOptions) {
    mpWindowOptions = hplNew(cEditorWindowOptions, (this));
    mpWindowOptions->Init();
    mpWindowOptions->SetActive(true);
    AddWindow(mpWindowOptions);
  }

  return true;
}
kGuiCallbackDeclaredFuncEnd(cLevelEditor, MainMenu_ItemClick);

//--------------------------------------------------------------------

bool cLevelEditor::MainMenu_UndoRedo(iWidget *apWidget,
                                     const cGuiMessageData &aData) {
  if (apWidget == mpMainMenuUndo)
    mpActionHandler->Undo();
  else if (apWidget == mpMainMenuRedo)
    mpActionHandler->Redo();

  return true;
}
kGuiCallbackDeclaredFuncEnd(cLevelEditor, MainMenu_UndoRedo);

//--------------------------------------------------------------------

iEditorWorld *cLevelEditor::CreateSpecificWorld() {
  return hplNew(cLevelEditorWorld, (this));
}

//--------------------------------------------------------------------

iEditorWindowEditModeSidebar *cLevelEditor::CreateSpecificEditModeSidebar() {
  return hplNew(iEditorWindowEditModeSidebar, (this, true));
}

//--------------------------------------------------------------------

void cLevelEditor::SetUpWindowAreas() {
  // Row under the menu is the horizontal edit-mode toolbar (already Init()ed
  // by the base before this runs, so its height is known).
  float fMenuY = mpMainMenu->GetSize().y;
  float fToolbarB = fMenuY + 2 + mpEditModeSidebar->GetSize().y + 2;

  // Preserve the width of the right-side edit-mode settings pane (base reserved
  // 0.23*screenX minus the old 33px sidebar column). The left column now hosts
  // the hierarchy panel instead of the sidebar.
  float fRightW = 0.23f * mvScreenSize.x - 33.0f;

  SetLayoutVec3f(eLayoutVec3_ViewportAreaPos,
                 cVector3f(kLevelEditorHierarchyPanelWidth + 2, fToolbarB, 1));
  SetLayoutVec2f(eLayoutVec2_ViewportAreaSize,
                 cVector2f(mvScreenSize.x - fRightW -
                               (kLevelEditorHierarchyPanelWidth + 2),
                           mvScreenSize.y - 52 - fToolbarB));
}

//--------------------------------------------------------------------

void cLevelEditor::LoadEditorSession(tinyxml2::XMLElement *apDoc,
                                     tinyxml2::XMLElement **apElement) {
  iEditorBase::LoadEditorSession(apDoc, apElement);
  tinyxml2::XMLElement *pSession = *apElement;

  ///////////////////////////////////////
  // Group Data
  tinyxml2::XMLElement *pGroups = pSession->FirstChildElement("Groups");
  if (pGroups) {
    for (tinyxml2::XMLElement *pGroup = pGroups->FirstChildElement();
         pGroup != NULL; pGroup = pGroup->NextSiblingElement()) {
      unsigned int lID = GetAttributeInt(pGroup, "ID", 0);
      tString sName = GetAttributeString(pGroup, "Name", "");
      bool bVisible = GetAttributeBool(pGroup, "Visible", true);

      AddGroup(lID, sName);
      GetGroup(lID).SetVisibility(bVisible);
    }
  }

  tinyxml2::XMLElement *pClipPlanes = pSession->FirstChildElement("ClipPlanes");
  if (pClipPlanes) {
    for (tinyxml2::XMLElement *pXmlPlane = pClipPlanes->FirstChildElement();
         pXmlPlane != NULL; pXmlPlane = pXmlPlane->NextSiblingElement()) {
      cEditorClipPlane *pPlane = mpEditorWorld->AddClipPlane();
      pPlane->Load(pXmlPlane);
    }

    mpLowerToolbar->SetFocusedClipPlane(
        GetAttributeInt(pClipPlanes, "Focused", 0));
  }
}

//--------------------------------------------------------------------

bool cLevelEditor::ImportFileCallback(iWidget *apWidget,
                                      const cGuiMessageData &aData) {
  if (aData.mlVal == 1) {
    msLastImportFile = mvLoadFilenames.front();
    tIntList lstImportedIDs;
    mpEditorWorld->ImportObjects(cString::To8Char(msLastImportFile),
                                 lstImportedIDs);

    cEditorEditModeSelect *pEditMode =
        (cEditorEditModeSelect *)GetEditMode("Select");
    AddAction(pEditMode->CreateSelectEntityAction(tIntList(),
                                                  eSelectActionType_Clear));
    AddAction(pEditMode->CreateSelectEntityAction(lstImportedIDs,
                                                  eSelectActionType_Select));
  }

  return true;
}
kGuiCallbackDeclaredFuncEnd(cLevelEditor, ImportFileCallback);

//--------------------------------------------------------------------

bool cLevelEditor::ExportFileCallback(iWidget *apWidget,
                                      const cGuiMessageData &aData) {
  if (aData.mlVal == 1) {
    mpEditorWorld->ExportObjects(cString::To8Char(msLastImportFile),
                                 mpSelection->GetEntities());
  }

  return true;
}
kGuiCallbackDeclaredFuncEnd(cLevelEditor, ExportFileCallback);

//--------------------------------------------------------------------

void cLevelEditor::SaveEditorSession(tinyxml2::XMLElement *apDoc,
                                     tinyxml2::XMLElement **apElement) {
  iEditorBase::SaveEditorSession(apDoc, apElement);
  tinyxml2::XMLElement *pSession = *apElement;

  ///////////////////////////////////////
  // Group Data
  tinyxml2::XMLElement *pXmlGroups =
      pSession->GetDocument()->NewElement("Groups");
  pSession->InsertEndChild(pXmlGroups);
  tGroupMapIt itGroups = mmapGroups.begin();
  for (; itGroups != mmapGroups.end(); ++itGroups) {
    cLevelEditorGroup *pGroup = &(itGroups->second);
    tinyxml2::XMLElement *pXmlGroup =
        pXmlGroups->GetDocument()->NewElement("Group");
    pXmlGroups->InsertEndChild(pXmlGroup);

    SetAttributeInt(pXmlGroup, "ID", pGroup->GetID());
    SetAttributeString(pXmlGroup, "Name", pGroup->GetName());
    SetAttributeBool(pXmlGroup, "Visible", pGroup->GetVisibility());
  }

  tEditorClipPlaneVec &vClipPlanes = mpEditorWorld->GetClipPlanes();
  if (vClipPlanes.empty() == false) {
    tinyxml2::XMLElement *pXmlClipPlanes =
        pSession->GetDocument()->NewElement("ClipPlanes");
    pSession->InsertEndChild(pXmlClipPlanes);
    for (int i = 0; i < (int)vClipPlanes.size(); ++i) {
      cEditorClipPlane *pPlane = vClipPlanes[i];
      tinyxml2::XMLElement *pXmlPlane =
          pXmlClipPlanes->GetDocument()->NewElement("");
      pXmlClipPlanes->InsertEndChild(pXmlPlane);

      pPlane->Save(pXmlPlane);
    }

    SetAttributeInt(pXmlClipPlanes, "Focused",
                    mpLowerToolbar->GetFocusedClipPlane());
  }
}

//--------------------------------------------------------------------

void cLevelEditor::UpdateEditMenu() {
  mpMainMenuUndo->SetEnabled(!mpActionHandler->IsDoneActionsListEmpty());
  mpMainMenuRedo->SetEnabled(!mpActionHandler->IsUndoneActionsListEmpty());

  bool bHasSelectedObjects = (mpSelection->IsEmpty() == false);

  mpMainMenuDelete->SetEnabled(bHasSelectedObjects &&
                               mpSelection->IsDeletable());
  mpMainMenuClone->SetEnabled(bHasSelectedObjects &&
                              mpSelection->IsCloneable());
  mpMainMenuCompound->SetEnabled(mpSelection->GetNumEntities() > 0);
}

//--------------------------------------------------------------------

//--------------------------------------------------------------------

void cLevelEditor::OnInit() {
  /////////////////////////////////////////////////////////
  // Add any extra dirs to resources!
  // static objects
  for (int i = 0; i < (int)mvExtraSODirs.size(); ++i) {
    const tWString &sExtraDir = mvExtraSODirs[i];
    mpEngine->GetResources()->AddResourceDir(sExtraDir, true);
    // Also register as a lookup dir so the dir shows up as a browsable
    // category in the object browser (not just loadable as a resource).
    mpDirHandler->AddLookUpDir(eDir_StaticObjects, sExtraDir, true);
  }
  // entities
  for (int i = 0; i < (int)mvExtraEntDirs.size(); ++i) {
    const tWString &sExtraDir = mvExtraEntDirs[i];
    mpEngine->GetResources()->AddResourceDir(sExtraDir, true);
    mpDirHandler->AddLookUpDir(eDir_Entities, sExtraDir, true);
  }
  ///////////////////////////////////////////////////
  // Add EditModes here!
  // Per-instance override: [Directories] MaterialsOverride in the editor
  // config points at an alternate surface-data file, else stock materials.cfg.
  tString materialsOverride =
      mpMainConfig->GetString("Directories", "MaterialsOverride", "");
  if (materialsOverride != "")
    mpEngine->GetPhysics()->LoadSurfaceData(materialsOverride);
  else
    mpEngine->GetPhysics()->LoadSurfaceData("materials.cfg");

  AddEditMode(hplNew(cEditorEditModeSelect, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModeLights, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModeBillboards, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModeParticleSystems, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModeSounds, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModeStaticObjects, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModeEntities, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModeAreas, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModePrimitives, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModeDecals, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModeFogAreas, (this, mpEditorWorld)));
  AddEditMode(hplNew(cEditorEditModeCombine, (this)));

  //////////////////////////////////////////////////
  // Live file-watch tuning. The watcher itself is created with the world; here
  // we just push the [FileWatch] config onto it. Verbose logs every watched
  // dir / drained event / debounce step so it is visible what triggers a
  // reload. DebounceMs is the stability window a change must hold before it
  // applies (coalesces mid-write / burst saves).
  if (cEditorFileWatcher *pWatcher = mpEditorWorld->GetFileWatcher()) {
    pWatcher->SetEnabled(mpMainConfig->GetBool("FileWatch", "Enabled", true));
    pWatcher->SetVerbose(mpMainConfig->GetBool("FileWatch", "Verbose", false));
    pWatcher->SetDebounce(
        mpMainConfig->GetFloat("FileWatch", "DebounceMs", 150.0f) / 1000.0f);
  }

  //////////////////////////////////////////////////
  // MCP server (localhost). Config was read in OnLoadConfig; start it here.
  // Also (re)started when the Options "MCP" tab applies changes.
  RestartMCPServer();

  //////////////////////////////////////////////////
  // Off-screen virtual-camera capture backing the MCP 'capture_view' tool.
  // Independent of whether the server is currently running (it is only used
  // when a capture is requested).
  mpCameraCapture = hplNew(cLevelEditorCameraCapture, (this));
}

//--------------------------------------------------------------------

void cLevelEditor::OnInitLayout() {
  ///////////////////////////////////
  // EditMode Tool box
  mpEditModeSidebar->SetPosition(cVector3f(0, mpMainMenu->GetSize().y + 2, 1));

  ///////////////////////////////////
  // Lower Toolbar
  mpLowerToolbar->SetPosition(cVector3f(
      GetLayoutVec3f(eLayoutVec3_ViewportAreaPos).x, mvScreenSize.y - 50, 1));
  cVector3f vHandlePos = cVector3f(5, 5, 0.1f);
  iWidget *pHandle = mpLowerToolbar->AddGridControls();
  pHandle->SetPosition(vHandlePos);
  vHandlePos = vHandlePos + cVector3f(pHandle->GetSize().x + 10, 0, 0);
  pHandle = mpLowerToolbar->AddViewportControls();
  pHandle->SetPosition(vHandlePos);
  vHandlePos = vHandlePos + cVector3f(pHandle->GetSize().x + 10, 0, 0);
  pHandle = mpLowerToolbar->AddLightingControls();
  pHandle->SetPosition(vHandlePos);
  vHandlePos += cVector3f(pHandle->GetSize().x + 10, 0, 0);
  pHandle = mpLowerToolbar->AddCameraControls();
  pHandle->SetPosition(vHandlePos);
  vHandlePos += cVector3f(pHandle->GetSize().x + 10, 0, 0);
  pHandle = mpLowerToolbar->AddClipPlaneControls();
  pHandle->SetPosition(vHandlePos);
  vHandlePos += cVector3f(pHandle->GetSize().x + 10, 0, 0);
  pHandle = mpLowerToolbar->AddVisibilityControls();
  pHandle->SetPosition(vHandlePos);
  vHandlePos += cVector3f(pHandle->GetSize().x + 10, 0, 0);
  pHandle = mpLowerToolbar->AddDebugControls();
  pHandle->SetPosition(vHandlePos);

  ////////////////////////////////////
  // Search Window
  mpWindowSearch = cEditorWindowFactory::CreateSearchWindow(
      this, (cEditorEditModeSelect *)GetEditMode("Select"));

  ////////////////////////////////////
  // Scene hierarchy panel (left column, between the horizontal toolbar and the
  // lower toolbar). Owned by the base window list (AddWindow), which also feeds
  // it the OnWorldModify / OnSelectionChange fan-outs it rebuilds/highlights
  // on.
  float fToolbarB =
      mpMainMenu->GetSize().y + 2 + mpEditModeSidebar->GetSize().y + 2;
  mpWindowHierarchy = hplNew(cLevelEditorWindowHierarchy, (this, fToolbarB));
  mpWindowHierarchy->Init();
  AddWindow(mpWindowHierarchy);
  mpWindowHierarchy->SetPosition(cVector3f(0, fToolbarB, 1));
  mpWindowHierarchy->SetActive(true);
}

//--------------------------------------------------------------------

void cLevelEditor::OnSetUpDirectories() {
  const tWString &sWorkingDir = GetWorkingDir();

  mpDirHandler->AddLookUpDir(
      eDir_Maps,
      sWorkingDir +
          mpMainConfig->GetStringW("Directories", "MapsDir", _W("maps")),
      true);
  mpDirHandler->AddLookUpDir(
      eDir_StaticObjects,
      sWorkingDir + mpMainConfig->GetStringW("Directories", "StaticObjectsDir",
                                             _W("static_objects")),
      true);
  mpDirHandler->AddLookUpDir(
      eDir_Entities,
      sWorkingDir + mpMainConfig->GetStringW("Directories", "EntitiesDir",
                                             _W("entities")),
      true);
  mpDirHandler->AddLookUpDir(
      eDir_Decals,
      sWorkingDir + mpMainConfig->GetStringW("Directories", "DecalsDir",
                                             _W("textures/decals")),
      true);
}

//--------------------------------------------------------------------

void cLevelEditor::OnUpdate(float afTimeStep) {
  // Apply a deferred enter/exit ent-file-scope request here — outside any GUI
  // widget callback — so the toolbar/panel widget surgery it does can't free a
  // widget mid-callback (SIGSEGV in iWidget::ProcessCallbacks).
  if (mbPendingScopeExit || mlPendingScopeEnter >= 0)
    ProcessPendingScope();

  // Keep the scoped shadow world in sync with the scene: poll the session for edits
  // and push them to every placed instance on a short debounce. Driven here (not from
  // a GUI window) so scoped modifications are ALWAYS persisted while scoped.
  if (mpScopedSession)
    mpScopedSession->Update(afTimeStep);

  // While the on-demand entity-properties box is up, selecting a sub-object
  // puts its own box in the SAME right-pane slot — so yield the entity box
  // (click "Entity" again to bring it back). Only runs while it's actually
  // shown.
  if (mpScopedEntityBox && GetSelection()->IsEmpty() == false) {
    mpScopedEntityBox->SetActive(false);
    cEditorWindowFactory::DestroyEditorWindow(
        (iEditorWindow *)mpScopedEntityBox);
    mpScopedEntityBox = NULL;
  }

  // Drain any MCP tool calls queued by the server's worker threads and run
  // them here, on the main/engine thread (see LevelEditorMCPServer.h).
  if (mpMCPServer)
    mpMCPServer->DrainQueue();
}

//--------------------------------------------------------------------

void cLevelEditor::OnPostRender(float afTimeStep) {
  // Pump the async camera-capture jobs from the base OnDraw (after the scene
  // render), so the editor world's per-frame GPU data (TLAS / lights / decals,
  // published by PrepareFrame for the visible viewport) is ready for our
  // headless eRenderer_Main Evaluate. Then hand any finished captures to the
  // MCP server (which parked the caller's promise). Always drain — even if the
  // server was disabled/restarted since the job was queued — so completions
  // don't pile up; FulfillDeferred is a no-op for an id the current server
  // doesn't hold.
  if (mpCameraCapture == NULL)
    return;

  mpCameraCapture->Pump(afTimeStep);

  int lJobId;
  cMCPToolResult result;
  while (mpCameraCapture->PopCompleted(lJobId, result)) {
    if (mpMCPServer)
      mpMCPServer->FulfillDeferred(lJobId, result);
  }
}

//--------------------------------------------------------------------

void cLevelEditor::OnPostUpdateLayout() {
  ///////////////////////////////////////
  // Update Title Bar
  tString sTitlebarFilename;

  if (msSaveFilename == _W(""))
    sTitlebarFilename = "Unnamed Map";
  else
    sTitlebarFilename = cString::To8Char(cString::GetFileNameW(msSaveFilename));

  // The document is "modified" if the map world has unsaved changes OR any prefab
  // in the scene has unsaved edits: pending prefab docs not yet flushed to disk
  // (scope-session Saves + MCP update_prefab), plus live scoped edits still inside
  // the session's debounce window before they're harvested into the pending doc.
  bool bModified = mpEditorWorld->IsModified() ||
                   GetPendingEntFileDirtyCount() > 0 ||
                   (mpScopedSession && mpScopedSession->IsDirty());
  tString sModified = bModified ? "(modified)" : "";

  mpEngine->GetGraphics()->GetWindow()->SetCaption(
      sTitlebarFilename + sModified + " - " + msCaption);

  ///////////////////////////////////////
  // Update Recent Files
  mpMainMenuRecent->ClearMenuItems();
  mpMainMenuRecent->SetEnabled(mlstRecentFiles.empty() == false);
  for (tWStringListIt it = mlstRecentFiles.begin(); it != mlstRecentFiles.end();
       ++it) {
    tWString sRecent = *it;

    if (sRecent == _W(""))
      break;

    if (cPlatform::FileExists(sRecent)) {
      cWidgetMenuItem *pItem = mpMainMenuRecent->AddMenuItem(sRecent);
      pItem->AddCallback(eGuiMessage_ButtonPressed, this,
                         kGuiCallback(RecentFileCallback));
    }
  }
  UpdateEditMenu();
}

//--------------------------------------------------------------------

void cLevelEditor::OnInitInput() {
  /////////////////////////////////////////////////////////
  // Set up Special input keys
}

//--------------------------------------------------------------------

void cLevelEditor::OnLoadConfig() {
  //////////////////////////////////////////////////////////////
  // Set up loading stuff that is specific to this editor,
  // and stuff like log filename (this is done pre engine creation)
  tWString sConfigFile = GetHomeDir() + _W("LevelEditor.cfg");

  mpLocalConfig = hplNew(cConfigFile, (sConfigFile));
  mpLocalConfig->Load();

  SetSettingValue("ScreenWidth",
                  mpLocalConfig->GetString("Screen", "Width", "1024"));
  SetSettingValue("ScreenHeight",
                  mpLocalConfig->GetString("Screen", "Height", "768"));
  SetSettingValue("FullScreen",
                  mpLocalConfig->GetString("Screen", "Fullscreen", "false"));

  SetSettingValue("TexQuality",
                  mpLocalConfig->GetString("Screen", "TexQuality", "0"));
  SetSettingValue("RendererBackend",
                  mpLocalConfig->GetString("Screen", "RendererBackend", "standard"));

  // Set Input config
  SetSettingValue("TumbleFactor",
                  mpLocalConfig->GetString("Input", "TumbleSpeed", "0.005"));
  SetSettingValue("TrackFactor",
                  mpLocalConfig->GetString("Input", "TrackFactor", "0.01"));
  SetSettingValue("ZoomFactor",
                  mpLocalConfig->GetString("Input", "ZoomFactor", "0.001"));
  SetSettingValue("MouseWheelZoom",
                  mpLocalConfig->GetString("Input", "MouseWheelZoom", "0.1"));

  cEditorSelection::SetRotateSnap(
      mpLocalConfig->GetFloat("Options", "RotateSnap", kPi2f / 6));
  cEditorSelection::SetScaleSnap(
      mpLocalConfig->GetFloat("Options", "ScaleSnap", 0.25f));

  mpActionHandler->SetMaxUndoSize(
      mpLocalConfig->GetInt("Options", "UndoStackSize", 50));
  iEngineEntityMesh::SetDisabledCoverage(
      mpLocalConfig->GetFloat("Options", "DisabledCoverage", 0.5f));

  // Viewport display gamma (the game's Graphics/Gamma equivalent). Read before
  // InitLayout creates the viewports, so every viewport ctor picks it up.
  // Clamped so a hand-edited cfg can't produce a black or blown-out pane.
  iEditorViewport::SetDisplayGamma(cMath::Clamp(
      mpLocalConfig->GetFloat("Options", "DisplayGamma", 1.0f), 0.1f, 5.0f));

  /////////////////////////////////
  // MCP server config (edited via the Options "MCP" tab)
  mbMCPEnabled = mpLocalConfig->GetBool("MCP", "Enabled", true);
  mlMCPPort = mpLocalConfig->GetInt("MCP", "Port", 8787);
  msMCPToken = mpLocalConfig->GetString("MCP", "Token", "");
  {
    const char *pEnvPort = getenv("HPL_EDITOR_MCP_PORT");
    if (pEnvPort)
      mlMCPPort = atoi(pEnvPort);
  }

  /////////////////////////////////
  // Recent files setup
  for (int i = 0; i < 10; ++i) {
    tWString sRecent = mpLocalConfig->GetStringW(
        "RecentUsedFiles", "RecentFile" + cString::ToString(i + 1), _W(""));

    if (sRecent == _W(""))
      break;

    if (cPlatform::FileExists(sRecent)) {
      tWStringListIt itFoundString =
          find(mlstRecentFiles.begin(), mlstRecentFiles.end(), sRecent);
      if (itFoundString != mlstRecentFiles.end())
        mlstRecentFiles.erase(itFoundString);

      mlstRecentFiles.push_back(sRecent);
    }
  }

  // Window caption
  msCaption = "HPL Level Editor";

  SetLogFile(GetHomeDir() + _W("LevelEditor.log"));

  msLastLoadPath = mpLocalConfig->GetStringW("Directories", "LastUsedPath",
                                             GetMainLookUpDir(eDir_Maps));

  ///////////////////////////////////////////
  // Get extra dirs for static objects
  int j = 1;
  while (true) {
    tWString sExtraDir = mpLocalConfig->GetStringW(
        "Directories", "ExtraStaticObjectDir" + cString::ToString(j), _W(""));
    if (sExtraDir == _W("")) {
      break;
    }

    if (cPlatform::FolderExists(sExtraDir)) {
      mvExtraSODirs.push_back(sExtraDir);
    } else {
      Log("Extra StaticObject Directory %ls does not exist\n",
          sExtraDir.c_str());
    }

    ++j;
  }

  ///////////////////////////////////////////
  // Get extra dirs for entities
  j = 1;
  while (true) {
    tWString sExtraDir = mpLocalConfig->GetStringW(
        "Directories", "ExtraEntityDir" + cString::ToString(j), _W(""));
    if (sExtraDir == _W("")) {
      break;
    }

    if (cPlatform::FolderExists(sExtraDir))
      mvExtraEntDirs.push_back(sExtraDir);
    else
      Log("Extra Entity Directory %ls does not exist\n", sExtraDir.c_str());

    ++j;
  }
}

//--------------------------------------------------------------------

void cLevelEditor::OnSaveConfig() {
  mpLocalConfig->SetString("Screen", "Width", GetSetting("ScreenWidth"));
  mpLocalConfig->SetString("Screen", "Height", GetSetting("ScreenHeight"));
  mpLocalConfig->SetString("Screen", "Fullscreen", GetSetting("Fullscreen"));

  mpLocalConfig->SetString("Screen", "TexQuality", GetSetting("TexQuality"));
  mpLocalConfig->SetString("Screen", "RendererBackend",
                           GetSetting("RendererBackend"));

  mpLocalConfig->SetString("Input", "TumbleFactor", GetSetting("TumbleFactor"));
  mpLocalConfig->SetString("Input", "TrackFactor", GetSetting("TrackFactor"));
  mpLocalConfig->SetString("Input", "ZoomFactor", GetSetting("ZoomFactor"));
  mpLocalConfig->SetString("Input", "MouseWheelZoom",
                           GetSetting("MouseWheelZoom"));

  mpLocalConfig->SetFloat("Options", "RotateSnap",
                          cEditorSelection::GetRotateSnap());
  mpLocalConfig->SetFloat("Options", "ScaleSnap",
                          cEditorSelection::GetScaleSnap());

  mpLocalConfig->SetInt("Options", "UndoStackSize",
                        (int)mpActionHandler->GetMaxUndoSize());
  mpLocalConfig->SetFloat("Options", "DisabledCoverage",
                          iEngineEntityMesh::GetDisabledCoverage());
  mpLocalConfig->SetFloat("Options", "DisplayGamma",
                          iEditorViewport::GetDisplayGamma());

  ////////////////////////////////
  // MCP server config
  mpLocalConfig->SetBool("MCP", "Enabled", mbMCPEnabled);
  mpLocalConfig->SetInt("MCP", "Port", mlMCPPort);
  mpLocalConfig->SetString("MCP", "Token", msMCPToken);

  // Save recent file names
  int i = 0;
  for (tWStringListIt it = mlstRecentFiles.begin(); it != mlstRecentFiles.end();
       ++it) {
    tWString sRecent = *it;

    if (sRecent == _W(""))
      break;

    if (cPlatform::FileExists(sRecent)) {
      mpLocalConfig->SetString("RecentUsedFiles",
                               "RecentFile" + cString::ToString(++i),
                               cString::To8Char(sRecent));
    }
  }
  // Save last used path
  mpLocalConfig->SetString("Directories", "LastUsedPath",
                           (const tString &)cString::To8Char(msLastLoadPath));

  // Save extra dirs
  for (int j = 0; j < (int)mvExtraSODirs.size(); ++j) {
    tString sExtraDir = cString::S16BitToUTF8(mvExtraSODirs[j]);

    mpLocalConfig->SetString("Directories",
                             "ExtraStaticObjectDir" + cString::ToString(j + 1),
                             sExtraDir);
  }
  for (int j = 0; j < (int)mvExtraEntDirs.size(); ++j) {
    tString sExtraDir = cString::S16BitToUTF8(mvExtraEntDirs[j]);

    mpLocalConfig->SetString(
        "Directories", "ExtraEntityDir" + cString::ToString(j + 1), sExtraDir);
  }

  mpLocalConfig->Save();
  hplDelete(mpLocalConfig);
}

//--------------------------------------------------------------------

cWidgetMainMenu *cLevelEditor::CreateMainMenu() {
  /////////////////////////////////
  // Setup main menu
  cWidgetMenuItem *pItem = NULL;
  cWidgetMenuItem *pSubItem = NULL;

  mpMainMenu = mpSet->CreateWidgetMainMenu(mpBGFrame);

  // File menu
  pItem = mpMainMenu->AddMenuItem(_W("File"));
  // New
  mpMainMenuNew = pItem->AddMenuItem(_W("New"));
  mpMainMenuNew->AddCallback(eGuiMessage_ButtonPressed, this,
                             kGuiCallback(MainMenu_ItemClick));
  mpMainMenuNew->AddShortcut(eKeyModifier_Ctrl, eKey_N);

  pItem->AddSeparator();

  // Open
  mpMainMenuLoad = pItem->AddMenuItem(_W("Open"));
  mpMainMenuLoad->AddCallback(eGuiMessage_ButtonPressed, this,
                              kGuiCallback(MainMenu_ItemClick));
  mpMainMenuLoad->AddShortcut(eKeyModifier_Ctrl, eKey_O);

  pItem->AddSeparator();
  // Save
  mpMainMenuSave = pItem->AddMenuItem(_W("Save"));
  mpMainMenuSave->AddCallback(eGuiMessage_ButtonPressed, this,
                              kGuiCallback(MainMenu_ItemClick));
  mpMainMenuSave->AddShortcut(eKeyModifier_Ctrl, eKey_S);

  // Save As
  mpMainMenuSaveAs = pItem->AddMenuItem(_W("Save as"));
  mpMainMenuSaveAs->AddCallback(eGuiMessage_ButtonPressed, this,
                                kGuiCallback(MainMenu_ItemClick));

  pItem->AddSeparator();

  // Open Recent
  mpMainMenuRecent = pItem->AddMenuItem(_W("Open Recent"));

  pItem->AddSeparator();

  // Import
  mpMainMenuImport = pItem->AddMenuItem(_W("Import objects"));
  mpMainMenuImport->AddCallback(eGuiMessage_ButtonPressed, this,
                                kGuiCallback(MainMenu_ItemClick));

  // Export
  mpMainMenuExport = pItem->AddMenuItem(_W("Export selection"));
  mpMainMenuExport->AddCallback(eGuiMessage_ButtonPressed, this,
                                kGuiCallback(MainMenu_ItemClick));

  pItem->AddSeparator();

  // Quit
  mpMainMenuExit = pItem->AddMenuItem(_W("Quit"));
  mpMainMenuExit->AddCallback(eGuiMessage_ButtonPressed, this,
                              kGuiCallback(MainMenu_ItemClick));
#if defined(_WIN32)
  mpMainMenuExit->AddShortcut(eKeyModifier_Alt, eKey_F4);
#elif defined(__linux__)
  mpMainMenuExit->AddShortcut(eKeyModifier_Ctrl, eKey_Q);
#elif defined(__APPLE__)
  mpMainMenuExit->AddShortcut(eKeyModifier_Ctrl, eKey_Q);
#endif

  // Edit menu
  pItem = mpMainMenu->AddMenuItem(_W("Edit"));
  // Undo
  mpMainMenuUndo = pItem->AddMenuItem(_W("Undo"));
  mpMainMenuUndo->AddCallback(eGuiMessage_ButtonPressed, this,
                              kGuiCallback(MainMenu_UndoRedo));
  mpMainMenuUndo->AddShortcut(eKeyModifier_Ctrl, eKey_Z);
  // Redo
  mpMainMenuRedo = pItem->AddMenuItem(_W("Redo"));
  mpMainMenuRedo->AddCallback(eGuiMessage_ButtonPressed, this,
                              kGuiCallback(MainMenu_UndoRedo));
  mpMainMenuRedo->AddShortcut(eKeyModifier_Ctrl, eKey_Y);

  pItem->AddSeparator();

  // Delete
  mpMainMenuDelete = pItem->AddMenuItem(_W("Delete"));
  mpMainMenuDelete->AddCallback(eGuiMessage_ButtonPressed, this,
                                kGuiCallback(MainMenu_ItemClick));
  mpMainMenuDelete->AddShortcut(eKeyModifier_None, eKey_BackSpace);
  mpMainMenuDelete->AddShortcut(eKeyModifier_None, eKey_Delete);
  // Duplicate
  mpMainMenuClone = pItem->AddMenuItem(_W("Duplicate"));
  mpMainMenuClone->AddCallback(eGuiMessage_ButtonPressed, this,
                               kGuiCallback(MainMenu_ItemClick));
  mpMainMenuClone->AddShortcut(eKeyModifier_Ctrl, eKey_D);
  // Create/Destroy compound
  mpMainMenuCompound = pItem->AddMenuItem(_W("Create/Destroy Compound object"));
  mpMainMenuCompound->AddCallback(eGuiMessage_ButtonPressed, this,
                                  kGuiCallback(MainMenu_ItemClick));
  mpMainMenuCompound->AddShortcut(eKeyModifier_None, eKey_B);

  pItem->AddSeparator();

  // Search
  mpMainMenuSearch = pItem->AddMenuItem(_W("Find objects"));
  mpMainMenuSearch->AddCallback(eGuiMessage_ButtonPressed, this,
                                kGuiCallback(MainMenu_ItemClick));
  mpMainMenuSearch->AddShortcut(eKeyModifier_Ctrl, eKey_F);

  // Groups
  mpMainMenuGroup = pItem->AddMenuItem(_W("Browse groups"));
  mpMainMenuGroup->AddCallback(eGuiMessage_ButtonPressed, this,
                               kGuiCallback(MainMenu_ItemClick));

  pItem->AddSeparator();

  // Level settings
  mpMainMenuLevelSettings = pItem->AddMenuItem(_W("Level settings"));
  mpMainMenuLevelSettings->AddCallback(eGuiMessage_ButtonPressed, this,
                                       kGuiCallback(MainMenu_ItemClick));

  pItem->AddSeparator();

  // Options
  mpMainMenuOptions = pItem->AddMenuItem(_W("Options"));
  mpMainMenuOptions->AddCallback(eGuiMessage_ButtonPressed, this,
                                 kGuiCallback(MainMenu_ItemClick));

  return mpMainMenu;
}

//--------------------------------------------------------------------
