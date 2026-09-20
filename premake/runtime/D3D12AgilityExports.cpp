// SPDX-License-Identifier: See premake/agility.lua for redistribution terms.
// Data exports read by d3d12.dll to load the app-local Agility SDK core.
// Attached to every final DX12 executable through link_agility_runtime in
// premake/agility.lua so the linker cannot drop the symbols from a static lib.

#include <windows.h>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619u;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
