// [Softbody] URP 17.3.0 `Packages/com.unity.render-pipelines.universal/ShaderLibrary/ObjectMotionVectors.hlsl`의 사본(Unity Companion License,
// com.unity.render-pipelines.universal copyright (c) 2020 Unity Technologies ApS). 바꾼 곳은 모두 `[Softbody]` 주석이 붙어 있다.
// 사본이 필요한 까닭: 원본이 파일 안에 #pragma vertex/fragment·target·인스턴싱 pragma를 직접 갖고 있어 감쌀 수 없다.
// 바꾼 것:
//   ① 지금 위치 = 다른 모든 패스와 같은 SoftbodyDisplacedPositionOS(깊이 비교에서 픽셀이 빠지지 않게).
//   ② 지난 위치 = 지난 프레임 이동 전 월드 위치 + SoftbodyPreviousDisplacementWS(지난 브릿지·지난 변위 텍스처·지난 회전·지난 원점, UE 지난 프레임 커널).
//      그래서 정지한 메시가 흔들리거나 눌릴 때도 속도가 잡혀 TAA·STP·모션 블러에 잔상이 남지 않는다.
//   ③ target 4.5(SV_VertexID·텍스처 Load), DOTS 인스턴싱 pragma 뺌(GPU Resident Drawer가 가져가지 않게),
//      Alembic 미리 구운 속도(_ADD_PRECOMPUTED_VELOCITY)는 받지 않는다(프로퍼티 이름만 URP Lit과 맞춰 둔다).
// 필요: 먼저 SoftbodyLitInput.hlsl(프로퍼티와 소프트바디 함수).
#ifndef SOFTBODY_OBJECT_MOTION_VECTORS_INCLUDED  // [Softbody] 가드 이름
#define SOFTBODY_OBJECT_MOTION_VECTORS_INCLUDED

#pragma target 4.5  // [Softbody] 원본 3.5

#pragma vertex vert
#pragma fragment frag

//--------------------------------------
// GPU Instancing
#pragma multi_compile_instancing
// [Softbody] DOTS.hlsl(DOTS_INSTANCING_ON) include는 뺐다.

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
struct Attributes
{
    float4 position             : POSITION;
#if _ALPHATEST_ON
    float2 uv                   : TEXCOORD0;
#endif
    float3 positionOld          : TEXCOORD4;
    uint vertexID               : SV_VertexID;  // [Softbody] 정점 표 칸
    UNITY_VERTEX_INPUT_INSTANCE_ID
};

struct Varyings
{
    float4 positionCS                 : SV_POSITION;
    float4 positionCSNoJitter         : POSITION_CS_NO_JITTER;
    float4 previousPositionCSNoJitter : PREV_POSITION_CS_NO_JITTER;
#if _ALPHATEST_ON
    float2 uv                         : TEXCOORD0;
#endif
    UNITY_VERTEX_INPUT_INSTANCE_ID
    UNITY_VERTEX_OUTPUT_STEREO
};

// -------------------------------------
// Vertex
Varyings vert(Attributes input)
{
    Varyings output = (Varyings)0;

    UNITY_SETUP_INSTANCE_ID(input);
    UNITY_TRANSFER_INSTANCE_ID(input, output);
    UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(output);

    // [Softbody] 다른 패스와 같은 함수로 옮긴 지금 위치
    float4 positionOS = float4(SoftbodyDisplacedPositionOS(input.vertexID, input.position.xyz), 1.0);
    const VertexPositionInputs vertexInput = GetVertexPositionInputs(positionOS.xyz);

    #if defined(_ALPHATEST_ON)
        output.uv = TRANSFORM_TEX(input.uv, _BaseMap);
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

    float4 prevPos = (unity_MotionVectorsParams.x == 1) ? float4(input.positionOld, 1) : input.position;

    // [Softbody] 지난 프레임 이동 전 월드 위치에 지난 프레임 소프트바디 이동을 더한다(같은 커널, 지난 입력).
    float3 previousPositionWS = mul(UNITY_PREV_MATRIX_M, prevPos).xyz;
    previousPositionWS += SoftbodyPreviousDisplacementWS(input.vertexID, previousPositionWS);
    output.previousPositionCSNoJitter = mul(_PrevViewProjMatrix, float4(previousPositionWS, 1.0));

    return output;
}

// -------------------------------------
// Fragment
float4 frag(Varyings input) : SV_Target
{
    UNITY_SETUP_INSTANCE_ID(input);
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


#endif // SOFTBODY_OBJECT_MOTION_VECTORS_INCLUDED
