Shader "Nix/rvc"
{
    // The machine's processor for Linux without an MMU, a shader a draw (README.md): the tick
    // (tick.hlsl) and the commit. The GPU device and the sound card are experiments/rvc_opt's.
    Properties
    {
        [ToggleUI] _Init ("Init", float) = 0
        [ToggleUI] _InitRaw ("Init directly from _Data_RAM_RAW", float) = 0
        _Ticks ("Ticks per Frame", Int) = 8192
        _TicksDivisor ("Tick Divisor", Int) = 1
        _PlayerID ("Player ID", Int) = -1
        _RTC0 ("RTC0", Int) = 0
        _RTC1 ("RTC1", Int) = 0
        _Data_RAM_RAW ("RAM Texture RAW", 2D) = "black" {}
        _Data_DTB_R ("Device Tree Binary Texture R", 2D) = "black" {}
        _Data_DTB_G ("Device Tree Binary Texture G", 2D) = "black" {}
        _Data_DTB_B ("Device Tree Binary Texture B", 2D) = "black" {}
        _Data_DTB_A ("Device Tree Binary Texture A", 2D) = "black" {}
        _Data_MTD_R ("MTD ROM Texture R", 2D) = "black" {}
        _Data_MTD_G ("MTD ROM Texture G", 2D) = "black" {}
        _Data_MTD_B ("MTD ROM Texture B", 2D) = "black" {}
        _Data_MTD_A ("MTD ROM Texture A", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Off
        Lighting Off
        Blend One Zero

        Pass
        {
            Name "CPUTick"
            HLSLPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry tick_geom
            #pragma fragment frag
            #include "tick.hlsl"
            ENDHLSL
        }

        Pass
        {
            Name "Commit"
            HLSLPROGRAM
            #pragma target 5.0
            #pragma vertex commit_vert
            #pragma fragment frag
            #include "commit.hlsl"
            ENDHLSL
        }
    }
}
