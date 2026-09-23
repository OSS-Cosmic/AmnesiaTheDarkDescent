#pragma once

// Shared shim for host/device shader headers. Each translation unit
// reaches here as one of three languages — Slang, C++, or GLSL — and the
// macros below paper over their divergent rules around namespace-scope
// constant declarations (Slang defaults declarations to `internal` and
// needs explicit `public` to cross `import`; C++ rejects `public` at
// namespace scope; GLSL rejects `static`). Detection keys off compiler-
// predefined macros so the build system doesn't need a per-target opt-in:
//
//   * Slang: slangc predefines `__SLANG_COMPILER__`.
//   * C++:   every conforming compiler predefines `__cplusplus`.
//   * GLSL:  neither is defined — implicit fall-through.

// Two target languages now: Slang and C++. The GLSL shader set under
// amnesia/glsl/ has been removed; if it ever comes back, this is where
// the HLSL→native `#define float3 vec3` chain would re-appear.
#ifdef __SLANG_COMPILER__
    #define SLANG_PUBLIC public
    #define SHARED_CONST public static const
    #define SHARED_STRUCT
#else  // __cplusplus
    #define SLANG_PUBLIC
    #define SHARED_CONST static const
    #define SHARED_STRUCT struct alignas(16) 
#endif

// -----------------------------------------------------------------------------
// C++ host types. Vector types are real structs (constexpr constructors,
// `operator[]`, named .x/.y/.z/.w members) so SceneTypes.slang's
// `uint2(W, H)` / `float3(0.f)` constants compile in C++ identically to
// Slang. Matrix types stay as float arrays (no constructor-call syntax in
// any shared declaration, only used as struct fields + memcpy'd from
// `ml::float4x4`). Sizes are bit-for-bit GPU-layout compatible.
// -----------------------------------------------------------------------------
#ifdef __cplusplus
#include <cstdint>
#include <cstddef>

namespace hpl {

typedef uint32_t uint;

struct float2 {
    float x, y;
    float2() = default;
    constexpr float2(float a, float b) : x(a), y(b) {}
    constexpr explicit float2(float s) : x(s), y(s) {}
    float& operator[](size_t i) { return (&x)[i]; }
    const float& operator[](size_t i) const { return (&x)[i]; }
};

struct float3 {
    float x, y, z;
    float3() = default;
    constexpr float3(float a, float b, float c) : x(a), y(b), z(c) {}
    constexpr explicit float3(float s) : x(s), y(s), z(s) {}
    float& operator[](size_t i) { return (&x)[i]; }
    const float& operator[](size_t i) const { return (&x)[i]; }
};

struct float4 {
    float x, y, z, w;
    float4() = default;
    constexpr float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
    constexpr explicit float4(float s) : x(s), y(s), z(s), w(s) {}
    float& operator[](size_t i) { return (&x)[i]; }
    const float& operator[](size_t i) const { return (&x)[i]; }
};

struct uint2 {
    uint32_t x, y;
    uint2() = default;
    constexpr uint2(uint32_t a, uint32_t b) : x(a), y(b) {}
    constexpr explicit uint2(uint32_t s) : x(s), y(s) {}
    uint32_t& operator[](size_t i) { return (&x)[i]; }
    const uint32_t& operator[](size_t i) const { return (&x)[i]; }
};

struct uint3 {
    uint32_t x, y, z;
    uint3() = default;
    constexpr uint3(uint32_t a, uint32_t b, uint32_t c) : x(a), y(b), z(c) {}
    constexpr explicit uint3(uint32_t s) : x(s), y(s), z(s) {}
    uint32_t& operator[](size_t i) { return (&x)[i]; }
    const uint32_t& operator[](size_t i) const { return (&x)[i]; }
};

struct uint4 {
    uint32_t x, y, z, w;
    uint4() = default;
    constexpr uint4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) : x(a), y(b), z(c), w(d) {}
    constexpr explicit uint4(uint32_t s) : x(s), y(s), z(s), w(s) {}
    uint32_t& operator[](size_t i) { return (&x)[i]; }
    const uint32_t& operator[](size_t i) const { return (&x)[i]; }
};

struct int2 {
    int32_t x, y;
    int2() = default;
    constexpr int2(int32_t a, int32_t b) : x(a), y(b) {}
    constexpr explicit int2(int32_t s) : x(s), y(s) {}
    int32_t& operator[](size_t i) { return (&x)[i]; }
    const int32_t& operator[](size_t i) const { return (&x)[i]; }
};

struct int3 {
    int32_t x, y, z;
    int3() = default;
    constexpr int3(int32_t a, int32_t b, int32_t c) : x(a), y(b), z(c) {}
    constexpr explicit int3(int32_t s) : x(s), y(s), z(s) {}
    int32_t& operator[](size_t i) { return (&x)[i]; }
    const int32_t& operator[](size_t i) const { return (&x)[i]; }
};

struct int4 {
    int32_t x, y, z, w;
    int4() = default;
    constexpr int4(int32_t a, int32_t b, int32_t c, int32_t d) : x(a), y(b), z(c), w(d) {}
    constexpr explicit int4(int32_t s) : x(s), y(s), z(s), w(s) {}
    int32_t& operator[](size_t i) { return (&x)[i]; }
    const int32_t& operator[](size_t i) const { return (&x)[i]; }
};

// Matrices stay as plain float arrays — the shared declarations never
// construct them via call syntax, only declare struct fields + memcpy
// from `ml::float4x4` host-side.
typedef float float3x3[9];
typedef float float4x4[16];

} // namespace hpl
#endif

// -----------------------------------------------------------------------------
// Namespace wrapper macros. C++ opens `namespace hpl {` so the shared
// constants + struct definitions land at `hpl::X`; Slang and GLSL see
// the macros expand to nothing (their declarations live at module / file
// scope respectively).
// -----------------------------------------------------------------------------
#ifdef __cplusplus
    #define HOST_NAMESPACE_BEGIN namespace hpl {
    #define HOST_NAMESPACE_END   }
#else
    #define HOST_NAMESPACE_BEGIN
    #define HOST_NAMESPACE_END
#endif

// -----------------------------------------------------------------------------
// Pinned DXIL register for the shared push-constant block. Same reasoning as
// gPerFrame's b3 pin (PerFrame/resource.slang): a D3D12 root signature is shared
// by every stage of a program, so a resource must land on the same register in
// every stage's reflection -- but slangc numbers registers per compilation unit
// and each entry point is compiled separately, so a vertex/fragment pair split
// across two files disagrees whenever they declare different globals. That is
// exactly how outline_alpha and glow_object broke: the vertex shader's `pass`
// CBV took b0, pushing its gPushConstants to b1, while the fragment shader --
// which declares no `pass` -- put gPushConstants at b0.
//
// A reserved space is used rather than a high b# in space0 because slangc only
// reserves an explicit binding within the unit that declares it: a stage that
// does NOT declare gPushConstants could still auto-assign that register to
// something else. Nothing is ever auto-assigned outside space0, so space9 is
// immune to that by construction.
//
// Vulkan is unaffected: vk::push_constant governs there, register() is
// HLSL-only, and only the DXIL compile is given -DDXIL. The __SLANG_COMPILER__
// guard keeps register() out of the C++ translation units that share this file.
// -----------------------------------------------------------------------------
#if defined(__SLANG_COMPILER__) && defined(DXIL)
    #define HPL_PUSH_CONSTANT_REGISTER : register(b0, space9)
#else
    #define HPL_PUSH_CONSTANT_REGISTER
#endif

// -----------------------------------------------------------------------------
// Type discriminators. Every Material struct stores its discriminator in `type`
// at offset 0; the IMaterial interface relies on it. (Lights carry no type
// field — their type is implied by which SSBO the entry lives in; see
// Scene.slang getLight().)
// -----------------------------------------------------------------------------
#define MATERIAL_TYPE_DIFFUSE       0u
#define MATERIAL_TYPE_TRANSLUCENT   1u
#define MATERIAL_TYPE_WATER         2u
#define MATERIAL_TYPE_DECAL         3u
