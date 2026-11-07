// Softbody/Lit DepthNormals 패스. URP 17.3 LitDepthNormalsPass.hlsl을 고치지 않고 감싼다: 위치를 옮기고, 픽셀 노멀·탄젠트를 ForwardLit과 같은
// 소프트바디 회전으로 돌린다 — SSAO·데칼·화면 공간 효과가 눌리고 흔들린 면의 노멀을 본다(색은 쓰지 않으므로 보간기는 셋).
// 필요: 먼저 SoftbodyLitInput.hlsl.
#ifndef SOFTBODY_LIT_DEPTH_NORMALS_PASS_INCLUDED
#define SOFTBODY_LIT_DEPTH_NORMALS_PASS_INCLUDED

#include "Packages/com.unity.render-pipelines.universal/Shaders/LitDepthNormalsPass.hlsl"

struct SoftbodyDepthNormalsAttributes
{
    Attributes base;
    uint vertexID : SV_VertexID;
};

struct SoftbodyDepthNormalsVaryings
{
    Varyings base;
    SOFTBODY_VARYINGS_NORMAL(11, 12, 13)   // URP DepthNormals는 TEXCOORD1·2·4·5·8을 쓴다
};

SoftbodyDepthNormalsVaryings SoftbodyDepthNormalsVertex(SoftbodyDepthNormalsAttributes input)
{
    SoftbodyDepthNormalsVaryings output = (SoftbodyDepthNormalsVaryings)0;
    Attributes attributes = input.base;
    UNITY_SETUP_INSTANCE_ID(attributes);

    SoftbodyVertexShadingData softbody = SoftbodyVertexShadingOS(input.vertexID, attributes.positionOS.xyz, attributes.normal);
    attributes.positionOS.xyz = SoftbodyDisplacedPositionOS(input.vertexID, attributes.positionOS.xyz);

    output.base = DepthNormalsVertex(attributes);
    SOFTBODY_TRANSFER_VARYINGS_NORMAL(softbody, output)
    return output;
}

void SoftbodyDepthNormalsFragment(
    SoftbodyDepthNormalsVaryings input
    , out half4 outNormalWS : SV_Target0
#ifdef _WRITE_RENDERING_LAYERS
    , out uint outRenderingLayers : SV_Target1
#endif
)
{
    SoftbodyVertexShadingData softbody = SOFTBODY_LOAD_VARYINGS_NORMAL(input);
    float4 surfacePixel = SoftbodySurfacePixel(softbody, input.base.normalWS);
    input.base.normalWS = SoftbodyRotateByShading(input.base.normalWS, surfacePixel, softbody.inertiaTilt);
#if defined(REQUIRES_WORLD_SPACE_TANGENT_INTERPOLATOR)
    input.base.tangentWS.xyz = SoftbodyRotateByShading(input.base.tangentWS.xyz, surfacePixel, softbody.inertiaTilt);
#endif

    DepthNormalsFragment(input.base, outNormalWS
#ifdef _WRITE_RENDERING_LAYERS
        , outRenderingLayers
#endif
    );
}

#endif // SOFTBODY_LIT_DEPTH_NORMALS_PASS_INCLUDED
