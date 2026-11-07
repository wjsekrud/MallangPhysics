// Softbody/Lit (Tessellated) DepthNormals 패스. 픽셀 쪽은 Softbody/Lit과 같다(SoftbodyLitDepthNormalsPass.hlsl). domain이 URP DepthNormalsVertex를 부른다.
// 필요: 먼저 SOFTBODY_TESS_PIXEL_SHADING 정의(관성 기울기를 넘긴다) + SoftbodyLitTessInput.hlsl.
#ifndef SOFTBODY_LIT_TESS_DEPTH_NORMALS_PASS_INCLUDED
#define SOFTBODY_LIT_TESS_DEPTH_NORMALS_PASS_INCLUDED

#include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitDepthNormalsPass.hlsl"

SoftbodyTessControlPoint SoftbodyTessDepthNormalsVertex(SoftbodyDepthNormalsAttributes input)
{
    Attributes attributes = input.base;
    UNITY_SETUP_INSTANCE_ID(attributes);
    return SoftbodyTessMakeControlPoint(input.vertexID, attributes.positionOS.xyz, attributes.normal, attributes.tangentOS,
        float4(attributes.texcoord, 0.0, 0.0), float2(0.0, 0.0));
}

[domain("tri")]
SoftbodyDepthNormalsVaryings SoftbodyTessDepthNormalsDomain(SoftbodyTessFactors factors, OutputPatch<SoftbodyTessControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    SoftbodyTessPoint p = SOFTBODY_TESS_DOMAIN_POINT(patch, bary);
    Attributes attributes = (Attributes)0;
    attributes.positionOS = float4(p.positionOS, 1.0);
    attributes.normal = p.normalOS;
    attributes.tangentOS = p.tangentOS;
    attributes.texcoord = p.texcoord.xy;

    SoftbodyDepthNormalsVaryings output = (SoftbodyDepthNormalsVaryings)0;
    output.base = DepthNormalsVertex(attributes);
    SoftbodyVertexShadingData softbody = SoftbodyTessShadingData(p);
    SOFTBODY_TRANSFER_VARYINGS_NORMAL(softbody, output)
    return output;
}

#endif // SOFTBODY_LIT_TESS_DEPTH_NORMALS_PASS_INCLUDED
