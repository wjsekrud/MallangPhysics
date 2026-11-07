// 개발: 저작 에셋 미리보기의 겉(대상 메시) — 반투명 회색, 가장자리일수록 진하게(UE AE.cpp 미리보기 껍질: 색 (0.7, 0.72, 0.76), 가운데 불투명도 0.22·윤곽 0.8).
// 편집기 전용. LightMode 없는 패스라 URP에서는 SRPDefaultUnlit으로, 내장 렌더러에서는 그대로 그려진다.
Shader "Hidden/Softbody/PreviewShell"
{
    Properties
    {
        _Color ("Color", Color) = (0.7, 0.72, 0.76, 1)
        _CenterAlpha ("Center Alpha", Range(0, 1)) = 0.22
        _EdgeAlpha ("Edge Alpha", Range(0, 1)) = 0.8
    }
    SubShader
    {
        Tags { "Queue" = "Transparent" "RenderType" = "Transparent" "IgnoreProjector" = "True" }
        Pass
        {
            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite Off
            Cull Back
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            float4 _Color;
            float _CenterAlpha;
            float _EdgeAlpha;

            struct appdata
            {
                float4 vertex : POSITION;
                float3 normal : NORMAL;
            };

            struct v2f
            {
                float4 pos : SV_POSITION;
                float3 normalWS : TEXCOORD0;
                float3 viewWS : TEXCOORD1;
            };

            v2f vert(appdata v)
            {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.normalWS = UnityObjectToWorldNormal(v.normal);
                o.viewWS = _WorldSpaceCameraPos - mul(unity_ObjectToWorld, v.vertex).xyz;
                return o;
            }

            float4 frag(v2f i) : SV_Target
            {
                float facing = abs(dot(normalize(i.normalWS), normalize(i.viewWS)));
                float rim = 1.0 - facing;
                float alpha = lerp(_CenterAlpha, _EdgeAlpha, rim * rim);
                return float4(_Color.rgb, alpha);
            }
            ENDCG
        }
    }
}
