-- HPL2 editors + MshConverter. The legacy viewers (MapViewer/ModelViewer/
-- ParticleViewer) are intentionally omitted here.
local TOOLS   = ROOT .. "/HPL2/tools"
local EDITORS = TOOLS .. "/editors"

-- Editor/viewer runtime data (editor/AreaTypes.cfg, editor/EntityTypes.cfg, the
-- billboard icon set, editor models/textures, viewer sample assets). The engine
-- resolves these relative to the working dir -- EditorBaseClasses.cpp registers
-- "editor/" as a resource dir -- so they must sit next to the built editor
-- executables. Mirrors the CMake `install(DIRECTORY tools/resources/ ...)` hack.
local RESOURCES = TOOLS .. "/resources"

local function winpath(p) return (p:gsub("/", "\\")) end

-- Resolve a list of bare source names recursively under the editors tree,
-- mirroring CMake's file(GLOB_RECURSE ... <name>.cpp).
local function resolve(basenames)
    local out = {}
    for _, n in ipairs(basenames) do
        for _, f in ipairs(os.matchfiles(EDITORS .. "/**/" .. n)) do
            table.insert(out, f)
        end
    end
    return out
end

-- Shared scaffolding for every editor target, including the engine include set
-- the editor sources need.
local function editor_includes()
    includedirs {
        EDITORS .. "/common",
        ROOT .. "/HPL2/core/include",
        ROOT .. "/HPL2/tools/tests/Common",
        ROOT .. "/amnesia/slang",     -- HPL2 headers pull in Constants.h
        ROOT .. "/amnesia/glsl",
        DEPS_SOURCES .. "/AngelScript/include",
        DEPS_EXTERN .. "/tinyxml2",
        DEPS_EXTERN .. "/rapidjson/include", -- RapidJSON submodule (header-only, MCP server)
        DEPS_EXTERN .. "/httplib",    -- cpp-httplib (header-only, MCP server)
    }
    deps_public_includes()
    vulkan_includes()
    mathlib_use()
    defines { "USERDIR_RESOURCES", "RAPIDJSON_HAS_STDSTRING=1" }
    link_engine()
    filter "system:linux"
        linkoptions { "-Wl,-rpath,'$$ORIGIN/libs'", "-Wl,-rpath,'$$ORIGIN'" }
    filter "system:windows"
        links { "ws2_32" }            -- cpp-httplib (LevelEditor MCP server) needs Winsock
    filter {}
end

-- Copy tools/resources/ next to the built executables. Done per editor target
-- (postbuild) rather than only in deploy.sh, so a plain `make` -- and
-- therefore the CI release archive, which just tars the runtime dir -- carries
-- the editor data even when no retail install is deployed over it.
local function tool_resources_postbuild()
    local dest = runtime_dir("")
    filter "system:not windows"
        postbuildcommands {
            string.format('{MKDIR} "%s"', dest),
            string.format('cp -R "%s/." "%s/"', RESOURCES, dest),
        }
    filter "system:windows"
        postbuildcommands {
            string.format('xcopy /E /I /Y /Q "%s" "%s\\" >nul',
                winpath(RESOURCES), winpath(dest)),
        }
    filter {}
end

local function editor_target(name, subdir, basenames)
    project(name)
        -- HPL2's entry wrapper (LowLevelSystemSDL.cpp) provides WinMain on Windows
        -- (GUI subsystem) and main elsewhere -- mirror amnesia.lua's kind split.
        filter "system:windows"
            kind "WindowedApp"
        filter "system:not windows"
            kind "ConsoleApp"
        filter {}
        language "C++"
        set_output("runtime")
        files (resolve(basenames))
        includedirs { EDITORS .. "/" .. subdir }
        editor_includes()
        tool_resources_postbuild()
        buildid(name, EDITORS .. "/" .. subdir)
end

local leveleditor = {
    "BoxCreator.cpp", "DirectoryHandler.cpp", "EditorAction.cpp",
    "EditorActionArea.cpp", "EditorActionCompoundObject.cpp",
    "EditorActionDynamicEntity.cpp", "EditorActionEntity.cpp",
    "EditorActionHandler.cpp", "EditorActionMisc.cpp", "EditorActionSelection.cpp",
    "EditorAxisAlignedPlane.cpp", "EditorBaseClasses.cpp", "EditorClipPlane.cpp",
    "EditorEditMode.cpp", "EditorEditModeAreas.cpp", "EditorEditModeBillboards.cpp",
    "EditorEditModeCombine.cpp", "EditorEditModeDecals.cpp",
    "EditorEditModeEntities.cpp", "EditorEditModeFogAreas.cpp",
    "EditorEditModeLights.cpp", "EditorEditModeParticleSystems.cpp",
    "EditorEditModePrimitive.cpp", "EditorEditModeSelect.cpp",
    "EditorEditModeSelectTool.cpp", "EditorEditModeSelectToolRotate.cpp",
    "EditorEditModeSelectToolScale.cpp", "EditorEditModeSelectToolTranslate.cpp",
    "EditorEditModeSounds.cpp", "EditorEditModeStaticObjects.cpp", "EditorGrid.cpp",
    "EditorHelper.cpp", "EditorIndex.cpp", "EditorInput.cpp", "EditorSelection.cpp",
    "EditorThumbnailBuilder.cpp", "EditorUserClassDefinitionManager.cpp",
    "EditorVar.cpp", "EditorViewport.cpp", "EditorWindow.cpp",
    "EditorWindowAreas.cpp", "EditorWindowBillboards.cpp", "EditorWindowCombine.cpp",
    "EditorWindowDecals.cpp", "EditorWindowEditModeSidebar.cpp",
    "EditorWindowEntities.cpp", "EditorWindowEntityEditBox.cpp",
    "EditorWindowEntityEditBoxArea.cpp", "EditorWindowEntityEditBoxBillboard.cpp",
    "EditorWindowEntityEditBoxCompound.cpp", "EditorWindowEntityEditBoxDecal.cpp",
    "EditorWindowEntityEditBoxEntity.cpp", "EditorWindowEntityEditBoxFogArea.cpp",
    "EditorWindowEntityEditBoxGroup.cpp", "EditorWindowEntityEditBoxLight.cpp",
    "EditorWindowEntityEditBoxParticleSystem.cpp",
    "EditorWindowEntityEditBoxPrimitive.cpp", "EditorWindowEntityEditBoxSound.cpp",
    "EditorWindowEntityEditBoxStaticObject.cpp", "EditorWindowEntitySearch.cpp",
    "EditorWindowFactory.cpp", "EditorWindowFogAreas.cpp", "EditorWindowLights.cpp",
    "EditorWindowLoaderStatus.cpp", "EditorWindowLowerToolbar.cpp",
    "EditorWindowMaterialEditor.cpp", "EditorWindowObjectBrowser.cpp",
    "EditorWindowOptions.cpp", "EditorWindowParticleSystems.cpp",
    "EditorWindowPrimitives.cpp", "EditorWindowSelect.cpp",
    "EditorWindowSoundBrowser.cpp", "EditorWindowSounds.cpp",
    "EditorWindowStaticObjects.cpp", "EditorWindowTextureBrowser.cpp",
    "EditorWindowViewport.cpp", "EditorWorld.cpp", "EngineEntity.cpp",
    "EntityIcon.cpp", "EntityPicker.cpp", "EntityWrapper.cpp",
    "EntityWrapperArea.cpp", "EntityWrapperBillboard.cpp",
    "EntityWrapperCompoundObject.cpp", "EntityWrapperDecal.cpp",
    "EntityWrapperEntity.cpp", "EntityWrapperFogArea.cpp", "EntityWrapperLight.cpp",
    "EntityWrapperLightBox.cpp", "EntityWrapperLightPoint.cpp",
    "EntityWrapperLightSpot.cpp", "EntityWrapperLightArea.cpp",
    "EntityWrapperParticleSystem.cpp", "EntityWrapperPrimitive.cpp",
    "EntityWrapperPrimitivePlane.cpp", "EntityWrapperSound.cpp",
    "EntityWrapperStaticObject.cpp", "SphereCreator.cpp", "StdAfx.cpp",
    "SurfacePicker.cpp",
    "LevelEditor.cpp", "LevelEditorActions.cpp", "LevelEditorMain.cpp",
    "LevelEditorMCPServer.cpp", "LevelEditorMCPCommands.cpp",
    "LevelEditorCameraCapture.cpp",
    "LevelEditorStaticObjectCombo.cpp", "LevelEditorWindow.cpp",
    "LevelEditorWindowGroup.cpp", "LevelEditorWindowHierarchy.cpp",
    "LevelEditorWindowLevelSettings.cpp",
    "LevelEditorEntFileSession.cpp",
    "LevelEditorWorld.cpp",
    "EditorFileWatcher.cpp", "PrefabManager.cpp",
    -- Headless .ent authoring for the MCP create_entity_file tool: the
    -- ModelEditor's document class + the wrapper/editbox TUs it links against.
    "ModelEditorWorld.cpp",
    "EntityWrapperSubMesh.cpp", "EntityWrapperBone.cpp",
    "EntityWrapperBody.cpp", "EntityWrapperBodyShape.cpp",
    "EntityWrapperJoint.cpp", "EntityWrapperJointBall.cpp",
    "EntityWrapperJointHinge.cpp", "EntityWrapperJointScrew.cpp",
    "EntityWrapperJointSlider.cpp",
    "EditorActionsBodies.cpp", "EditorActionsSubMesh.cpp",
    "EditorWindowEntityEditBoxSubMesh.cpp", "EditorWindowEntityEditBoxBone.cpp",
    "EditorWindowEntityEditBoxBody.cpp", "EditorWindowEntityEditBoxBodyShape.cpp",
    "EditorWindowEntityEditBoxGroupShapes.cpp", "EditorWindowEntityEditBoxJoint.cpp",
    -- Body (shape) + Joint creator edit modes and their subtype panels, so the
    -- LevelEditor's scoped ent-file session can create/edit shapes and joints in
    -- place (previously ModelEditor-only).
    "EditorEditModeBodies.cpp", "EditorEditModeJoints.cpp",
    "EditorWindowBodies.cpp", "EditorWindowJoints.cpp",
}

local modeleditor = {
    "BoxCreator.cpp", "DirectoryHandler.cpp", "EditorAction.cpp",
    "EditorActionCompoundObject.cpp", "EditorActionEntity.cpp",
    "EditorActionHandler.cpp", "EditorActionMisc.cpp", "EditorActionSelection.cpp",
    "EditorActionsBodies.cpp", "EditorActionsSubMesh.cpp",
    "EditorAxisAlignedPlane.cpp", "EditorBaseClasses.cpp", "EditorClipPlane.cpp",
    "EditorEditMode.cpp", "EditorEditModeBillboards.cpp", "EditorEditModeBodies.cpp",
    "EditorEditModeJoints.cpp", "EditorEditModeLights.cpp",
    "EditorEditModeParticleSystems.cpp", "EditorEditModeSelect.cpp",
    "EditorEditModeSelectTool.cpp", "EditorEditModeSelectToolRotate.cpp",
    "EditorEditModeSelectToolScale.cpp", "EditorEditModeSelectToolTranslate.cpp",
    "EditorEditModeSounds.cpp", "EditorGrid.cpp", "EditorHelper.cpp",
    "EditorIndex.cpp", "EditorInput.cpp", "EditorSelection.cpp",
    "EditorThumbnailBuilder.cpp", "EditorUserClassDefinitionManager.cpp",
    "EditorVar.cpp", "EditorViewport.cpp", "EditorWindow.cpp",
    "EditorWindowBillboards.cpp", "EditorWindowBodies.cpp",
    "EditorWindowEditModeSidebar.cpp", "EditorWindowEntities.cpp",
    "EditorWindowEntityEditBox.cpp", "EditorWindowEntityEditBoxBillboard.cpp",
    "EditorWindowEntityEditBoxBody.cpp", "EditorWindowEntityEditBoxBodyShape.cpp",
    "EditorWindowEntityEditBoxBone.cpp", "EditorWindowEntityEditBoxCompound.cpp",
    "EditorWindowEntityEditBoxGroup.cpp", "EditorWindowEntityEditBoxGroupShapes.cpp",
    "EditorWindowEntityEditBoxJoint.cpp", "EditorWindowEntityEditBoxLight.cpp",
    "EditorWindowEntityEditBoxParticleSystem.cpp",
    "EditorWindowEntityEditBoxSound.cpp", "EditorWindowEntityEditBoxSubMesh.cpp",
    "EditorWindowEntitySearch.cpp", "EditorWindowFactory.cpp",
    "EditorWindowJoints.cpp", "EditorWindowLights.cpp",
    "EditorWindowLoaderStatus.cpp", "EditorWindowLowerToolbar.cpp",
    "EditorWindowMaterialEditor.cpp", "EditorWindowObjectBrowser.cpp",
    "EditorWindowOptions.cpp", "EditorWindowParticleSystems.cpp",
    "EditorWindowSelect.cpp", "EditorWindowSoundBrowser.cpp",
    "EditorWindowSounds.cpp", "EditorWindowTextureBrowser.cpp",
    "EditorWindowViewport.cpp", "EditorWorld.cpp", "EngineEntity.cpp",
    "EntityIcon.cpp", "EntityPicker.cpp", "EntityWrapper.cpp",
    "EntityWrapperBillboard.cpp", "EntityWrapperBody.cpp",
    "EntityWrapperBodyShape.cpp", "EntityWrapperBone.cpp",
    "EntityWrapperCompoundObject.cpp", "EntityWrapperJoint.cpp",
    "EntityWrapperJointBall.cpp", "EntityWrapperJointHinge.cpp",
    "EntityWrapperJointScrew.cpp", "EntityWrapperJointSlider.cpp",
    "EntityWrapperLight.cpp", "EntityWrapperLightBox.cpp",
    "EntityWrapperLightPoint.cpp", "EntityWrapperLightSpot.cpp",
    "EntityWrapperLightArea.cpp", "EntityWrapperParticleSystem.cpp",
    "EntityWrapperSound.cpp", "EntityWrapperSubMesh.cpp", "SphereCreator.cpp",
    "StdAfx.cpp", "SurfacePicker.cpp",
    "ModelEditor.cpp", "ModelEditorActions.cpp", "ModelEditorLowerToolbar.cpp",
    "ModelEditorMain.cpp", "ModelEditorWindowAnimations.cpp",
    "ModelEditorWindowOutline.cpp", "ModelEditorWindowPhysicsTest.cpp",
    "ModelEditorWindowUserSettings.cpp", "ModelEditorWorld.cpp",
    "EditorFileWatcher.cpp", "PrefabManager.cpp", "ModelEditorReloadNotify.cpp",
}

local particleeditor = {
    "DirectoryHandler.cpp", "EditorAction.cpp", "EditorActionCompoundObject.cpp",
    "EditorActionEntity.cpp", "EditorActionHandler.cpp", "EditorActionMisc.cpp",
    "EditorActionSelection.cpp", "EditorAxisAlignedPlane.cpp",
    "EditorBaseClasses.cpp", "EditorClipPlane.cpp", "EditorEditMode.cpp",
    "EditorEditModeSelect.cpp", "EditorEditModeSelectTool.cpp",
    "EditorEditModeSelectToolRotate.cpp", "EditorEditModeSelectToolScale.cpp",
    "EditorEditModeSelectToolTranslate.cpp", "EditorGrid.cpp", "EditorHelper.cpp",
    "EditorIndex.cpp", "EditorInput.cpp", "EditorSelection.cpp",
    "EditorThumbnailBuilder.cpp", "EditorUserClassDefinitionManager.cpp",
    "EditorVar.cpp", "EditorViewport.cpp", "EditorWindow.cpp",
    "EditorWindowEditModeSidebar.cpp", "EditorWindowEntityEditBox.cpp",
    "EditorWindowEntityEditBoxCompound.cpp", "EditorWindowEntityEditBoxGroup.cpp",
    "EditorWindowEntitySearch.cpp", "EditorWindowFactory.cpp",
    "EditorWindowLoaderStatus.cpp", "EditorWindowLowerToolbar.cpp",
    "EditorWindowMaterialEditor.cpp", "EditorWindowOptions.cpp",
    "EditorWindowSelect.cpp", "EditorWindowSoundBrowser.cpp",
    "EditorWindowTextureBrowser.cpp", "EditorWindowViewport.cpp", "EditorWorld.cpp",
    "EngineEntity.cpp", "EntityIcon.cpp", "EntityPicker.cpp", "EntityWrapper.cpp",
    "EntityWrapperCompoundObject.cpp", "StdAfx.cpp", "SurfacePicker.cpp",
    "EntityWrapperParticleEmitter.cpp", "ParticleEditor.cpp",
    "ParticleEditorActions.cpp", "ParticleEditorMain.cpp",
    "ParticleEditorWindowEmitterParams.cpp", "ParticleEditorWindowEmitters.cpp",
    "ParticleEditorWorld.cpp",
    "EditorFileWatcher.cpp", "PrefabManager.cpp", "ParticleEditorReloadNotify.cpp",
}

local materialeditor = {
    "DirectoryHandler.cpp", "EditorAction.cpp", "EditorActionCompoundObject.cpp",
    "EditorActionEntity.cpp", "EditorActionHandler.cpp", "EditorActionMisc.cpp",
    "EditorActionSelection.cpp", "EditorAxisAlignedPlane.cpp",
    "EditorBaseClasses.cpp", "EditorClipPlane.cpp", "EditorEditMode.cpp",
    "EditorEditModeSelect.cpp", "EditorEditModeSelectTool.cpp",
    "EditorEditModeSelectToolRotate.cpp", "EditorEditModeSelectToolScale.cpp",
    "EditorEditModeSelectToolTranslate.cpp", "EditorGrid.cpp", "EditorHelper.cpp",
    "EditorIndex.cpp", "EditorInput.cpp", "EditorSelection.cpp",
    "EditorThumbnailBuilder.cpp", "EditorUserClassDefinitionManager.cpp",
    "EditorVar.cpp", "EditorViewport.cpp", "EditorWindow.cpp",
    "EditorWindowEditModeSidebar.cpp", "EditorWindowEntityEditBox.cpp",
    "EditorWindowEntityEditBoxCompound.cpp", "EditorWindowEntityEditBoxGroup.cpp",
    "EditorWindowEntitySearch.cpp", "EditorWindowFactory.cpp",
    "EditorWindowLoaderStatus.cpp", "EditorWindowLowerToolbar.cpp",
    "EditorWindowMaterialEditor.cpp", "EditorWindowOptions.cpp",
    "EditorWindowSelect.cpp", "EditorWindowSoundBrowser.cpp",
    "EditorWindowTextureBrowser.cpp", "EditorWindowViewport.cpp", "EditorWorld.cpp",
    "EngineEntity.cpp", "EntityIcon.cpp", "EntityPicker.cpp", "EntityWrapper.cpp",
    "EntityWrapperCompoundObject.cpp", "StdAfx.cpp", "SurfacePicker.cpp",
    "MaterialEditor.cpp", "MaterialEditorMain.cpp",
    "EditorFileWatcher.cpp", "PrefabManager.cpp",
}

editor_target("LevelEditor",    "leveleditor",    leveleditor)
editor_target("ModelEditor",    "modeleditor",    modeleditor)
editor_target("ParticleEditor", "particleeditor", particleeditor)
editor_target("MaterialEditor", "materialeditor", materialeditor)

-- MshConverter -- single source file, links HPL2 directly (not via AddToolTarget).
project "MshConverter"
    kind "ConsoleApp"
    language "C++"
    set_output("runtime")
    files { TOOLS .. "/mshconverter/MshConverter.cpp" }
    includedirs {
        ROOT .. "/HPL2/core/include",
        ROOT .. "/amnesia/slang",
        ROOT .. "/amnesia/glsl",
        DEPS_SOURCES .. "/AngelScript/include",
        DEPS_EXTERN .. "/tinyxml2",
    }
    deps_public_includes()
    vulkan_includes()
    mathlib_use()
    link_engine()
    filter "system:linux"
        linkoptions { "-Wl,-rpath,'$$ORIGIN/libs'", "-Wl,-rpath,'$$ORIGIN'" }
    filter {}
