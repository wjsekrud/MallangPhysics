// 개발: 저작 에셋 미리보기의 단단한 속(구) — 빛 받는 단색(UE AE.cpp 미리보기 속: 주황 (1, 0.42, 0.08)). 빛은 카메라 쪽 위에서 비춘다(장면 빛과 무관).
// 구 메시의 감김을 따지지 않게 양면(닫힌 구라 뒷면은 앞면에 가린다). 편집기 전용. LightMode 없는 패스라 URP에서는 SRPDefaultUnlit으로, 내장 렌더러에서는 그대로 그려진다.
Shader "Hidden/Softbody/PreviewSolid"
{
    Properties
    {
        _Color ("Color", Color) = (1, 0.42, 0.08, 1)
    }
    SubShader
    {
        Tags { "Queue" = "Geometry" "RenderType" = "Opaque" }
        Pass
        {
            ZWrite On
            Cull Off
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            float4 _Color;

            struct appdata
            {
                float4 vertex : POSITION;
                float3 normal : NORMAL;
            };

            struct v2f
            {
                float4 pos : SV_POSITION;
                float3 normalVS : TEXCOORD0;
            };

            v2f vert(appdata v)
            {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.normalVS = mul((float3x3)UNITY_MATRIX_V, UnityObjectToWorldNormal(v.normal));
                return o;
            }

            float4 frag(v2f i) : SV_Target
            {
                // 뷰 공간 빛: 카메라 왼쪽 위 앞
                float3 light = normalize(float3(-0.4, 0.6, 0.7));
                float lit = 0.35 + 0.65 * saturate(dot(normalize(i.normalVS), light));
                return float4(_Color.rgb * lit, 1.0);
            }
            ENDCG
        }
    }
}
