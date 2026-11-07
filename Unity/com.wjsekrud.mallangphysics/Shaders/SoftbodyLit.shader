// Softbody/Lit — URP Lit에 소프트바디(접촉으로 눌리고 솟고 끌리는 살 + 관성 출렁임)를 더한 셰이더(UE 예제 호스트 머티리얼 M_SoftbodyHost 자리).
// 화면에서 하는 일:
//   - 정점 위치를 표면 장 변위와 관성 흔들림만큼 옮긴다(모든 위치 패스가 같은 함수로 — 그림자·깊이·모션 벡터가 맞물린다).
//   - 픽셀 노멀을 눌린 면·흔들린 면을 따라 돌린다(노멀맵은 그대로 얹힌다), 눌린 자리를 기본 색에서 어둡게 한다.
//   - 디버그 보기(Debug View) 0~5는 기본 색을 굽기 검수 색으로 바꾼다.
// 프로퍼티는 URP Lit과 같은 이름 전부(URP Lit 머티리얼의 셰이더만 바꾸면 값이 옮겨진다) + "_Softbody" 이름(런타임 SoftbodyShaderIds, D-30).
// 소프트바디 값은 Softbody 컴포넌트가 렌더러마다 머티리얼 사본에 넣는다 — 편집 중(세팅 전)에는 스위치가 0이라 원래 메시 그대로다.
// 키워드는 URP Lit 것뿐이다(소프트바디는 모두 값으로 가른다). DOTS 인스턴싱 변종은 만들지 않는다 — GPU Resident Drawer가 이 머티리얼을
// 가져가지 않게(정점 번호가 바뀌면 정점 표가 어긋난다, D-17).
// 패스(03 §10.3): ForwardLit·GBuffer·ShadowCaster·DepthOnly·DepthNormals·MotionVectors·XRMotionVectors·SceneSelectionPass·ScenePickingPass는
// 위치를 옮기고, Meta(라이트맵 굽기 전용, 라이트맵 UV 공간에 그린다)는 옮기지 않는다. Universal2D는 뺐다(2D 렌더러는 대상 밖).
// 정점 사이 함몰을 쪼갠 점으로 그리는 테셀레이션 변종은 따로 둔 셰이더 Softbody/Lit (Tessellated)다(SoftbodyLitTessellated.shader — 프로퍼티 셋이 늘어
// UnityPerMaterial 배치가 다르다). 이 셰이더는 테셀레이션이 안 되는 장치에서 그 변종의 FallBack이다.
// 출처: Properties 블록·패스마다의 태그·렌더 상태·pragma는 URP 17.3.0 `Packages/com.unity.render-pipelines.universal/Shaders/Lit.shader`를 옮긴 것이다
// (Unity Companion License, com.unity.render-pipelines.universal copyright © 2020 Unity Technologies ApS — Third Party Notices).
// 고친 것: Softbody 프로퍼티·패스 파일, DOTS 인스턴싱·Universal2D 뺌, target 4.5, AlwaysRenderMotionVectors 태그, CustomEditor.
Shader "Softbody/Lit"
{
    Properties
    {
        // ---- URP Lit과 같은 프로퍼티(URP 17.3 Lit.shader 그대로) ----
        // Specular vs Metallic workflow
        _WorkflowMode("WorkflowMode", Float) = 1.0

        [MainTexture] _BaseMap("Albedo", 2D) = "white" {}
        [MainColor] _BaseColor("Color", Color) = (1,1,1,1)

        _Cutoff("Alpha Cutoff", Range(0.0, 1.0)) = 0.5

        _Smoothness("Smoothness", Range(0.0, 1.0)) = 0.5
        _SmoothnessTextureChannel("Smoothness texture channel", Float) = 0

        _Metallic("Metallic", Range(0.0, 1.0)) = 0.0
        _MetallicGlossMap("Metallic", 2D) = "white" {}

        _SpecColor("Specular", Color) = (0.2, 0.2, 0.2)
        _SpecGlossMap("Specular", 2D) = "white" {}

        [ToggleOff] _SpecularHighlights("Specular Highlights", Float) = 1.0
        [ToggleOff] _EnvironmentReflections("Environment Reflections", Float) = 1.0

        _BumpScale("Scale", Float) = 1.0
        _BumpMap("Normal Map", 2D) = "bump" {}

        _Parallax("Scale", Range(0.005, 0.08)) = 0.005
        _ParallaxMap("Height Map", 2D) = "black" {}

        _OcclusionStrength("Strength", Range(0.0, 1.0)) = 1.0
        _OcclusionMap("Occlusion", 2D) = "white" {}

        [HDR] _EmissionColor("Color", Color) = (0,0,0)
        _EmissionMap("Emission", 2D) = "white" {}

        _DetailMask("Detail Mask", 2D) = "white" {}
        _DetailAlbedoMapScale("Scale", Range(0.0, 2.0)) = 1.0
        _DetailAlbedoMap("Detail Albedo x2", 2D) = "linearGrey" {}
        _DetailNormalMapScale("Scale", Range(0.0, 2.0)) = 1.0
        [Normal] _DetailNormalMap("Normal Map", 2D) = "bump" {}

        // SRP batching compatibility for Clear Coat (Not used in Lit)
        [HideInInspector] _ClearCoatMask("_ClearCoatMask", Float) = 0.0
        [HideInInspector] _ClearCoatSmoothness("_ClearCoatSmoothness", Float) = 0.0

        // Blending state
        _Surface("__surface", Float) = 0.0
        _Blend("__blend", Float) = 0.0
        _Cull("__cull", Float) = 2.0
        [ToggleUI] _AlphaClip("__clip", Float) = 0.0
        [HideInInspector] _SrcBlend("__src", Float) = 1.0
        [HideInInspector] _DstBlend("__dst", Float) = 0.0
        [HideInInspector] _SrcBlendAlpha("__srcA", Float) = 1.0
        [HideInInspector] _DstBlendAlpha("__dstA", Float) = 0.0
        [HideInInspector] _ZWrite("__zw", Float) = 1.0
        [HideInInspector] _BlendModePreserveSpecular("_BlendModePreserveSpecular", Float) = 1.0
        [HideInInspector] _AlphaToMask("__alphaToMask", Float) = 0.0
        // 개발: 이름만 URP Lit과 맞춘다(Alembic 미리 구운 속도는 받지 않는다 — 인스펙터에서도 숨긴다).
        [HideInInspector] _AddPrecomputedVelocity("_AddPrecomputedVelocity", Float) = 0.0
        [HideInInspector] _XRMotionVectorsPass("_XRMotionVectorsPass", Float) = 1.0

        [ToggleUI] _ReceiveShadows("Receive Shadows", Float) = 1.0
        // Editmode props
        _QueueOffset("Queue offset", Float) = 0.0

        // ObsoleteProperties
        [HideInInspector] _MainTex("BaseMap", 2D) = "white" {}
        [HideInInspector] _Color("Base Color", Color) = (1, 1, 1, 1)
        [HideInInspector] _GlossMapScale("Smoothness", Float) = 0.0
        [HideInInspector] _Glossiness("Smoothness", Float) = 0.0
        [HideInInspector] _GlossyReflections("EnvironmentReflections", Float) = 0.0

        [HideInInspector][NoScaleOffset]unity_Lightmaps("unity_Lightmaps", 2DArray) = "" {}
        [HideInInspector][NoScaleOffset]unity_LightmapsInd("unity_LightmapsInd", 2DArray) = "" {}
        [HideInInspector][NoScaleOffset]unity_ShadowMasks("unity_ShadowMasks", 2DArray) = "" {}

        // ---- 소프트바디(이름 = SoftbodyShaderIds.Names) ----
        // 머티리얼 값(컴포넌트가 넣지 않는다). 인스펙터 Softbody 칸에 보인다.
        _SoftbodySkirtStrength("Contact Strength", Float) = 1.0
        [HideInInspector] _SoftbodySurfaceBetweenWeight("Contact Shading Inside Triangles", Float) = 1.0
        [HideInInspector] _SoftbodySurfaceBetweenEdgeTexels("Contact Shading Edge Texels", Float) = 2.0

        // 컴포넌트가 넣는 텍스처. 판정(Material.HasTexture)이 이 선언을 본다 — 지우지 말 것. 비우면 아래 기본 텍스처로 읽힌다.
        [HideInInspector][NoScaleOffset] _SoftbodyContactData("Softbody Inertia Bridge", 2D) = "black" {}
        [HideInInspector][NoScaleOffset] _SoftbodyPrevContactData("Softbody Previous Inertia Bridge", 2D) = "black" {}
        [HideInInspector][NoScaleOffset] _SoftbodyAttr2Tex("Softbody Inertia Groups", 2D) = "black" {}
        [HideInInspector][NoScaleOffset] _SoftbodyAttrDbgTex("Softbody Debug Values", 2D) = "black" {}
        [HideInInspector][NoScaleOffset] _SoftbodyPaintTex("Softbody Paint", 2D) = "white" {}
        [HideInInspector][NoScaleOffset] _SoftbodyInertiaResponseTex("Softbody Flesh Softness", 2D) = "black" {}
        [HideInInspector][NoScaleOffset] _SoftbodyCarrierUVTex("Softbody Contact Map Points", 2D) = "black" {}
        [HideInInspector][NoScaleOffset] _SoftbodyCarrierVertexUVTex("Softbody Contact Map UVs", 2D) = "black" {}
        [HideInInspector][NoScaleOffset] _SoftbodySurfaceDispTex("Softbody Contact Displacement", 2D) = "black" {}
        [HideInInspector][NoScaleOffset] _SoftbodyPrevSurfaceDispTex("Softbody Previous Contact Displacement", 2D) = "black" {}
        [HideInInspector][NoScaleOffset] _SoftbodySurfaceNormalTex("Softbody Contact Normals", 2D) = "black" {}
        [HideInInspector][NoScaleOffset] _SoftbodySurfaceLumpNormalTex("Softbody Lump Normals", 2D) = "black" {}

        // 컴포넌트가 매 프레임 넣는 값.
        [HideInInspector] _SoftbodyInertiaResponseCount("Softbody Softness Rows", Float) = 0.0
        [HideInInspector] _SoftbodySurfaceFieldOn("Softbody Contact On", Float) = 0.0
        [HideInInspector] _SoftbodySurfaceAttachRange("Softbody Follow Range", Float) = 10.0
        [HideInInspector] _SoftbodyMaxDisplacement("Softbody Max Displacement", Float) = 150.0
        [HideInInspector] _SoftbodyVisWeight("Softbody Debug View", Float) = 0.0
        [HideInInspector] _SoftbodyVisMode("Softbody Debug View Mode", Float) = 0.0
        [HideInInspector] _SoftbodyL2WX("Softbody Frame Row X", Vector) = (1, 0, 0, 0)
        [HideInInspector] _SoftbodyL2WY("Softbody Frame Row Y", Vector) = (0, 1, 0, 0)
        [HideInInspector] _SoftbodyL2WZ("Softbody Frame Row Z", Vector) = (0, 0, 1, 0)
        [HideInInspector] _SoftbodyPrevL2WX("Softbody Previous Frame Row X", Vector) = (1, 0, 0, 0)
        [HideInInspector] _SoftbodyPrevL2WY("Softbody Previous Frame Row Y", Vector) = (0, 1, 0, 0)
        [HideInInspector] _SoftbodyPrevL2WZ("Softbody Previous Frame Row Z", Vector) = (0, 0, 1, 0)
        [HideInInspector] _SoftbodyTextureSize("Softbody Table Size", Float) = 0.0
        [HideInInspector] _SoftbodyOriginWS("Softbody Origin", Vector) = (0, 0, 0, 0)
        [HideInInspector] _SoftbodyPrevOriginWS("Softbody Previous Origin", Vector) = (0, 0, 0, 0)
        [HideInInspector] _SoftbodyCarrierVertexUVOn("Softbody Contact Map UVs On", Float) = 0.0
    }

    SubShader
    {
        // AlwaysRenderMotionVectors: 정지한 렌더러도 모션 벡터 패스를 그리게 — 살이 흔들리거나 눌려도 속도가 잡힌다(Shader Graph와 같은 태그).
        Tags
        {
            "RenderType" = "Opaque"
            "RenderPipeline" = "UniversalPipeline"
            "UniversalMaterialType" = "Lit"
            "IgnoreProjector" = "True"
            "AlwaysRenderMotionVectors" = "true"
        }
        LOD 300

        // ------------------------------------------------------------------
        //  Forward pass. Shades all light in a single pass. GI + emission + Fog
        Pass
        {
            Name "ForwardLit"
            Tags
            {
                "LightMode" = "UniversalForward"
            }

            // -------------------------------------
            // Render State Commands
            Blend[_SrcBlend][_DstBlend], [_SrcBlendAlpha][_DstBlendAlpha]
            ZWrite[_ZWrite]
            Cull[_Cull]
            AlphaToMask[_AlphaToMask]

            HLSLPROGRAM
            // 개발: URP Lit은 2.0. 정점 번호(SV_VertexID)와 텍스처 Load가 필요하다(표면 장 컴퓨트가 SM5를 요구하므로 대상 GPU는 모두 된다).
            #pragma target 4.5

            // -------------------------------------
            // Shader Stages
            #pragma vertex SoftbodyLitPassVertex
            #pragma fragment SoftbodyLitPassFragment

            // -------------------------------------
            // Material Keywords
            #pragma shader_feature_local _NORMALMAP
            #pragma shader_feature_local _PARALLAXMAP
            #pragma shader_feature_local _RECEIVE_SHADOWS_OFF
            #pragma shader_feature_local _ _DETAIL_MULX2 _DETAIL_SCALED
            #pragma shader_feature_local_fragment _SURFACE_TYPE_TRANSPARENT
            #pragma shader_feature_local_fragment _ALPHATEST_ON
            #pragma shader_feature_local_fragment _ _ALPHAPREMULTIPLY_ON _ALPHAMODULATE_ON
            #pragma shader_feature_local_fragment _EMISSION
            #pragma shader_feature_local_fragment _METALLICSPECGLOSSMAP
            #pragma shader_feature_local_fragment _SMOOTHNESS_TEXTURE_ALBEDO_CHANNEL_A
            #pragma shader_feature_local_fragment _OCCLUSIONMAP
            #pragma shader_feature_local_fragment _SPECULARHIGHLIGHTS_OFF
            #pragma shader_feature_local_fragment _ENVIRONMENTREFLECTIONS_OFF
            #pragma shader_feature_local_fragment _SPECULAR_SETUP

            // -------------------------------------
            // Universal Pipeline keywords
            #pragma multi_compile _ _MAIN_LIGHT_SHADOWS _MAIN_LIGHT_SHADOWS_CASCADE _MAIN_LIGHT_SHADOWS_SCREEN
            #pragma multi_compile _ _ADDITIONAL_LIGHTS_VERTEX _ADDITIONAL_LIGHTS
            #pragma multi_compile _ EVALUATE_SH_MIXED EVALUATE_SH_VERTEX
            #pragma multi_compile_fragment _ _ADDITIONAL_LIGHT_SHADOWS
            #pragma multi_compile_fragment _ _REFLECTION_PROBE_BLENDING
            #pragma multi_compile_fragment _ _REFLECTION_PROBE_BOX_PROJECTION
            #pragma multi_compile_fragment _ _REFLECTION_PROBE_ATLAS
            #pragma multi_compile_fragment _ _SHADOWS_SOFT _SHADOWS_SOFT_LOW _SHADOWS_SOFT_MEDIUM _SHADOWS_SOFT_HIGH
            #pragma multi_compile_fragment _ _SCREEN_SPACE_OCCLUSION
            #pragma multi_compile_fragment _ _SCREEN_SPACE_IRRADIANCE
            #pragma multi_compile_fragment _ _DBUFFER_MRT1 _DBUFFER_MRT2 _DBUFFER_MRT3
            #pragma multi_compile_fragment _ _LIGHT_COOKIES
            #pragma multi_compile _ _LIGHT_LAYERS
            #pragma multi_compile _ _CLUSTER_LIGHT_LOOP
            #include_with_pragmas "Packages/com.unity.render-pipelines.core/ShaderLibrary/FoveatedRenderingKeywords.hlsl"
            #include_with_pragmas "Packages/com.unity.render-pipelines.universal/ShaderLibrary/RenderingLayers.hlsl"

            // -------------------------------------
            // Unity defined keywords
            #pragma multi_compile _ LIGHTMAP_SHADOW_MIXING
            #pragma multi_compile _ SHADOWS_SHADOWMASK
            #pragma multi_compile _ DIRLIGHTMAP_COMBINED
            #pragma multi_compile _ LIGHTMAP_ON
            #pragma multi_compile_fragment _ LIGHTMAP_BICUBIC_SAMPLING
            #pragma multi_compile_fragment _ REFLECTION_PROBE_ROTATION
            #pragma multi_compile _ DYNAMICLIGHTMAP_ON
            #pragma multi_compile _ USE_LEGACY_LIGHTMAPS
            #pragma multi_compile _ LOD_FADE_CROSSFADE
            #pragma multi_compile_fragment _ DEBUG_DISPLAY
            #include_with_pragmas "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Fog.hlsl"
            #include_with_pragmas "Packages/com.unity.render-pipelines.universal/ShaderLibrary/ProbeVolumeVariants.hlsl"

            //--------------------------------------
            // GPU Instancing (DOTS 인스턴싱은 넣지 않는다)
            #pragma multi_compile_instancing
            #pragma instancing_options renderinglayer

            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitForwardPass.hlsl"
            ENDHLSL
        }

        Pass
        {
            Name "ShadowCaster"
            Tags
            {
                "LightMode" = "ShadowCaster"
            }

            // -------------------------------------
            // Render State Commands
            ZWrite On
            ZTest LEqual
            ColorMask 0
            Cull[_Cull]

            HLSLPROGRAM
            #pragma target 4.5

            // -------------------------------------
            // Shader Stages
            #pragma vertex SoftbodyShadowPassVertex
            #pragma fragment ShadowPassFragment

            // -------------------------------------
            // Material Keywords
            #pragma shader_feature_local _ALPHATEST_ON
            #pragma shader_feature_local_fragment _SMOOTHNESS_TEXTURE_ALBEDO_CHANNEL_A

            //--------------------------------------
            // GPU Instancing
            #pragma multi_compile_instancing

            // -------------------------------------
            // Unity defined keywords
            #pragma multi_compile _ LOD_FADE_CROSSFADE

            // This is used during shadow map generation to differentiate between directional and punctual light shadows, as they use different formulas to apply Normal Bias
            #pragma multi_compile_vertex _ _CASTING_PUNCTUAL_LIGHT_SHADOW

            // -------------------------------------
            // Includes
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitShadowCasterPass.hlsl"
            ENDHLSL
        }

        Pass
        {
            Name "GBuffer"
            Tags
            {
                "LightMode" = "UniversalGBuffer"
            }

            // -------------------------------------
            // Render State Commands
            ZWrite[_ZWrite]
            ZTest LEqual
            Cull[_Cull]

            HLSLPROGRAM
            #pragma target 4.5

            // Deferred Rendering Path does not support the OpenGL-based graphics API:
            // Desktop OpenGL, OpenGL ES 3.0, WebGL 2.0.
            #pragma exclude_renderers gles3 glcore

            // -------------------------------------
            // Shader Stages
            #pragma vertex SoftbodyLitGBufferPassVertex
            #pragma fragment SoftbodyLitGBufferPassFragment

            // -------------------------------------
            // Material Keywords
            #pragma shader_feature_local _NORMALMAP
            #pragma shader_feature_local_fragment _ALPHATEST_ON
            //#pragma shader_feature_local_fragment _ALPHAPREMULTIPLY_ON
            #pragma shader_feature_local_fragment _EMISSION
            #pragma shader_feature_local_fragment _METALLICSPECGLOSSMAP
            #pragma shader_feature_local_fragment _SMOOTHNESS_TEXTURE_ALBEDO_CHANNEL_A
            #pragma shader_feature_local_fragment _OCCLUSIONMAP
            #pragma shader_feature_local _PARALLAXMAP
            #pragma shader_feature_local _ _DETAIL_MULX2 _DETAIL_SCALED

            #pragma shader_feature_local_fragment _SPECULARHIGHLIGHTS_OFF
            #pragma shader_feature_local_fragment _ENVIRONMENTREFLECTIONS_OFF
            #pragma shader_feature_local_fragment _SPECULAR_SETUP
            #pragma shader_feature_local _RECEIVE_SHADOWS_OFF

            // -------------------------------------
            // Universal Pipeline keywords
            #pragma multi_compile _ _MAIN_LIGHT_SHADOWS _MAIN_LIGHT_SHADOWS_CASCADE _MAIN_LIGHT_SHADOWS_SCREEN
            //#pragma multi_compile _ _ADDITIONAL_LIGHTS_VERTEX _ADDITIONAL_LIGHTS
            //#pragma multi_compile _ _ADDITIONAL_LIGHT_SHADOWS
            #pragma multi_compile_fragment _ _REFLECTION_PROBE_BLENDING
            #pragma multi_compile_fragment _ _REFLECTION_PROBE_BOX_PROJECTION
            #pragma multi_compile_fragment _ _SHADOWS_SOFT _SHADOWS_SOFT_LOW _SHADOWS_SOFT_MEDIUM _SHADOWS_SOFT_HIGH
            #pragma multi_compile_fragment _ _DBUFFER_MRT1 _DBUFFER_MRT2 _DBUFFER_MRT3
            #pragma multi_compile_fragment _ _RENDER_PASS_ENABLED
            #pragma multi_compile _ _CLUSTER_LIGHT_LOOP
            #pragma multi_compile _ EVALUATE_SH_MIXED EVALUATE_SH_VERTEX
            #include_with_pragmas "Packages/com.unity.render-pipelines.core/ShaderLibrary/FoveatedRenderingKeywords.hlsl"
            #include_with_pragmas "Packages/com.unity.render-pipelines.universal/ShaderLibrary/RenderingLayers.hlsl"

            // -------------------------------------
            // Unity defined keywords
            #pragma multi_compile _ LIGHTMAP_SHADOW_MIXING
            #pragma multi_compile _ SHADOWS_SHADOWMASK
            #pragma multi_compile _ DIRLIGHTMAP_COMBINED
            #pragma multi_compile _ LIGHTMAP_ON
            #pragma multi_compile_fragment _ LIGHTMAP_BICUBIC_SAMPLING
            #pragma multi_compile_fragment _ REFLECTION_PROBE_ROTATION
            #pragma multi_compile _ DYNAMICLIGHTMAP_ON
            #pragma multi_compile _ USE_LEGACY_LIGHTMAPS
            #pragma multi_compile _ LOD_FADE_CROSSFADE
            #pragma multi_compile_fragment _ _GBUFFER_NORMALS_OCT
            #pragma multi_compile_fragment _ _SCREEN_SPACE_IRRADIANCE
            #include_with_pragmas "Packages/com.unity.render-pipelines.universal/ShaderLibrary/ProbeVolumeVariants.hlsl"

            //--------------------------------------
            // GPU Instancing
            #pragma multi_compile_instancing
            #pragma instancing_options renderinglayer

            // -------------------------------------
            // Includes
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitGBufferPass.hlsl"
            #include_with_pragmas "Packages/com.unity.render-pipelines.universal/ShaderLibrary/GBufferOutputFormat.hlsl"
            ENDHLSL
        }

        Pass
        {
            Name "DepthOnly"
            Tags
            {
                "LightMode" = "DepthOnly"
            }

            // -------------------------------------
            // Render State Commands
            ZWrite On
            ColorMask R
            Cull[_Cull]

            HLSLPROGRAM
            #pragma target 4.5

            // -------------------------------------
            // Shader Stages
            #pragma vertex SoftbodyDepthOnlyVertex
            #pragma fragment DepthOnlyFragment

            // -------------------------------------
            // Material Keywords
            #pragma shader_feature_local _ALPHATEST_ON
            #pragma shader_feature_local_fragment _SMOOTHNESS_TEXTURE_ALBEDO_CHANNEL_A

            // -------------------------------------
            // Unity defined keywords
            #pragma multi_compile _ LOD_FADE_CROSSFADE

            //--------------------------------------
            // GPU Instancing
            #pragma multi_compile_instancing

            // -------------------------------------
            // Includes
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitDepthOnlyPass.hlsl"
            ENDHLSL
        }

        // This pass is used when drawing to a _CameraNormalsTexture texture
        Pass
        {
            Name "DepthNormals"
            Tags
            {
                "LightMode" = "DepthNormals"
            }

            // -------------------------------------
            // Render State Commands
            ZWrite On
            Cull[_Cull]

            HLSLPROGRAM
            #pragma target 4.5

            // -------------------------------------
            // Shader Stages
            #pragma vertex SoftbodyDepthNormalsVertex
            #pragma fragment SoftbodyDepthNormalsFragment

            // -------------------------------------
            // Material Keywords
            #pragma shader_feature_local _NORMALMAP
            #pragma shader_feature_local _PARALLAXMAP
            #pragma shader_feature_local _ _DETAIL_MULX2 _DETAIL_SCALED
            #pragma shader_feature_local _ALPHATEST_ON
            #pragma shader_feature_local_fragment _SMOOTHNESS_TEXTURE_ALBEDO_CHANNEL_A

            // -------------------------------------
            // Unity defined keywords
            #pragma multi_compile _ LOD_FADE_CROSSFADE

            // -------------------------------------
            // Universal Pipeline keywords
            #include_with_pragmas "Packages/com.unity.render-pipelines.universal/ShaderLibrary/RenderingLayers.hlsl"

            //--------------------------------------
            // GPU Instancing
            #pragma multi_compile_instancing

            // -------------------------------------
            // Includes
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitDepthNormalsPass.hlsl"
            ENDHLSL
        }

        // This pass it not used during regular rendering, only for lightmap baking.
        Pass
        {
            Name "Meta"
            Tags
            {
                "LightMode" = "Meta"
            }

            // -------------------------------------
            // Render State Commands
            Cull Off

            HLSLPROGRAM
            #pragma target 4.5

            // -------------------------------------
            // Shader Stages
            #pragma vertex UniversalVertexMeta
            #pragma fragment UniversalFragmentMetaLit

            // -------------------------------------
            // Material Keywords
            #pragma shader_feature_local_fragment _SPECULAR_SETUP
            #pragma shader_feature_local_fragment _EMISSION
            #pragma shader_feature_local_fragment _METALLICSPECGLOSSMAP
            #pragma shader_feature_local_fragment _ALPHATEST_ON
            #pragma shader_feature_local_fragment _ _SMOOTHNESS_TEXTURE_ALBEDO_CHANNEL_A
            #pragma shader_feature_local _ _DETAIL_MULX2 _DETAIL_SCALED
            #pragma shader_feature_local_fragment _SPECGLOSSMAP
            #pragma shader_feature EDITOR_VISUALIZATION

            // -------------------------------------
            // Includes
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
            #include "Packages/com.unity.render-pipelines.universal/Shaders/LitMetaPass.hlsl"

            ENDHLSL
        }

        Pass
        {
            Name "MotionVectors"
            Tags { "LightMode" = "MotionVectors" }
            ColorMask RG

            HLSLPROGRAM
            #pragma shader_feature_local _ALPHATEST_ON
            #pragma multi_compile _ LOD_FADE_CROSSFADE

            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
            #include_with_pragmas "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyObjectMotionVectors.hlsl"
            ENDHLSL
        }

        Pass
        {
            Name "XRMotionVectors"
            Tags { "LightMode" = "XRMotionVectors" }
            ColorMask RGBA

            // Stencil write for obj motion pixels
            Stencil
            {
                WriteMask 1
                Ref 1
                Comp Always
                Pass Replace
            }

            HLSLPROGRAM
            #pragma shader_feature_local _ALPHATEST_ON
            #pragma multi_compile _ LOD_FADE_CROSSFADE
            #define APPLICATION_SPACE_WARP_MOTION 1

            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
            #include_with_pragmas "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyObjectMotionVectors.hlsl"
            ENDHLSL
        }

        Pass
        {
            Name "SceneSelectionPass"
            Tags { "LightMode" = "SceneSelectionPass" }

            // -------------------------------------
            // Render State Commands
            BlendOp Add
            Blend One Zero
            ZWrite On
            Cull[_Cull]

            HLSLPROGRAM
            #pragma target 4.5
            #pragma vertex SoftbodyEditorVertex
            #pragma fragment SoftbodySceneSelectionFragment
            #pragma shader_feature_local _ALPHATEST_ON
            #pragma multi_compile_instancing

            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitEditorPass.hlsl"
            ENDHLSL
        }

        Pass
        {
            Name "ScenePickingPass"
            Tags { "LightMode" = "Picking" }

            // -------------------------------------
            // Render State Commands
            BlendOp Add
            Blend One Zero
            ZWrite On
            Cull[_Cull]

            HLSLPROGRAM
            #pragma target 4.5
            #pragma vertex SoftbodyEditorVertex
            #pragma fragment SoftbodyScenePickingFragment
            #pragma shader_feature_local _ALPHATEST_ON
            #pragma multi_compile_instancing

            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitInput.hlsl"
            #include "Packages/com.wjsekrud.mallangphysics/Shaders/Lit/SoftbodyLitEditorPass.hlsl"
            ENDHLSL
        }
    }

    FallBack "Hidden/Universal Render Pipeline/FallbackError"
    CustomEditor "Softbody.Editor.Rendering.SoftbodyLitShaderGUI"
}
