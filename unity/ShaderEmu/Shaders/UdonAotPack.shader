// EmuUdonCpu's copy of the display's RAM, a word a pixel of an RGBA32 stage, as state texels:
// four words a texel, so that Display.shader reads it as it reads the machine's state.
Shader "ShaderEmu/UdonAotPack"
{
    Properties
    {
        _MainTex ("Stage", 2D) = "black" {}
        _Rows ("Rows of the stage", Float) = 64
        _DstRow ("The state row the stage's first goes to", Float) = 0
        _Keep ("1: rows outside the stage's keep what they had", Float) = 0
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

            Texture2D<float4> _MainTex;
            float _Rows, _DstRow, _Keep;

            uint4 frag(v2f_img i) : SV_Target
            {
                uint2 p = uint2(i.pos.xy);
                p.y -= (uint)_DstRow;
                if (p.y >= (uint)_Rows)
                {
                    if (_Keep > 0.5) discard;
                    return 0;
                }
                uint w[4];
                for (uint k = 0; k < 4; k++)
                {
                    uint4 b = (uint4)round(_MainTex.Load(int3(p.x * 4 + k, p.y, 0)) * 255.0);
                    w[k] = b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
                }
                return uint4(w[0], w[1], w[2], w[3]);
            }
            ENDCG
        }
    }
}
