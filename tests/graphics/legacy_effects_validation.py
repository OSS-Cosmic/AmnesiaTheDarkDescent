"""Compile retail-effect regressions for both products/backends and read back DXIL.

From a VS x64 developer prompt, build the standalone runner first:
  cl /EHsc /std:c++17 tests/graphics/ri_d3d12/legacy_effects_readback.cpp \
     /Febuild-premake/legacy_effects_readback.exe d3d12.lib dxgi.lib
Then run: python tests/graphics/legacy_effects_validation.py
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / "amnesia/slang"


def main():
    compiler = next((ROOT / "build-premake/_deps/slang-prebuilt").glob("*/bin/slangc.exe"))
    output = ROOT / "build-premake/legacy-shader-check"
    output.mkdir(exist_ok=True)
    runner = ROOT / "build-premake/legacy_effects_readback.exe"
    if not runner.is_file():
        raise RuntimeError("Build legacy_effects_readback.exe first; see module docstring")
    sources = [SHADERS / name for name in (
        "Standard/Standard.light.3d.slang",
        "Standard/Standard.translucent.3d.slang",
        "Standard/Standard.waterReflection.3d.slang",
        "Standard/Standard.water.3d.slang",
        "Water/Water.frag.slang",
        "Water/WaterGuide.frag.slang",
        "PostEffects/posteffect_infection.frag.slang")]
    fixture = ROOT / "tests/graphics/ri_d3d12/shaders/legacy_effects.slang"
    for product in ("tdd", "amfp"):
        for target in ("spirv", "dxil"):
            for source in sources + [fixture]:
                artifact = output / f"{product}-{source.name}.{target}"
                args = [str(compiler), str(source), "-target", target,
                        "-profile", "sm_6_8" if target == "dxil" else "sm_6_6",
                        "-matrix-layout-column-major", "-I", str(SHADERS), "-o", str(artifact)]
                if product == "amfp":
                    args += ["-DAMFP"]
                if target == "dxil":
                    args += ["-DDXIL"]
                else:
                    args += ["-emit-spirv-directly", "-fvk-use-entrypoint-name", "-fvk-use-scalar-layout"]
                if source == fixture:
                    args += ["-entry", "CSMain", "-stage", "compute"]
                result = subprocess.run(args, capture_output=True, text=True)
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                print("PASS compile", product, target, source.name, flush=True)
                if source == fixture and target == "dxil":
                    subprocess.run([str(runner), str(artifact)], check=True)


if __name__ == "__main__":
    main()
