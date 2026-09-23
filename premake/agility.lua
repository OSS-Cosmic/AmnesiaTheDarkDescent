-- premake/agility.lua -- DirectX 12 Agility SDK acquisition and deployment.

local AGILITY_PACKAGE_ID = 'Microsoft.Direct3D.D3D12'
local AGILITY_VERSION = '1.619.5'
local AGILITY_SDK_VERSION = 619
local AGILITY_NUPKG_SHA256 = '0e9bcf32aac9a79343ede9b21e4864950ee54577e3d8e19bfcdf002bb4e9bfd6'
local AGILITY_URL = 'https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.619.5'
local AGILITY_DEPS = ROOT .. '/build-premake/_deps/agility-sdk'
local AGILITY_ARCHIVE = AGILITY_DEPS .. '/microsoft.direct3d.d3d12.1.619.5.nupkg'
local AGILITY_EXTRACTED = AGILITY_DEPS .. '/microsoft.direct3d.d3d12.1.619.5'

local function winpath(p) return (p:gsub('/', '\\')) end

local function windows_serialized_commands(commands, mutex_name)
    local body = {}
    for _, command in ipairs(commands) do
        table.insert(body, string.format(
            "& cmd.exe /D /C '%s'; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }",
            command))
    end
    local script = string.format(
        "& {$mutex = [System.Threading.Mutex]::new($false, 'Local\\%s'); $acquired = $false; try {try {$mutex.WaitOne(); $acquired = $true} catch [System.Threading.AbandonedMutexException] {$acquired = $true}; %s} finally {if ($acquired) {$mutex.ReleaseMutex()}; $mutex.Dispose()}}",
        mutex_name, table.concat(body, " "))
    script = script:gsub('"', '\\"')
    return "powershell -NoProfile -ExecutionPolicy Bypass -Command \"" .. script .. "\""
end

local function agility_tree_is_valid(root)
    local required = {
        root .. '/build/native/include/d3d12.h',
        root .. '/build/native/include/d3dx12/d3dx12.h',
        root .. '/build/native/bin/x64/D3D12Core.dll',
        root .. '/build/native/bin/x64/d3d12SDKLayers.dll',
    }
    for _, filename in ipairs(required) do
        if not os.isfile(filename) then
            error('Agility: missing required package file: ' .. filename)
        end
    end

    local nuspec = root .. '/' .. AGILITY_PACKAGE_ID .. '.nuspec'
    if not os.isfile(nuspec) then
        error('Agility: missing package manifest: ' .. nuspec)
    end
    local contents = io.open(nuspec, 'r')
    local text = contents:read('*a')
    contents:close()
    local version = text:match('<version>%s*([^<]-)%s*</version>')
    if version ~= AGILITY_VERSION then
        error(string.format('Agility: package version mismatch (expected %s, got %s)',
            AGILITY_VERSION, tostring(version)))
    end

    for _, filename in ipairs({
        root .. '/build/native/bin/x64/D3D12Core.dll',
        root .. '/build/native/bin/x64/d3d12SDKLayers.dll',
    }) do
        local size = os.stat(filename).size
        if not size or size < 1024 * 1024 then
            error('Agility: runtime DLL is suspiciously small: ' .. filename)
        end
    end
    return true
end

local function archive_sha256(filename)
    local command
    if os.target() == 'windows' then
        command = 'certutil -hashfile "' .. winpath(filename) .. '" SHA256'
    else
        command = 'sha256sum "' .. filename .. '"'
    end
    local output = os.outputof(command) or ''
    for candidate in output:gmatch('%x+') do
        if #candidate == 64 then return candidate:lower() end
    end
    error('Agility: could not calculate SHA-256 for ' .. filename)
end

local function resolve_agility_sdk()
    local requested = _OPTIONS['agility-sdk-dir']
    if requested then
        local root = path.getabsolute(requested)
        agility_tree_is_valid(root)
        return root
    end

    if os.isdir(AGILITY_EXTRACTED) then
        agility_tree_is_valid(AGILITY_EXTRACTED)
        return AGILITY_EXTRACTED
    end

    os.mkdir(AGILITY_DEPS)
    if not os.isfile(AGILITY_ARCHIVE) then
        print('Agility: downloading ' .. AGILITY_URL)
        local res, code = http.download(AGILITY_URL, AGILITY_ARCHIVE, {})
        if res ~= 'OK' then
            os.remove(AGILITY_ARCHIVE)
            error(string.format('Agility: download failed (%s, code %s). URL: %s',
                tostring(res), tostring(code), AGILITY_URL))
        end
    end

    local actual_hash = archive_sha256(AGILITY_ARCHIVE)
    if actual_hash ~= AGILITY_NUPKG_SHA256 then
        error(string.format('Agility: SHA-256 mismatch for %s (expected %s, got %s)',
            AGILITY_ARCHIVE, AGILITY_NUPKG_SHA256, actual_hash))
    end

    print('Agility: extracting ' .. AGILITY_ARCHIVE)
    os.mkdir(AGILITY_EXTRACTED)
    zip.extract(AGILITY_ARCHIVE, AGILITY_EXTRACTED)
    agility_tree_is_valid(AGILITY_EXTRACTED)
    return AGILITY_EXTRACTED
end

local AGILITY_ENABLED = os.target() == 'windows' and _OPTIONS['with-d3d12'] == 'yes'

if AGILITY_ENABLED then
    AGILITY_ROOT = resolve_agility_sdk()
    AGILITY_INCLUDE_DIR = AGILITY_ROOT .. '/build/native/include'
    AGILITY_X64_BIN_DIR = AGILITY_ROOT .. '/build/native/bin/x64'
    AGILITY_LICENSE_FILES = {}
    for _, filename in ipairs({ 'LICENSE.txt', 'LICENSE-CODE.txt', 'distributable files.txt', 'README.md' }) do
        table.insert(AGILITY_LICENSE_FILES, {
            source_path = AGILITY_ROOT .. '/' .. filename,
            filename = filename,
        })
    end
    _G.AGILITY_SDK_VERSION = AGILITY_SDK_VERSION

end

-- Staging project declarations run AFTER the workspace exists (project() must be
-- called inside a workspace scope). premake5.lua calls this after the workspace
-- block and after external.lua's own project declarations.
function agility_declare_staging_projects()
    if not AGILITY_ENABLED then return end

    local function add_staging_project(name, runtime_dir, license_dir)
        local build_commands = {
            string.format('if not exist "%s" mkdir "%s"', winpath(runtime_dir), winpath(runtime_dir)),
            string.format('copy /Y "%s" "%s\\"', winpath(AGILITY_X64_BIN_DIR .. '/D3D12Core.dll'), winpath(runtime_dir)),
            string.format('copy /Y "%s" "%s\\"', winpath(AGILITY_X64_BIN_DIR .. '/d3d12SDKLayers.dll'), winpath(runtime_dir)),
            string.format('if not exist "%s" mkdir "%s"', winpath(license_dir), winpath(license_dir)),
        }
        for _, license in ipairs(AGILITY_LICENSE_FILES) do
            table.insert(build_commands, string.format('copy /Y "%s" "%s\\"',
                winpath(license.source_path), winpath(license_dir)))
        end

        project(name)
            kind 'Makefile'
            location (ROOT .. '/build-premake/projects')
            filter 'system:windows'
                buildcommands {
                    windows_serialized_commands(
                        build_commands,
                        'Redux-Amnesia-' .. name .. '-%{cfg.buildcfg}'),
                }
                rebuildcommands {
                    windows_serialized_commands(
                        build_commands,
                        'Redux-Amnesia-' .. name .. '-%{cfg.buildcfg}'),
                }
                cleancommands {
                    string.format('{RMDIR} "%s"', winpath(runtime_dir)),
                    string.format('{RMDIR} "%s"', winpath(license_dir)),
                }
            filter {}
    end

    add_staging_project('AgilityRuntimeGame',
        '%{wks.location}/amnesia/%{cfg.buildcfg}/D3D12',
        '%{wks.location}/amnesia/%{cfg.buildcfg}/licenses/agility')
    add_staging_project('AgilityRuntimeTests',
        '%{wks.location}/tests/%{cfg.buildcfg}/D3D12',
        '%{wks.location}/tests/%{cfg.buildcfg}/licenses/agility')
end

function link_agility_runtime(target_layout)
    if not AGILITY_ENABLED then return end
    files { ROOT .. '/premake/runtime/D3D12AgilityExports.cpp' }
    dependson { target_layout == 'tests' and 'AgilityRuntimeTests' or 'AgilityRuntimeGame' }
end
