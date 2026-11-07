// Softbody/Lit (Tessellated) 편집기 패스 둘(SceneSelectionPass·ScenePickingPass). 조각 함수는 Softbody/Lit과 같다(SoftbodyLitEditorPass.hlsl).
// Scene 뷰 외곽선·클릭이 쪼갠 모양과 맞게 위치는 다른 패스와 같은 쪼갠 점이다.
// 필요: 먼저 SoftbodyLitTessInput.hlsl.
#ifndef SOFTBODY_LIT_TESS_EDITOR_PASS_INCLUDED
#define SOFTBODY_LIT_TESS_EDITOR_PASS_INCLUDED

#include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitEditorPass.hlsl"

struct SoftbodyTessEditorAttributes
{
    float4 positionOS : POSITION;
    float3 normalOS   : NORMAL;
    float2 texcoord   : TEXCOORD0;
    uint vertexID     : SV_VertexID;
};

SoftbodyTessControlPoint SoftbodyTessEditorVertex(SoftbodyTessEditorAttributes input)
{
    return SoftbodyTessMakeControlPoint(input.vertexID, input.positionOS.xyz, input.normalOS, float4(1.0, 0.0, 0.0, 1.0),
        float4(input.texcoord, 0.0, 0.0), float2(0.0, 0.0));
}

[domain("tri")]
SoftbodyEditorVaryings SoftbodyTessEditorDomain(SoftbodyTessFactors factors, OutputPatch<SoftbodyTessControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    SoftbodyTessPoint p = SOFTBODY_TESS_DOMAIN_POINT(patch, bary);
    SoftbodyEditorVaryings output = (SoftbodyEditorVaryings)0;
#if defined(_ALPHATEST_ON)
    output.uv = TRANSFORM_TEX(p.texcoord.xy, _BaseMap);
#endif
    output.positionCS = TransformObjectToHClip(p.positionOS);
    return output;
}

#endif // SOFTBODY_LIT_TESS_EDITOR_PASS_INCLUDED
