$input v_texcoord0, v_normal, v_tangent, v_bitangent, v_view, v_position

#include <common.sh>

SAMPLERCUBE(s_envMap, 0);
SAMPLER2D(s_normalMap, 1);
SAMPLER2D(s_specularMap, 2);
SAMPLER2D(s_heightMap, 3);
SAMPLER2D(s_diffuseMap, 4);
SAMPLER2D(s_envMapAlphaMap, 5);

uniform mat4 u_mtxInvViewRotation;
uniform vec4 u_param[2];
#define u_heightMapScale (u_param[0].x)
#define u_heightMapBias (u_param[0].y)
#define u_fresnelBias (u_param[0].z)
#define u_fresnelPow (u_param[0].w)

#define u_useCubeMapAlpha (u_param[1].x)
#define u_alphaReject (u_param[1].y)

void main()
{
    vec4 diffuseColor = vec4(0.0, 0.0, 0.0, 0.0);
    vec2 texCoord = v_texcoord0.xy;

    #ifdef USE_PARALLAX_MAPS
        vec3 eyeVec = normalize(v_view);

        //Get give normalizedView the length so it reaches bottom.
        float normalLength = 1.0 / eyeVec.z;
        eyeVec *= vec3(normalLength, normalLength, normalLength);	
        
        //Apply scale and bias
        eyeVec.xy *= u_heightMapScale;
        //vec2 vBiasPosOffset = vEyeVec.xy * avHeightMapScaleAndBias.y; <- not working! because the ray casting buggers out when u are really close!
		
        vec3 heightMapPos = vec3(texCoord.xy, 0.0);

        //Determine number of steps based on angle between normal and eye
        float fSteps = floor((1.0 - dot(eyeVec, normalize(v_normal)) ) * 18.0) + 2.0; 
        
        // Do a linear search to find the first intersection
        {
            eyeVec /= fSteps;
            int iterations = int(clamp(fSteps - 1.0, 0.0, 28.0));
            for(int i = 0; i < iterations; i++) 
            { 
                float fDepth = texture2D(s_heightMap, texCoord.xy).w; 
                if(heightMapPos.z < fDepth) {
                    heightMapPos += eyeVec;
                }
            } 
        }

        // Do a binary search to find the exact intersection
        {
            const int lSearchSteps = 6; 
            for(int i=0; i<lSearchSteps; i++) 
            { 
                float fDepth = texture2D(s_heightMap, texCoord.xy).w;
                if(heightMapPos.z < fDepth) {
                    heightMapPos += eyeVec; 
                }
                
                eyeVec *= 0.5; 
                heightMapPos -= eyeVec; 
            } 
        }
        texCoord.xy = heightMapPos.xy;
    #endif

    diffuseColor = texture2D(s_diffuseMap, texCoord.xy);

    if(diffuseColor.w < u_alphaReject ) {
        discard;
    }
    
    vec3 screenNormal = vec3(0, 0, 0);
    
    #ifdef USE_NORMAL_MAP
        vec3 vNormal = texture2D(s_normalMap, texCoord).xyz - 0.5; //No need for full unpack x*2-1, becuase normal is normalized. (but now we do not normalize...)
        screenNormal = normalize(vNormal.x * v_tangent + vNormal.y * v_bitangent + vNormal.z * v_normal);
    #else
        screenNormal = normalize(v_normal);
    #endif

    #ifdef USE_ENVMAP
        vec3 cameraEyeSpace = normalize(v_position);	

        vec3 vEnvUv = reflect(cameraEyeSpace, screenNormal);
        vEnvUv = mul(u_mtxInvViewRotation, vec4(vEnvUv.xyz, 1.0)).xyz;
                    
        vec4 reflectionColor = textureCube(s_envMap, vEnvUv);
        
        float afEDotN = max(dot(-cameraEyeSpace, screenNormal),0.0);
        float fFresnel = Fresnel(afEDotN, u_fresnelBias, u_fresnelPow);
        
        if(0.0 < u_useCubeMapAlpha) {
            reflectionColor *= texture2D(s_envMapAlphaMap, texCoord).w;
        }
                
        gl_FragData[0] = diffuseColor + reflectionColor * fFresnel;
    #else
        gl_FragData[0] = diffuseColor;
    #endif
    
    gl_FragData[1].xyz = screenNormal; 
    gl_FragData[1].w = 0.0;
    gl_FragData[2].xyz = v_position;
    gl_FragData[2].w = 0.0;
    #ifdef USE_SPECULAR_MAPS
        gl_FragData[3].xy = texture2D(s_specularMap, texCoord).xy;
    #else
        gl_FragData[3].xy = vec2(0.0, 0.0);
    #endif
    gl_FragData[3].z = 0.0;
    gl_FragData[3].w = 0.0;
}