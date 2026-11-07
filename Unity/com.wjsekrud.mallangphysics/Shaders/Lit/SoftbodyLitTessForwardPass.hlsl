// Softbody/Lit (Tessellated) ForwardLit 패스. 픽셀 쪽은 Softbody/Lit과 같다(SoftbodyLitForwardPass.hlsl의 SoftbodyLitPassFragment·보간기 꼴을
// 그대로 쓴다). 정점 셰이더는 제어점을, domain은 쪼갠 점마다 URP LitPassVertex를 부른다.
// 필요: 먼저 SOFTBODY_TESS_PIXEL_SHADING 정의 + SoftbodyLitTessInput.hlsl.
#ifndef SOFTBODY_LIT_TESS_FORWARD_PASS_INCLUDED
#define SOFTBODY_LIT_TESS_FORWARD_PASS_INCLUDED

#include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitForwardPass.hlsl"

SoftbodyTessControlPoint SoftbodyLitTessVertex(SoftbodyLitAttributes input)
{
    Attributes attributes = input.base;
    UNITY_SETUP_INSTANCE_ID(attributes);
    return SoftbodyTessMakeControlPoint(input.vertexID, attributes.positionOS.xyz, attributes.normalOS, attributes.tangentOS,
        float4(attributes.texcoord, attributes.staticLightmapUV), attributes.dynamicLightmapUV);
}

[domain("tri")]
SoftbodyLitVaryings SoftbodyLitTessDomain(SoftbodyTessFactors factors, OutputPatch<SoftbodyTessControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    SoftbodyTessPoint p = SOFTBODY_TESS_DOMAIN_POINT(patch, bary);
    Attributes attributes = (Attributes)0;
    attributes.positionOS = float4(p.positionOS, 1.0);
    attributes.normalOS = p.normalOS;
    attributes.tangentOS = p.tangentOS;
    attributes.texcoord = p.texcoord.xy;
    attributes.staticLightmapUV = p.texcoord.zw;
    attributes.dynamicLightmapUV = p.dynamicLightmapUV;

    SoftbodyLitVaryings output = (SoftbodyLitVaryings)0;
    output.base = LitPassVertex(attributes);
    SoftbodyVertexShadingData softbody = SoftbodyTessShadingData(p);
    SOFTBODY_TRANSFER_VARYINGS(softbody, output)
    return output;
}

#endif // SOFTBODY_LIT_TESS_FORWARD_PASS_INCLUDED
