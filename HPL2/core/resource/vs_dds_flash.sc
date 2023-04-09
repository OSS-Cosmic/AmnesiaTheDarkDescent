$input a_position, a_texcoord0, a_normal
$output v_texcoord0, v_normal

#include <common.sh>

uniform mat4 u_mtxUV;
uniform mat4 u_normalMtx;

void main()
{
    vec3 wpos = mul(u_model[0], vec4(a_position, 1.0)).xyz;
    mat3 normalMtx = normalMatrix3x3(mul(u_view, u_model[0]));
    v_normal = normalize(mul(normalMtx, a_normal.xyz));
    v_texcoord0 = a_texcoord0;
    gl_Position = mul(u_viewProj, vec4(wpos, 1.0));
}