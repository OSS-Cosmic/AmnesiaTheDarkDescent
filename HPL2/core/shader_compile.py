import argparse
from enum import Enum
import subprocess
import sys
import os

parser = argparse.ArgumentParser(description='generate core shaders.')
parser.add_argument('--output', dest='output', action='store', default='core/shaders', help='output directory')
parser.add_argument('--compiler', dest='compiler', action='store', help='shaderc compiler from bgfx')
parser.add_argument('--bgfx', dest='bgfx', action='store', help='bgfx root path for include')

class ShaderType(Enum):
    FS = 0
    VS = 1
    CS = 2


shaders = [
# deferred
    { "type" : ShaderType.VS, "inout" : "resource/vs_deferred_light.io",              "input": "resource/vs_deferred_light.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_deferred_light.io",              "input": "resource/fs_deferred_pointlight.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_deferred_light.io",              "input": "resource/fs_deferred_spotlight.sc", "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_light_box.io",                   "input": "resource/vs_light_box.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_light_box.io",                   "input": "resource/fs_light_box.sc", "includes": ["resource"]},
#gui
    { "type" : ShaderType.FS, "inout" : "resource/vs_gui.io",                          "input": "resource/fs_gui.sc", "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_gui.io",                          "input": "resource/vs_gui.sc", "includes": ["resource"]},
#  post effects
    { "type" : ShaderType.FS, "inout" : "resource/vs_post_effect.io",                  "input": "resource/fs_posteffect_fullscreen_fog.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_post_effect.io",                  "input": "resource/fs_posteffect_blur.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_post_effect.io",                  "input": "resource/fs_posteffect_bloom_add.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_post_effect.io",                  "input": "resource/fs_posteffect_image_trail_frag.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_post_effect.io",                  "input": "resource/fs_posteffect_radial_blur_frag.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_post_effect.io",                  "input": "resource/fs_posteffect_color_conv.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_post_effect.io",                  "input": "resource/fs_post_effect_copy.sc", "includes": ["resource"]},
# other
    { "type" : ShaderType.VS, "inout" : "resource/vs_null.io",                         "input": "resource/vs_null.sc" , "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_null.io",                         "input": "resource/fs_null.sc" , "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_basic_solid_diffuse.io",          "input": "resource/vs_basic_solid_diffuse.sc" , "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_basic_solid_illumination.io",     "input": "resource/vs_basic_solid_illumination.sc", "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_basic_solid_z.io",                "input": "resource/vs_basic_solid_z.sc", "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_basic_translucent_material.io",   "input": "resource/vs_basic_translucent_material.sc", "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_decal_material.io",               "input": "resource/vs_decal_material.sc", "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_deferred_fog.io",                 "input": "resource/vs_deferred_fog.sc", "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_post_effect.io",                  "input": "resource/vs_post_effect.sc", "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_water_material.io",               "input": "resource/vs_water_material.sc", "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_simple_flat.io",                  "input": "resource/vs_simple_flat.sc" , "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_simple_flat.io",                  "input": "resource/fs_simple_flat.sc" , "includes": ["resource"]},
    { "type" : ShaderType.VS, "inout" : "resource/vs_simple_diffuse.io",               "input": "resource/vs_simple_diffuse.sc" , "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_simple_diffuse.io",               "input": "resource/fs_simple_diffuse.sc" , "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_basic_solid_diffuse.io",          "input": "resource/fs_basic_solid_diffuse.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_basic_solid_illumination.io",     "input": "resource/fs_basic_solid_illumination.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_basic_solid_z.io",                "input": "resource/fs_basic_solid_z.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_basic_translucent_material.io",   "input": "resource/fs_basic_translucent_material.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_decal_material.io",               "input": "resource/fs_decal_material.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_deferred_fog.io",                 "input": "resource/fs_deferred_fog.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_simple_diffuse.io",               "input": "resource/fs_simple_diffuse.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_simple_flat.io",                  "input": "resource/fs_simple_flat.sc", "includes": ["resource"]},
    { "type" : ShaderType.FS, "inout" : "resource/vs_water_material.io",               "input": "resource/fs_water_material.sc", "includes": ["resource"]},
]

def toD3dPrefix(shaderType):
    if shaderType == ShaderType.FS:
        return "ps"
    elif shaderType == ShaderType.VS:
        return "vs"
    elif shaderType == ShaderType.CS:
        return "cs"
    else:
        raise Exception("Unknown shader type")

def toType(shaderType):
    if shaderType == ShaderType.FS:
        return "fragment"
    elif shaderType == ShaderType.VS:
        return "vertex"
    elif shaderType == ShaderType.CS:
        return "compute"
    else:
        raise Exception("Unknown shader type")

def wrap_subprocess(*args, **kwargs):
    print(f'cmd: {" ".join(args[0])}')
    try:
        output = subprocess.check_output(*args, **kwargs)
    except subprocess.CalledProcessError as exc:
        print(f'{exc.output.decode("utf-8")}')
    else:
        print("Output: \n{}\n".format(output))


def main():
    args = parser.parse_args()

    os.makedirs(f'{args.output}/shaders/dx9/', exist_ok=True)
    os.makedirs(f'{args.output}/shaders/dx11/', exist_ok=True)
    os.makedirs(f'{args.output}/shaders/osx/', exist_ok=True)
    os.makedirs(f'{args.output}/shaders/glsl/', exist_ok=True)
    os.makedirs(f'{args.output}/shaders/essl/', exist_ok=True)
    os.makedirs(f'{args.output}/shaders/spirv/', exist_ok=True)
    
    for shader in shaders:
        input_file_path = os.path.abspath(shader["input"])
        name = os.path.basename(input_file_path).split(".")[0]
        varying_def_path = os.path.abspath(shader["inout"])
        includes = [item for inc in shader["includes"] for item in ['-i', os.path.abspath(inc)]]

        if sys.platform == 'win32':
            # dx9
            if not shader["type"] == ShaderType.CS:
                wrap_subprocess([
                    args.compiler,
                    '-f', f'{input_file_path}',
                    '-o', f'{args.output}/shaders/dx9/{name}.bin',
                    '--type', f'{toType(shader["type"])}',
                    '--platform', " windows",
                    '--varyingdef', f'{varying_def_path}',
                    '--profile', f'{toD3dPrefix(shader["type"])}_3_0',
                    '-O', "3",
                    '-i', f'{args.bgfx}/src',
                    ] + includes)
            
            if not shader["type"] == ShaderType.CS:
                wrap_subprocess([
                    args.compiler,
                    '-f', f'{input_file_path}',
                    '-o', f'{args.output}/shaders/dx11/{name}.bin',
                    '--type', f' {toType(shader["type"])}',
                    '--platform', " windows",
                    '--varyingdef', f'{varying_def_path}',
                    '--profile', f'{toD3dPrefix(shader["type"])}_5_0',
                    '-O', "3",
                    '-i', f'{args.bgfx}/src',
                    ] + includes)
            else:
                wrap_subprocess([
                    args.compiler,
                    '-f', f'{input_file_path}',
                    '-o', f'{args.output}/shaders/dx11/{name}.bin',
                    '--type', f'{toType(shader["type"])}',
                    '--platform', "windows",
                    '--varyingdef', f'{varying_def_path}',
                    f'--profile {toD3dPrefix(shader["type"])}_5_0',
                    "-O 1",
                    '-i', f'{args.bgfx}/src',
                    ] + includes)

        if sys.platform == 'darwin':
            wrap_subprocess([
                args.compiler,
                '-f', f'{input_file_path}',
                '-o', f'{args.output}/shaders/osx/{name}.bin',
                '--type', f'{toType(shader["type"])}',
                '--platform', "osx",
                '--varyingdef', f'{varying_def_path}',
                '--profile', f'metal',
                '-O', "3",
                '-i', f'{args.bgfx}/src',
                ] + includes)

        if not shader["type"] == ShaderType.CS:
            wrap_subprocess([
                args.compiler,
                '-f', f'{input_file_path}',
                '-o', f'{args.output}/shaders/essl/{name}.bin',
                '--type', f'{toType(shader["type"])}',
                '--platform ', "android",
                '--varyingdef', f'{varying_def_path}',
                '-i', f'{args.bgfx}/src',
                ] + includes)

        # spirv
        if shader["type"] == ShaderType.CS:
            wrap_subprocess([
                args.compiler,
                '-f', f'{input_file_path}',
                '-o', f'{args.output}/shaders/glsl/{name}.bin',
                '--type', f'{toType(shader["type"])}',
                '--platform', "linux",
                '--varyingdef', f'{varying_def_path}',
                '--profile', f'430',
                '-i', f'{args.bgfx}/src',
                ] + includes)
        else:
            wrap_subprocess([
                args.compiler,
                '-f', f'{input_file_path}',
                '-o', f'{args.output}/shaders/glsl/{name}.bin',
                '--type', f'{toType(shader["type"])}',
                '--platform', "linux",
                '--varyingdef', f'{varying_def_path}',
                '--profile', f'130',
                '-i', f'{args.bgfx}/src',
                ] + includes)
        
        if not shader["type"] == ShaderType.CS:
            wrap_subprocess([
                args.compiler,
                '-f', f'{input_file_path}',
                '-o', f'{args.output}/shaders/spirv/{name}.bin',
                '--type', f'{toType(shader["type"])}',
                '--platform', "linux",
                '--varyingdef', f'{varying_def_path}',
                '--profile', f'spirv',
                '-i', f'{args.bgfx}/src',
                ] + includes)
main()