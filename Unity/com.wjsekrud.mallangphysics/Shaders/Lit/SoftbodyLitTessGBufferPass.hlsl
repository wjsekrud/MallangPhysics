// Softbody/Lit (Tessellated) GBuffer 패스. 픽셀 쪽은 Softbody/Lit과 같다(SoftbodyLitGBufferPass.hlsl). domain이 URP LitGBufferPassVertex를 부른다.
// 필요: 먼저 SOFTBODY_TESS_PIXEL_SHADING 정의 + SoftbodyLitTessInput.hlsl.
#ifndef SOFTBODY_LIT_TESS_GBUFFER_PASS_INCLUDED
#define SOFTBODY_LIT_TESS_GBUFFER_PASS_INCLUDED

#include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitGBufferPass.hlsl"

SoftbodyTessControlPoint SoftbodyLitTessGBufferVertex(SoftbodyLitGBufferAttributes input)
{
    Attributes attributes = input.base;
    UNITY_SETUP_INSTANCE_ID(attributes);
    return SoftbodyTessMakeControlPoint(input.vertexID, attributes.positionOS.xyz, attributes.normalOS, attributes.tangentOS,
        float4(attributes.texcoord, attributes.staticLightmapUV), attributes.dynamicLightmapUV);
}

[domain("tri")]
SoftbodyLitGBufferVaryings SoftbodyLitTessGBufferDomain(SoftbodyTessFactors factors, OutputPatch<SoftbodyTessControlPoint, 3> patch,
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

    SoftbodyLitGBufferVaryings output = (SoftbodyLitGBufferVaryings)0;
    output.base = LitGBufferPassVertex(attributes);
    SoftbodyVertexShadingData softbody = SoftbodyTessShadingData(p);
    SOFTBODY_TRANSFER_VARYINGS(softbody, output)
    return output;
}

#endif // SOFTBODY_LIT_TESS_GBUFFER_PASS_INCLUDED
