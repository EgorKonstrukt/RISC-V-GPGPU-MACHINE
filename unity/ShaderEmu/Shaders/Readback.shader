Shader "ShaderEmu/Readback"
{
    // Blit target for the script's readback: state row 0 (64 texels), the 64 control texels at
    // RAM 0x87000000, and 112 texels of the row at 0x876b8000: the network's window (40,
    // docs/lan.md), then what the cores ran (64 from 0x876ba000: MC_STATS_ALL) and eight texels
    // of which cores run and which are asleep (0x876ba400: 32 cores a texel), one
    // output pixel per 32-bit word, its bytes in r, g, b, a. A round of
    // a frame writes row _Row of the target, and the script reads all of a frame's rows at once.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _Row ("The row of the target this round writes", Int) = 0
    }
    SubShader
    {
        Cull Off
        ZTest Always
        ZWrite Off
        Blend One Zero

        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            Texture2D<uint4> _State;

            static const uint Words = 960, ControlRow = 64 + 0x700000 / 2048, WindowRow = 64 + 0x76b800 / 2048;
            static const float Rows = 32;   // the target's height: EmuMachine's MaxRounds
            uniform uint _Row;

            // Blit's rectangle put on one row: clip space as the card has it, row 0 at y = 1.
            float4 vert(appdata_img v) : SV_Position {
                return float4(v.texcoord.x * 2.0 - 1.0, 1.0 - 2.0 * (_Row + v.texcoord.y) / Rows, 0.5, 1.0);
            }

            float4 frag(float4 at : SV_Position) : SV_Target {
                uint n = min((uint)at.x, Words - 1);
                uint texel = n / 4, k = n & 3;
                uint more = texel - 128;
                more = more < 40 ? more : 0x200 + (more - 40);   // (the counts, and right after them the bits)
                uint4 t = texel < 64 ? _State[uint2(texel, 0)] : texel < 128 ? _State[uint2(texel - 64, ControlRow)] : _State[uint2(more, WindowRow)];
                uint v = k == 0 ? t.r : k == 1 ? t.g : k == 2 ? t.b : t.a;
                return float4(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24) / 255.0;
            }
            ENDCG
        }
    }
}
