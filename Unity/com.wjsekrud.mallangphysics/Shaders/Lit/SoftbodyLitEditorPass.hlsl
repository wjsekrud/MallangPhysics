// Softbody/Lit 편집기 패스 둘: SceneSelectionPass(Scene 뷰 선택 외곽선)와 ScenePickingPass(Scene 뷰 클릭 고르기).
// 이 패스가 없으면 편집기가 대신 그리는 셰이더가 옮기기 전 메시를 그려 Play 중 Scene 뷰의 외곽선·클릭이 눌리고 흔들린 살과 어긋난다.
// 위치는 다른 패스와 같은 SoftbodyDisplacedPositionOS로 옮긴다. 출력 값은 URP 손 셰이더(Particles)·Shader Graph와 같다.
// 출처: URP 17.3.0 `Packages/com.unity.render-pipelines.universal/Shaders/Particles/ParticlesEditorPass.hlsl`의 선언(_ObjectId·_PassValue·_SelectionID)과
// 함수 꼴(정점 → 잘라내기 → 두 조각 함수)을 따라 짰다(Unity Companion License, com.unity.render-pipelines.universal copyright © 2020
// Unity Technologies ApS — Third Party Notices). 고친 것: 입자 입력 대신 Lit 입력(_BaseMap 알파 잘라내기, LOD 크로스페이드)과 소프트바디 위치.
// 필요: 먼저 SoftbodyLitInput.hlsl.
#ifndef SOFTBODY_LIT_EDITOR_PASS_INCLUDED
#define SOFTBODY_LIT_EDITOR_PASS_INCLUDED

#if defined(LOD_FADE_CROSSFADE)
    #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/LODCrossFade.hlsl"
#endif

// 편집기가 그리기 직전에 넣는 값(URP Shaders/Particles/ParticlesEditorPass.hlsl과 같은 선언).
float _ObjectId;
float _PassValue;
float4 _SelectionID;

struct SoftbodyEditorAttributes
{
    float4 positionOS : POSITION;
    float2 texcoord   : TEXCOORD0;
    uint vertexID     : SV_VertexID;
    UNITY_VERTEX_INPUT_INSTANCE_ID
};

struct SoftbodyEditorVaryings
{
    float4 positionCS : SV_POSITION;
#if defined(_ALPHATEST_ON)
    float2 uv         : TEXCOORD0;
#endif
    UNITY_VERTEX_INPUT_INSTANCE_ID
    UNITY_VERTEX_OUTPUT_STEREO
};

SoftbodyEditorVaryings SoftbodyEditorVertex(SoftbodyEditorAttributes input)
{
    SoftbodyEditorVaryings output = (SoftbodyEditorVaryings)0;
    UNITY_SETUP_INSTANCE_ID(input);
    UNITY_TRANSFER_INSTANCE_ID(input, output);
    UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(output);

#if defined(_ALPHATEST_ON)
    output.uv = TRANSFORM_TEX(input.texcoord, _BaseMap);
#endif
    output.positionCS = TransformObjectToHClip(SoftbodyDisplacedPositionOS(input.vertexID, input.positionOS.xyz));
    return output;
}

void SoftbodyEditorClip(SoftbodyEditorVaryings input)
{
    UNITY_SETUP_INSTANCE_ID(input);
    UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(input);
#if defined(_ALPHATEST_ON)
    Alpha(SampleAlbedoAlpha(input.uv, TEXTURE2D_ARGS(_BaseMap, sampler_BaseMap)).a, _BaseColor, _Cutoff);
#endif
#if defined(LOD_FADE_CROSSFADE)
    LODFadeCrossFade(input.positionCS);
#endif
}

half4 SoftbodySceneSelectionFragment(SoftbodyEditorVaryings input) : SV_Target
{
    SoftbodyEditorClip(input);
    return half4(_ObjectId, _PassValue, 1.0, 1.0);
}

half4 SoftbodyScenePickingFragment(SoftbodyEditorVaryings input) : SV_Target
{
    SoftbodyEditorClip(input);
    return _SelectionID;
}

#endif // SOFTBODY_LIT_EDITOR_PASS_INCLUDED
