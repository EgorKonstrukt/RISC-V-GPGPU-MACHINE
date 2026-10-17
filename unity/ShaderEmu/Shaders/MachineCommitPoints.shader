Shader "ShaderEmu/MachineCommitPoints"
{
    // What the cores stored in a pass, drawn as points by a camera of its own (docs/machine/multicore.md,
    // "The commit as points"; main.shader's COMMIT_POINTS): a geometry shader makes each point the
    // rectangle it stands for, and the pixel shader is the commit's own. With _Copy the same
    // points only copy their texels from the state given, for the buffer the commit did not draw.
    Properties
    {
        _SelfTexture2D ("State before the commit (or, with _Copy, after it)", 2D) = "black" {}
        _TickState ("CPU state area after the tick", 2D) = "black" {}
        _GpuTarget ("The GPU's colour target", 2D) = "black" {}
        _Data_MTD_R ("MTD ROM Texture R", 2D) = "black" {}
        _Data_MTD_G ("MTD ROM Texture G", 2D) = "black" {}
        _Data_MTD_B ("MTD ROM Texture B", 2D) = "black" {}
        _Data_MTD_A ("MTD ROM Texture A", 2D) = "black" {}
        [ToggleUI] _Copy ("Copy the texels as they are", Int) = 0
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Always
        ZWrite Off
        Lighting Off
        Blend One Zero

        Pass
        {
            Name "CommitPoints"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex points_vert
            #pragma geometry commit_geom
            #pragma fragment frag
            #pragma editor_sync_compilation

            #define PASS_COMMIT
            #define GPU_DEVICE
            #define COMMIT_AS_POINTS

            #include "UnityCG.cginc"
            #include "MachineBlit.cginc"
            #include "MachineCommit.cginc"

            uniform uint _Copy;

            struct point_id {
                uint id : TEXCOORD0;
            };

            point_id points_vert(appdata_base v, uint id : SV_VertexID) {
                point_id o;
                o.id = id;
                return o;
            }

            // texel w of a core's state as the tick left it
            uint4 tick_state(uint core, uint w) {
                if (core == 0) return _TickState[uint2(w & 63, w >> 6)];
                return _TickState[mc_tile_at(mc_tile_of(core) + (w >> 6)) + uint2(w & 7, (w & 63) >> 3)];
            }

            struct point_v2f {
                float4 vertex : SV_Position;
                uint core : TEXCOORD0;   // whose store it is
            };

            void commit_quad(inout TriangleStream<point_v2f> stream, uint x, uint y, uint w, uint h) {
                for (uint corner = 0; corner < 4; corner++) {
                    float2 p = float2(x + (corner & 1) * w, y + (corner >> 1) * h) / (float2)s_dim;
                    point_v2f o;
                    o.core = commit_core;
                    // clip space as the card has it: the texture's top left is (-1, 1)
                    o.vertex = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.5, 1.0);
                    stream.Append(o);
                }
                stream.RestartStrip();
            }

            #define COMMIT_RAM_TEXELS (2048u * (4096u - 64u))
            // Point n is core 0's bucket n (256 places, 112 used, then its last store and its
            // copy), and from 258 on 130 a worker (128 places, its last store, its copy).
            [maxvertexcount(12)]
            void commit_geom(point point_id IN[1], inout TriangleStream<point_v2f> stream) {
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                // only the commit's own camera draws them (its target's size says which that is)
                if ((uint)_ScreenParams.x != s_dim.x || (uint)_ScreenParams.y != s_dim.y) return;
                uint id = IN[0].id, core = 0, e = id, bits = L1_TABLE_BITS;
                if (id >= 258) {
                    core = 1 + (id - 258) / 130;
                    e = (id - 258) % 130;
                    if (core >= CORES) return;
                    MC_GEO_READ
                    bits = mc_bits_of(core);
                    if (bits == 0) return;
                }
                commit_core = core;
                uint last = core == 0 ? 256 : 128;
                if (e < last) {
                    // a bucket of the cache: an entry's tag is the texel's number and one
                    if (e >= L1_BUCKETS_OF(bits)) return;
                    uint4 tags = tick_state(core, L1_STATE_AT + e * L1_STRIDE);
                    for (uint n = 0; n < 3; n++) {
                        uint tag = idx_uint4(tags, n);
                        if (tag != 0 && tag - 1 < COMMIT_RAM_TEXELS) commit_quad(stream, (tag - 1) % 2048, 64 + (tag - 1) / 2048, 1, 1);
                    }
                } else if (e == last) {
                    // the store a full cache had no room for
                    uint addr = tick_state(core, 8).r;
                    if ((addr >> 4) < COMMIT_RAM_TEXELS) commit_quad(stream, (addr >> 4) % 2048, 64 + (addr >> 4) / 2048, 1, 1);
                } else {
                    // a copy or a fill: the texels from its destination to its end, row by row
                    uint stalled = tick_state(core, 28).r;
                    uint4 op = tick_state(core, 38);
#ifdef CSR_FIELDS
                    op.g = tick_state(core, 37).a & 0x7fffffff;   // (no translation: types.cginc)
#endif
                    if ((stalled != STALL_MEMOP_COPY && stalled != STALL_MEMOP_FILL) || op.b == 0) return;
                    uint t0 = op.g >> 4, t1 = min((op.g + op.b - 1) >> 4, COMMIT_RAM_TEXELS - 1);
                    if (t0 > t1) return;
                    uint y0 = t0 / 2048, y1 = t1 / 2048, x0 = t0 % 2048, x1 = t1 % 2048;
                    if (y0 == y1) {
                        commit_quad(stream, x0, 64 + y0, x1 - x0 + 1, 1);
                    } else {
                        commit_quad(stream, x0, 64 + y0, 2048 - x0, 1);
                        commit_quad(stream, 0, 64 + y1, x1 + 1, 1);
                        if (y1 > y0 + 1) commit_quad(stream, 0, 64 + y0 + 1, 2048, y1 - y0 - 1);
                    }
                }
            }

            uint4 frag(point_v2f i) : SV_Target {
                uint2 pos = (uint2)i.vertex.xy;
                if (_Copy) return _SelfTexture2D[pos];
                commit_core = i.core;
                return commit_texel(pos);
            }
            ENDCG
        }
    }
}
