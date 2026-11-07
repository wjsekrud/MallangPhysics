// [Softbody] Softbody/Lit (Tessellated)의 MotionVectors·XRMotionVectors 패스. Softbody/Lit의 SoftbodyObjectMotionVectors.hlsl(URP 17.3.0
// `ShaderLibrary/ObjectMotionVectors.hlsl`의 사본 — Unity Companion License, com.unity.render-pipelines.universal copyright (c) 2020 Unity
// Technologies ApS)을 테셀레이션으로 고친 것. 그 파일이 #pragma vertex/fragment를 갖고 있어 include할 수 없어 따로 둔다.
// 바꾼 것(그 파일 대비):
//   ① 정점 셰이더는 제어점을 내고(지난 프레임 코너도 — SoftbodyTessSetPrevious), domain이 쪼갠 점마다 지금·지난 위치를 낸다.
//   ② 지금 위치 = 다른 모든 패스와 같은 쪼갠 점(깊이 비교에서 픽셀이 빠지지 않게). 지난 위치 = 지난 코너를 이은 점 + 지난 변위 텍스처·회전으로
//      구한 쪼갠 점 높이(SoftbodyPreviousDisplacementWS와 같은 짝, 노멀 방향은 지금 노멀).
//   ③ 인스턴싱 pragma를 뺐다(Softbody/Lit (Tessellated)는 인스턴싱 변종을 만들지 않는다 — 렌더러마다 머티리얼 사본이라 묶이지 않는다).
// 필요: 먼저 SOFTBODY_TESS_PREVIOUS 정의 + SoftbodyLitTessInput.hlsl.
#ifndef SOFTBODY_LIT_TESS_MOTION_VECTORS_INCLUDED
#define SOFTBODY_LIT_TESS_MOTION_VECTORS_INCLUDED

#pragma target 4.5
#pragma require tessellation tessHW

#pragma vertex SoftbodyTessMotionVectorsVertex
#pragma hull SoftbodyTessHull
#pragma domain SoftbodyTessMotionVectorsDomain
#pragma fragment SoftbodyTessMotionVectorsFragment

//-------------------------------------
// Other pragmas
#include_with_pragmas "Packages/com.unity.render-pipelines.core/ShaderLibrary/FoveatedRenderingKeywords.hlsl"

// -------------------------------------
// Includes
#include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
#include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/UnityInput.hlsl"

#if defined(LOD_FADE_CROSSFADE)
    #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/LODCrossFade.hlsl"
#endif

#include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/MotionVectorsCommon.hlsl"

// -------------------------------------
// Structs
struct SoftbodyTessMotionVectorsAttributes
{
    float4 position             : POSITION;
    float3 normalOS             : NORMAL;       // [Softbody] 쪼갠 점 높이 방향
    float2 uv                   : TEXCOORD0;
    float3 positionOld          : TEXCOORD4;
    uint vertexID               : SV_VertexID;  // [Softbody] 정점 표 칸
};

struct SoftbodyTessMotionVectorsVaryings
{
    float4 positionCS                 : SV_POSITION;
    float4 positionCSNoJitter         : POSITION_CS_NO_JITTER;
    float4 previousPositionCSNoJitter : PREV_POSITION_CS_NO_JITTER;
#if _ALPHATEST_ON
    float2 uv                         : TEXCOORD0;
#endif
    UNITY_VERTEX_OUTPUT_STEREO
};

// -------------------------------------
// Vertex: 제어점 + 지난 프레임 코너
SoftbodyTessControlPoint SoftbodyTessMotionVectorsVertex(SoftbodyTessMotionVectorsAttributes input)
{
    SoftbodyTessControlPoint o = SoftbodyTessMakeControlPoint(input.vertexID, input.position.xyz, input.normalOS, float4(1.0, 0.0, 0.0, 1.0),
        float4(input.uv, 0.0, 0.0), float2(0.0, 0.0));
    float4 prevPos = (unity_MotionVectorsParams.x == 1) ? float4(input.positionOld, 1) : input.position;
    SoftbodyTessSetPrevious(o, input.vertexID, mul(UNITY_PREV_MATRIX_M, prevPos).xyz, input.normalOS);
    return o;
}

// -------------------------------------
// Domain
[domain("tri")]
SoftbodyTessMotionVectorsVaryings SoftbodyTessMotionVectorsDomain(SoftbodyTessFactors factors, OutputPatch<SoftbodyTessControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    SoftbodyTessMotionVectorsVaryings output = (SoftbodyTessMotionVectorsVaryings)0;
    UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(output);

    SoftbodyTessPoint p = SOFTBODY_TESS_DOMAIN_POINT(patch, bary);
    float4 positionOS = float4(p.positionOS, 1.0);
    const VertexPositionInputs vertexInput = GetVertexPositionInputs(positionOS.xyz);

    #if defined(_ALPHATEST_ON)
        output.uv = TRANSFORM_TEX(p.texcoord.xy, _BaseMap);
    #endif

#if defined(APPLICATION_SPACE_WARP_MOTION)
    // We do not need jittered position in ASW
    output.positionCSNoJitter = mul(_NonJitteredViewProjMatrix, mul(UNITY_MATRIX_M, positionOS));
    output.positionCS = output.positionCSNoJitter;
#else
    // Jittered. Match the frame.
    output.positionCS = vertexInput.positionCS;
    output.positionCSNoJitter = mul(_NonJitteredViewProjMatrix, mul(UNITY_MATRIX_M, positionOS));
#endif

    output.previousPositionCSNoJitter = mul(_PrevViewProjMatrix, float4(p.previousPositionWS, 1.0));
    return output;
}

// -------------------------------------
// Fragment
float4 SoftbodyTessMotionVectorsFragment(SoftbodyTessMotionVectorsVaryings input) : SV_Target
{
    UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(input);

    #if defined(_ALPHATEST_ON)
        Alpha(SampleAlbedoAlpha(input.uv, TEXTURE2D_ARGS(_BaseMap, sampler_BaseMap)).a, _BaseColor, _Cutoff);
    #endif

    #if defined(LOD_FADE_CROSSFADE)
        LODFadeCrossFade(input.positionCS);
    #endif

    #if defined(APPLICATION_SPACE_WARP_MOTION)
        return float4(CalcAswNdcMotionVectorFromCsPositions(input.positionCSNoJitter, input.previousPositionCSNoJitter), 1);
    #else
        return float4(CalcNdcMotionVectorFromCsPositions(input.positionCSNoJitter, input.previousPositionCSNoJitter), 0, 0);
    #endif
}

#endif // SOFTBODY_LIT_TESS_MOTION_VECTORS_INCLUDED
