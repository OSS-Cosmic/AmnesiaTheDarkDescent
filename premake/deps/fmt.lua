-- fmt (fmtlib/fmt, pinned to 12.2.0) -- compiled static lib.
--
-- Only format.cc and os.cc are built. src/fmt.cc is the C++20 module interface unit and
-- src/fmt-c.cc is the optional C API; neither belongs in a classic header/TU build.
--
-- openal-soft vendors its own fmt 11.2.0 and links it privately into OpenAL32. That copy
-- is unreachable from here (EXCLUDE_FROM_ALL, headers never exported by link_openal()) and
-- does not collide at link time: fmt wraps its API in an inline namespace tagged with the
-- major version, so this copy's symbols are fmt::v12 and openal-soft's are fmt::v11.
--
-- FMT_USE_EXCEPTIONS mirrors the per-toolchain exceptionhandling setting on the HPL2
-- project (premake/hpl2.lua: "Off" for gcc/clang, "On" for MSVC). fmt does auto-detect
-- -fno-exceptions via __EXCEPTIONS/__cpp_exceptions (base.h), but the define is set
-- explicitly on BOTH this project and its consumers (fmt_use() in premake/helpers.lua) so
-- the library and the TUs including its headers can never disagree about FMT_THROW -- the
-- macro is used from inline/template code, so a mismatch is an ODR violation, not a
-- compile error. Note the macro is FMT_USE_EXCEPTIONS, not FMT_EXCEPTIONS: the latter is
-- not read anywhere in fmt 12 and would be silently ignored.
project "fmt"
    kind "StaticLib"
    language "C++"
    set_output("static")

    files {
        DEPS_EXTERN .. "/fmt/src/format.cc",
        DEPS_EXTERN .. "/fmt/src/os.cc",
    }
    includedirs { DEPS_EXTERN .. "/fmt/include" }

    filter "toolset:gcc or clang"
        exceptionhandling "Off"
        defines { "FMT_USE_EXCEPTIONS=0" }
    -- fmt's base.h static_asserts that the literal encoding is UTF-8 unless FMT_UNICODE is
    -- 0, which on MSVC means building with /utf-8. We do NOT do that: the workspace sets
    -- characterset "MBCS" on purpose (premake5.lua) and /utf-8 would switch the execution
    -- charset of every string literal in the engine, not just fmt's. FMT_UNICODE=0 only
    -- gives up fmt's UTF-8 transcoding when writing to a Windows console -- HPL2 logs via
    -- its own Log(), never fmt::print to a console, so nothing here depends on it.
    -- gcc/clang default to UTF-8 literals and FMT_WIN32 is 0 there, so this is MSVC-only.
    filter "system:windows"
        defines { "FMT_UNICODE=0" }
    filter {}
