// The GPU's picture as RGBA32 bytes that read as 0x00RRGGBB words, for EmuUdonCpu's readback into
// its RAM (gpu.h's gpu_writeback): pixel (x, y) of the target at texel (x, y).
Shader "ShaderEmu/UdonAotGpuBack"
{
    Properties
    {
        _MainTex ("The GPU's colour target", 2D) = "black" {}
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

            float4 frag(v2f_img i) : SV_Target
            {
                float3 c = floor(_MainTex.Load(int3(i.pos.xy, 0)).rgb * 255.0 + 0.5) / 255.0;
                return float4(c.b, c.g, c.r, 0);
            }
            ENDCG
        }
    }
}
