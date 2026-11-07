// Shader Graph Custom Function(File 모드)용 소프트바디 함수. 사용법은 같은 폴더의 README.md("Shader Graph").
// 프로퍼티는 이름으로 읽는다: 그래프 블랙보드에 SoftbodyShaderIds 이름 그대로(_SoftbodyContactData …) 프로퍼티를 두고 Exposed로 켜야 한다
// (Shader Graph가 블랙보드 프로퍼티를 이 파일보다 먼저 선언하므로 함수가 그 이름을 그대로 쓴다). 하나라도 없으면 "undeclared identifier"로 멈춘다.
// 계산은 SoftbodyMaterial.hlsl(손 셰이더·Softbody/Lit과 같은 함수)이 한다 — 이 파일은 노드 입출력 꼴만 맞춘다.
// 노드 이름 = 아래 함수 이름에서 _float/_half를 뺀 것(SoftbodyVertex, SoftbodyMotionVector, SoftbodyPixel, SoftbodyBaseColor).
// 정밀도: SoftbodyVertex·SoftbodyMotionVector·SoftbodyPixel은 _float(Single)만 둔다. Half 그래프(또는 노드 Precision = Half)에서는 Shader Graph가
// _half를 찾다가 "undeclared identifier 'SoftbodyVertex_half'"로 멈춘다 — 일부러다. half 입력이면 노드에 들어오기 전에 값이 잘린다:
// 정점 번호는 2048을 넘으면 엉뚱한 칸을 읽고(fp16 정수 한계), 위치·캐리어 UV는 반정밀로 계단이 진다(모바일·Unified 정밀 모델에서 half = fp16).
// 그래서 README는 그래프 Precision(또는 이 노드들의 Precision)을 Single로 두라고 한다. 색만 다루는 SoftbodyBaseColor는 _half도 둔다.
#ifndef SOFTBODY_SHADER_GRAPH_INCLUDED
#define SOFTBODY_SHADER_GRAPH_INCLUDED

#include "Packages/com.wjsekrud.mallangphysics/Shaders/Include/SoftbodyMaterial.hlsl"

// 정점 단계. VertexID = Vertex ID 노드, PositionOS·NormalOS = Object 공간 Position·Normal Vector 노드.
// PositionOSOut → 마스터 스택 Vertex Position(Object). Carrier·Shading·InertiaTilt·VisColor → Custom Interpolator 넷(정점 블록) → 조각 단계로.
// Vertex ID는 float로 온다 — 정점 2^24(약 1,677만) 개까지 정확하다.
void SoftbodyVertex_float(float VertexID, float3 PositionOS, float3 NormalOS,
    out float3 PositionOSOut, out float4 Carrier, out float4 Shading, out float3 InertiaTilt, out float3 VisColor)
{
    uint vertexID = (uint)(VertexID + 0.5);
    SoftbodyVertexShadingData data = SoftbodyVertexShading(vertexID, TransformObjectToWorld(PositionOS), TransformObjectToWorldNormal(NormalOS, true));
    PositionOSOut = SoftbodyDisplacedPositionOS(vertexID, PositionOS);
    Carrier = data.carrier;
    Shading = data.shading;
    InertiaTilt = data.inertiaTilt;
    VisColor = data.visColor;
}

// 정점 단계, 그래프 설정 Additional Motion Vectors = Custom일 때 마스터 스택 Motion Vector(Object 공간, "지난 프레임에서 지금까지 옮겨 간 양")에 잇는다.
// = 지금 이동(오브젝트 공간) − 지난 이동(지난 프레임 오브젝트 공간). Shader Graph 모션 벡터 패스가 지난 위치를 다시 셈해 이 값을 빼므로
// 결과가 "지난 위치 + 지난 이동"이 된다. 지난 이동의 원점 상대 위치는 지금 위치 − 지금 원점으로 어림한다(03 §10.4 — 그래프 정점 문맥에 지난
// 위치가 없다). 렌더러가 통째로 움직여도(달리는 캐릭터) 위치와 원점이 같이 움직이므로 이 어림에는 그 이동이 섞이지 않고, 본이 빠르게 돌 때
// 회전 몫(관성 구동점 둘레의 팔)만 조금 어긋난다. 지난 원점(_SoftbodyPrevOriginWS)과 지금 위치를 짝지으면 한 프레임 이동량이 팔에 통째로 더해진다.
// Softbody/Lit(손 셰이더)은 어림 없이 지난 위치·지난 원점을 쓴다(UE PreviousFrameSwitch와 같다).
void SoftbodyMotionVector_float(float VertexID, float3 PositionOS, out float3 MotionVectorOS)
{
    uint vertexID = (uint)(VertexID + 0.5);
    float3 positionWS = TransformObjectToWorld(PositionOS);
    float2 texUV = SoftbodyTexelUV(vertexID);
    float3 relPosCm = SoftbodyRelPosCm(positionWS, _SoftbodyOriginWS.xyz);
    float3 currentWS = SOFTBODY_UNIT_PER_CM * SoftbodyKernelCm(texUV, relPosCm,
        _SoftbodyContactData, _SoftbodySurfaceDispTex, _SoftbodyL2WX.xyz, _SoftbodyL2WY.xyz, _SoftbodyL2WZ.xyz);
    float3 previousWS = SOFTBODY_UNIT_PER_CM * SoftbodyKernelCm(texUV, relPosCm,
        _SoftbodyPrevContactData, _SoftbodyPrevSurfaceDispTex, _SoftbodyPrevL2WX.xyz, _SoftbodyPrevL2WY.xyz, _SoftbodyPrevL2WZ.xyz);
    float3 currentOS = TransformWorldToObjectDir(currentWS, false);
    float3 previousOS = mul((float3x3)UNITY_PREV_MATRIX_I_M, previousWS);
    MotionVectorOS = currentOS - previousOS;
}

// 조각 단계. Carrier·Shading·InertiaTilt = 정점에서 넘긴 Custom Interpolator, VertexNormalWS = World 공간 Normal Vector 노드(보간한 정점 노멀),
// BaseNormalWS = 노멀맵을 월드로 바꾼 노멀(노멀맵이 없으면 VertexNormalWS). NormalWS → 마스터 스택 Normal(그래프 설정 Fragment Normal Space = World).
// Occlusion = 1 − 함몰 어둡기 — Base Color에 곱한다(SoftbodyBaseColor).
void SoftbodyPixel_float(float4 Carrier, float4 Shading, float3 InertiaTilt, float3 VertexNormalWS, float3 BaseNormalWS,
    out float3 NormalWS, out float Occlusion)
{
    SoftbodyVertexShadingData data;
    data.carrier = Carrier;
    data.shading = Shading;
    data.inertiaTilt = InertiaTilt;
    data.visColor = float3(0.0, 0.0, 0.0);
    NormalWS = SoftbodyPixelNormalWS(data, VertexNormalWS, BaseNormalWS, Occlusion);
}

// 조각 단계. BaseColorOut = lerp(BaseColor × Occlusion, VisColor, 디버그 보기 무게) → 마스터 스택 Base Color.
void SoftbodyBaseColor_float(float3 BaseColor, float Occlusion, float3 VisColor, out float3 BaseColorOut)
{
    BaseColorOut = SoftbodyBaseColor(BaseColor, Occlusion, VisColor);
}

// 그래프 정밀도가 Half일 때 Shader Graph는 _half 함수를 부른다. 색은 반정밀로 충분하므로 이 노드만 _half를 둔다(위 "정밀도").
void SoftbodyBaseColor_half(half3 BaseColor, half Occlusion, half3 VisColor, out half3 BaseColorOut)
{
    float3 color;
    SoftbodyBaseColor_float(BaseColor, Occlusion, VisColor, color);
    BaseColorOut = color;
}

#endif // SOFTBODY_SHADER_GRAPH_INCLUDED
