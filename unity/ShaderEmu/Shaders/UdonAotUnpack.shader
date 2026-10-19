// A boot image's four lanes (ShaderEmuImages.cs: a lane is one word of every 16-byte texel, texel 0
// in the last row) as one word a pixel, in order from the target's first row: what EmuUdonCpu reads
// back into an int[].
Shader "ShaderEmu/UdonAotUnpack"
{
    Properties
    {
        _MainTex ("Unused", 2D) = "black" {}
        _LaneR ("Lane 0", 2D) = "black" {}
        _LaneG ("Lane 1", 2D) = "black" {}
        _LaneB ("Lane 2", 2D) = "black" {}
        _LaneA ("Lane 3", 2D) = "black" {}
        _Texel0 ("First texel", Float) = 0
    }
    SubShader
    {
        Pass
        {
            ZTest Always Cull Off ZWrite Off
            CGPROGRAM
            #pragma vertex vert_img
            #pragma fragment frag
            #include "UnityCG.cginc"

            Texture2D<float4> _LaneR, _LaneG, _LaneB, _LaneA;
            float4 _LaneR_TexelSize;
            float _Texel0;

            float4 frag(v2f_img i) : SV_Target
            {
                uint2 p = uint2(i.pos.xy);
                uint w = p.y * 2048 + p.x;
                uint t = (uint)_Texel0 + (w >> 2);
                uint h = (uint)_LaneR_TexelSize.w;
                if ((t >> 11) >= h) return 0;
                int3 c = int3(t & 2047, h - 1 - (t >> 11), 0);
                uint lane = w & 3;
                return lane == 0 ? _LaneR.Load(c) : lane == 1 ? _LaneG.Load(c) : lane == 2 ? _LaneB.Load(c) : _LaneA.Load(c);
            }
            ENDCG
        }
    }
}
