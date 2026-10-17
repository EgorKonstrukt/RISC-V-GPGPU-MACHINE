Shader "ShaderEmu/ShareUnitsDecode"
{
    // Another player's display from the units of one packet (docs/world/share.md, "Units";
    // ShareUnits.shader makes them). Pass 0 finds each unit's record in the packet; pass 1
    // draws the pixels those units cover into the picture, which keeps the rest.
    Properties
    {
        _Packet ("A packet's units, four bytes a texel", 2D) = "black" {}
        _Length ("Bytes of them", Float) = 0
        _Index ("Pass 0's: where each unit's record is, plus 1", 2D) = "black" {}
        _Stream ("Width and height of the picture", Vector) = (0, 0, 0, 0)
        _TargetSize ("Size of the target", Vector) = (1280, 960, 0, 0)
    }
    SubShader
    {
        Cull Off
        ZTest Always
        ZWrite Off
        Blend One Zero

        CGINCLUDE
        #pragma target 5.0
        #pragma vertex vert
        #pragma fragment frag
        #include "UnityCG.cginc"

        Texture2D<float4> _Packet, _Index;
        float4 _Stream, _TargetSize;
        float _Length;

        struct v2f {
            float4 pos : SV_Position;
            float2 uv : TEXCOORD0;
        };

        v2f vert(appdata_img v) {
            v2f o;
            o.pos = UnityObjectToClipPos(v.vertex);
            o.uv = v.texcoord.xy;
            return o;
        }

        uint byte_at(uint n) {
            float4 v = _Packet.Load(int3(n >> 2, 0, 0));
            uint k = n & 3;
            return (uint)((k == 0 ? v.r : k == 1 ? v.g : k == 2 ? v.b : v.a) * 255.0 + 0.5);
        }

        uint two_at(uint n) {
            return byte_at(n) | byte_at(n + 1) << 8;
        }

        // The bytes of block m's kind and of the kinds' bytes after a record's four.
        uint kind_at(uint record, uint m) {
            return (byte_at(record + 4 + m / 4) >> (2 * (m & 3))) & 3;
        }

        uint units_across(uint level) { return 80u >> level; }
        uint units_row(uint level) { return level == 0 ? 53 : level == 1 ? 23 : level == 2 ? 8 : 0; }
        ENDCG

        // 0: a texel a unit (level 3's from row 0, 2 from 8, 1 from 23, 0 from 53): 1 + where
        // its record starts in the packet, 0 when the packet has none.
        Pass
        {
            Name "Index"
            CGPROGRAM
            float4 frag(v2f i) : SV_Target {
                uint x = (uint)i.pos.x, y = (uint)i.pos.y;
                uint level = y < 8 ? 3 : y < 23 ? 2 : y < 53 ? 1 : 0;
                if (x >= units_across(level)) return 0;
                uint mine = level << 13 | ((y - units_row(level)) * units_across(level) + x);
                uint at = 0, found = 0, length = (uint)_Length;
                [loop] for (uint r = 0; r < 170 && at + 4 <= length; r++) {
                    if (two_at(at) == mine) found = at + 1;
                    uint n = countbits(two_at(at + 2)), size = 4 + (n + 3) / 4;
                    [loop] for (uint m = 0; m < n; m++) {
                        uint kind = kind_at(at, m);
                        size += kind == 0 ? 0 : kind == 3 ? 6 : 2;
                    }
                    at += size;
                }
                return float4(found & 255, found >> 8, 0, 0) / 255.0;
            }
            ENDCG
        }

        // 1: a pixel a unit of the packet covers, by the sharpest of them; the rest is kept.
        Pass
        {
            Name "Draw"
            CGPROGRAM
            float4 frag(v2f i) : SV_Target {
                float2 q = float2(i.uv.x, 1.0 - i.uv.y) * _TargetSize.xy;
                if (q.x >= _Stream.x || q.y >= _Stream.y) discard;
                uint2 p = (uint2)q;
                uint record = 0, level = 0, bit = 0, mask = 0;
                [loop] for (uint k = 0; k < 4; k++) {
                    uint2 block = p >> (k + 2);
                    uint4 v = (uint4)(_Index.Load(int3(block.x / 4, units_row(k) + block.y / 4, 0)) * 255.0 + 0.5);
                    uint found = v.r | v.g << 8;
                    uint b = (block.y & 3) * 4 + (block.x & 3), bits = found == 0 ? 0 : two_at(found + 1);
                    if (record == 0 && ((bits >> b) & 1) != 0) {
                        record = found;
                        level = k;
                        bit = b;
                        mask = bits;
                    }
                }
                if (record == 0) discard;
                record--;
                // the block's colours are in its own bytes or in those of a block before it
                uint count = countbits(mask), mine = countbits(mask & ((1u << bit) - 1));
                uint at = record + 4 + (count + 3) / 4, flatAt = at, pairAt = at, kind = 0;
                [loop] for (uint m = 0; m <= mine; m++) {
                    kind = kind_at(record, m);
                    if (kind == 1) flatAt = at;
                    if (kind == 3) pairAt = at;
                    if (m < mine) at += kind == 0 ? 0 : kind == 3 ? 6 : 2;
                }
                uint2 s = (p >> level) & 3;
                uint colour = two_at(flatAt);
                if (kind >= 2) colour = two_at(pairAt + 2 * ((two_at(kind == 2 ? at : at + 4) >> (s.y * 4 + s.x)) & 1));
                // (as whole bytes: a target of bytes may round a fraction near a half either way)
                uint3 v = uint3(colour >> 11, (colour >> 5) & 63, colour & 31);
                float3 c = (float3)((v * 510 + uint3(31, 63, 31)) / uint3(62, 126, 62)) / 255.0;
#ifndef UNITY_COLORSPACE_GAMMA
                c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
