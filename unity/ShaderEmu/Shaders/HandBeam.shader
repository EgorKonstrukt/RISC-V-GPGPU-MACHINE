Shader "ShaderEmu/HandBeam"
{
    // Where a holodeck's sitter's hand points in their program's world (docs/world/holodeck.md): a thin
    // ribbon from the object's place along its +z, turned to the eye, added to what is behind it.
    // It is drawn after the program's world and before the room's seal, so that world's depth cuts it.
    Properties
    {
        _Color ("Colour, and how solid at the hand", Color) = (0.72, 0.95, 1, 0.55)
        _Width ("Width, metres", Float) = 0.0035
        _Length ("Length, metres", Float) = 3
        _Fade ("How soon it fades along its length", Range(0.5, 4)) = 1.6
    }
    SubShader
    {
        Tags { "Queue"="Transparent-496" "RenderType"="Transparent" "IgnoreProjector"="True" "DisableBatching"="True" }
        Blend SrcAlpha One, Zero One
        ZWrite Off
        ZTest LEqual
        Cull Off

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

            float4 _Color;
            float _Width, _Length, _Fade;

            struct appdata {
                float4 vertex : POSITION;
                float2 uv : TEXCOORD0;      // x: -1 and 1 across, y: 0 at the hand to 1 at the end
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct v2f {
                float4 pos : SV_Position;
                float3 uv : TEXCOORD0;      // z: what a ribbon widened to a pixel and a half is dimmed by
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata v) {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                float3 from = mul(unity_ObjectToWorld, float4(0, 0, 0, 1)).xyz;
                float3 way = normalize(mul((float3x3)unity_ObjectToWorld, float3(0, 0, 1)));
                float3 at = from + way * (_Length * v.uv.y);
                float3 eye = _WorldSpaceCameraPos;   // (each eye's own)
                float3 side = cross(way, at - eye);
                side /= max(length(side), 1e-6);
                // never thinner than a pixel and a half, which flickers: wider and fainter instead
                float pixel = distance(at, eye) * 2.0 / (abs(UNITY_MATRIX_P[1][1]) * _ScreenParams.y);
                float half = max(_Width * 0.5, pixel * 0.75);
                o.pos = UnityWorldToClipPos(at + side * (v.uv.x * half));
                o.uv = float3(v.uv, _Width * 0.5 / half);
                return o;
            }

            float4 frag(v2f i) : SV_Target {
                float across = saturate(1.0 - i.uv.x * i.uv.x);
                float along = pow(saturate(1.0 - i.uv.y), _Fade);
                return float4(_Color.rgb, _Color.a * across * along * i.uv.z);
            }
            ENDCG
        }
    }
}
