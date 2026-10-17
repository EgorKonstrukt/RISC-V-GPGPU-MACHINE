Shader "ShaderEmu/HoloGuard"
{
    // A holodeck's own things (the seat, the stand, its panel, its screen) are before the
    // program's world however near that reaches (docs/world/holodeck.md): a copy of each draws
    // nothing but a mark in the stencil where the thing itself is seen, after the room's mask
    // and before the program's world, and no pass of that world draws on a pixel so marked.
    // One copy has every room's things: only those in the room this visitor sits in mark
    // (EmuHolodeck sets its box). Another room's showed as holes in this room's world.
    SubShader
    {
        Tags { "Queue"="Transparent-498" "RenderType"="Opaque" }
        Pass
        {
            ColorMask 0
            ZWrite Off
            ZTest LEqual
            Offset -1, -1      // the copy lies on the thing itself
            Cull Off
            Stencil { Ref 215 Comp Always Pass Replace }   // 87 + 128 (GpuHolodeck.shader)

            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

            float4 _UdonHoloMin, _UdonHoloMax;   // the sitter's room, in the world; nothing set: no room

            struct v2f {
                float4 pos : SV_Position;
                float3 world : TEXCOORD0;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata_base v) {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.pos = UnityObjectToClipPos(v.vertex);
                o.world = mul(unity_ObjectToWorld, v.vertex).xyz;
                return o;
            }

            float4 frag(v2f i) : SV_Target {
                clip(min(min(i.world.x - _UdonHoloMin.x, i.world.y - _UdonHoloMin.y), i.world.z - _UdonHoloMin.z));
                clip(min(min(_UdonHoloMax.x - i.world.x, _UdonHoloMax.y - i.world.y), _UdonHoloMax.z - i.world.z));
                clip(_UdonHoloMax.x - _UdonHoloMin.x - 0.001);
                return 0;
            }
            ENDCG
        }
    }
}
