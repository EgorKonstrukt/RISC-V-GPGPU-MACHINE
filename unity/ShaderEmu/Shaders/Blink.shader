Shader "ShaderEmu/Blink"
{
    // A small lamp that flickers (docs/stations.md): a computer's disk lamp, a network socket's.
    // Its sixteen bits are an eighth of a second each, the oldest the highest, played over and
    // over; a behaviour writes them now and then, so the lamp needs nothing every frame.
    Properties
    {
        _Color ("Lit", Color) = (1, 0.08, 0.04, 1)
        _Glow ("Brightness (above 1 it blooms)", Float) = 3
        _Dim ("How much of that an unlit lamp keeps", Range(0, 1)) = 0.02
        _Pattern ("Sixteen bits", Float) = 0
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }
        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

            float4 _Color;
            float _Glow, _Dim, _Pattern;

            struct appdata { float4 vertex : POSITION; UNITY_VERTEX_INPUT_INSTANCE_ID };
            struct v2f { float4 pos : SV_POSITION; UNITY_VERTEX_OUTPUT_STEREO };

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.pos = UnityObjectToClipPos(v.vertex);
                return o;
            }

            float4 frag(v2f i) : SV_Target
            {
                float bit = 15.0 - floor(frac(_Time.y * 0.5) * 16.0);
                float lit = fmod(floor(_Pattern / exp2(bit) + 0.001), 2.0);
                return float4(_Color.rgb * _Glow * lerp(_Dim, 1.0, lit), 1.0);
            }
            ENDCG
        }
    }
}
