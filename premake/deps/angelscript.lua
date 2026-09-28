-- AngelScript (scripting) -- explicit source list, not a glob.
-- The x64 MSVC assembly callfunc is added (and assembled via MASM) only on VS.
--
-- Each product compiles the AngelScript its scripts were written for:
--   AngelScript      2.19.2  TDD (and tools/tests)   extern/AngelScript_2.19.2
--   AngelScript_AMFP 2.24.1  AMFP                    extern/AngelScript_2.24.1
--                            (the version the retail AMFP binaries report);
--                            its maps use array<T> and the std::string add-on.
ANGELSCRIPT_INCLUDE = DEPS_SOURCES .. "/AngelScript_2.19.2/include"
ANGELSCRIPT_AMFP_INCLUDE = DEPS_SOURCES .. "/AngelScript_2.24.1/include"

local function angelscript_project(name, dir, names)
project(name)
    kind "StaticLib"
    language "C++"
    set_output("static")

    local sd = dir .. "/sources/"
    for _, n in ipairs(names) do files { sd .. n .. ".cpp" } end
    includedirs { dir .. "/include" }

    filter "system:windows"
        files { sd .. "as_callfunc_x64_msvc_asm.asm" }
    filter { "system:windows", "files:**as_callfunc_x64_msvc_asm.asm" }
        defines { "_M_X64" }
    filter {}
end

angelscript_project("AngelScript", DEPS_SOURCES .. "/AngelScript_2.19.2", {
    "as_arrayobject", "as_atomic", "as_builder", "as_bytecode",
    "as_callfunc_arm", "as_callfunc_mips", "as_callfunc_ppc_64",
    "as_callfunc_ppc", "as_callfunc_sh4", "as_callfunc_x64_gcc",
    "as_callfunc_x64_msvc", "as_callfunc_x86", "as_callfunc_xenon",
    "as_callfunc", "as_compiler", "as_configgroup", "as_context",
    "as_datatype", "as_gc", "as_generic", "as_globalproperty", "as_memory",
    "as_module", "as_objecttype", "as_outputbuffer", "as_parser",
    "as_restore", "as_scriptcode", "as_scriptengine", "as_scriptfunction",
    "as_scriptnode", "as_scriptobject", "as_string_util", "as_string",
    "as_thread", "as_tokenizer", "as_typeinfo", "as_variablescope",
})

angelscript_project("AngelScript_AMFP", DEPS_SOURCES .. "/AngelScript_2.24.1", {
    "as_atomic", "as_builder", "as_bytecode",
    "as_callfunc_arm", "as_callfunc_mips", "as_callfunc_ppc_64",
    "as_callfunc_ppc", "as_callfunc_sh4", "as_callfunc_x64_gcc",
    "as_callfunc_x64_mingw", "as_callfunc_x64_msvc", "as_callfunc_x86",
    "as_callfunc_xenon", "as_callfunc", "as_compiler", "as_configgroup",
    "as_context", "as_datatype", "as_gc", "as_generic", "as_globalproperty",
    "as_memory", "as_module", "as_objecttype", "as_outputbuffer", "as_parser",
    "as_restore", "as_scriptcode", "as_scriptengine", "as_scriptfunction",
    "as_scriptnode", "as_scriptobject", "as_string_util", "as_string",
    "as_thread", "as_tokenizer", "as_typeinfo", "as_variablescope",
})
