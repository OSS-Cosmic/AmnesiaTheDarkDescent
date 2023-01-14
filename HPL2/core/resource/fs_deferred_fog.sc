
$input v_position, v_ray

#include <common.sh>

SAMPLER2D(s_positionMap, 0);

uniform vec4 u_fogColor;
uniform vec4 u_params[4];

#define u_fogStart u_params[0].x
#define u_fogLength u_params[0].y
#define u_fogFalloffExp u_params[0].z

#define u_fogRayCastStart (vec3(u_params[0].w, u_params[1].x, u_params[1].y))
#define u_fogNegPlaneDistNeg (vec3(u_params[1].z, u_params[1].w, u_params[2].x))
#define u_fogNegPlaneDistPos (vec3(u_params[2].y, u_params[2].z, u_params[2].w))

#define u_useBacksize (u_params[3].x)
#define u_useOutsideBox (u_params[3].y)

float GetPlaneIntersection(vec3 ray, vec3 avPlaneNormal, float afNegPlaneDist, float afFinalT)
{
    //Get T (amount of ray) to intersection
    float fMul  = dot(ray, avPlaneNormal);
    float fT = afNegPlaneDist / fMul;
    
    //Get the intersection and see if inside box
    vec3 vIntersection = abs(ray * fT + u_fogRayCastStart);
    if( all( lessThan(vIntersection, vec3(0.5001)) ) )
    {
        return max(afFinalT, fT);	
    }
    return afFinalT;
}

void main()
{

    vec2 ndc = gl_FragCoord.xy * u_viewTexel.xy;
    float fDepth = texture2D(s_positionMap, ndc).z;

    if(0.0 < u_useOutsideBox) {
        fDepth = fDepth +  v_position.z; //VertexPos is negative!
    
        float fFinalT = 0.0;
        if(0.0 < u_useBacksize) {
            fFinalT = GetPlaneIntersection(v_ray, vec3(-1.0, 0.0, 0.0),	u_fogNegPlaneDistNeg.x,	fFinalT);//Left
            fFinalT = GetPlaneIntersection(v_ray, vec3(1.0, 0.0, 0.0), 	u_fogNegPlaneDistPos.x,	fFinalT);//Right
            fFinalT = GetPlaneIntersection(v_ray, vec3(0.0, -1.0, 0.0),	u_fogNegPlaneDistNeg.y, fFinalT);//Bottom
            fFinalT = GetPlaneIntersection(v_ray, vec3(0.0, 1.0, 0.0 ),	u_fogNegPlaneDistPos.y, fFinalT);//Top
            fFinalT = GetPlaneIntersection(v_ray, vec3(0.0, 0.0, -1.0),	u_fogNegPlaneDistNeg.z, fFinalT);//Back
            fFinalT = GetPlaneIntersection(v_ray, vec3(0.0, 0.0, 1.0), 	u_fogNegPlaneDistPos.z,	fFinalT);//Front
            
            float fLocalBackZ = fFinalT * v_position.z -  v_position.z;
            fDepth = min(-fLocalBackZ, fDepth);
        }
        
    } else {
        if(0.0 < u_useBacksize) {
            fDepth = min(-v_position.z, fDepth);
        }
    }

    fDepth = min(- v_position.z, fDepth);
    float fAmount = max(fDepth / u_fogLength,0.0);
    
    gl_FragColor.xyz = u_fogColor.xyz;
    gl_FragColor.w = pow(fAmount, u_fogFalloffExp) * u_fogColor.w;
}