Shader "ShaderEmu/ShareUnits"
{
    // What a player sends of their display, made here whole: Udon copies one row of bytes
    // (docs/share.md, "Units"; tools/share_units.py is the same in numpy, pass for pass).
    // The picture is cut into blocks of four sizes, 4x4 samples of 1, 2, 4 or 8 pixels (levels
    // 0 to 3); a unit is 4x4 blocks of one level. _State keeps for each 8x8 cell the level the
    // receiver has it at (4: stale) and the packet that carried it.
    Properties
    {
        _MainTex ("Capture: the decoded display", 2D) = "black" {}
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

        Texture2D<float4> _Now, _Before;   // this capture and the last one, 1280 x 960
        Texture2D<float4> _Blocks;         // 640 x 450: two texels a block, level 0 from row 0, 1 from 240, 2 from 360, 3 from 420
        Texture2D<float4> _Exact;          // 160 x 210: a texel a block of level 1 (from row 0), 2 (120), 3 (180)
        Texture2D<float4> _Need;           // 160 x 120: the coarsest level that shows a cell as level 0 does
        Texture2D<float4> _State;          // 160 x 120: r, the level held; g, the packet
        Texture2D<float4> _Stats;          // 1 x 1: the level each class goes at
        Texture2D<float4> _Plan;           // 160 x 120: r, 1 + the level a cell goes at; g, its class
        Texture2D<float4> _Units;          // 80 x 113: level 3's units from row 0, 2 from 8, 1 from 23, 0 from 53
        Texture2D<float4> _Prefix;         // 256 x 1: the bytes before each 64 places of the order; 200: all; 201: the stale cells' units'
        Texture2D<float4> _Cut;            // 2 x 1: the stale units' bytes that go, and all that go; where the next packet starts
        float4 _Size;                      // the picture's width and height
        float4 _Lost;                      // a packet number and how many from it were lost
        float _Reset, _Room, _Seq, _Turn;

        static const uint CellsX = 160, CellsY = 120, Chunk = 64, Places = 12760, None = 0xffffffff;

        float4 vert(appdata_img v) : SV_Position {
            return UnityObjectToClipPos(v.vertex);
        }

        uint4 bytes(float4 v) {
            return (uint4)(v * 255.0 + 0.5);
        }

        float4 out_bytes(uint a, uint b, uint c, uint d) {
            return float4(a, b, c, d) / 255.0;
        }

        uint units_across(uint level) { return 80u >> level; }
        uint units_of(uint level) { return level == 0 ? 4800 : level == 1 ? 1200 : level == 2 ? 300 : 80; }
        uint units_row(uint level) { return level == 0 ? 53 : level == 1 ? 23 : level == 2 ? 8 : 0; }
        uint blocks_row(uint level) { return level == 0 ? 0 : level == 1 ? 240 : level == 2 ? 360 : 420; }

        void block_at(uint level, uint2 b, out uint lo, out uint hi, out uint mask) {
            int3 at = int3(b.x * 2, blocks_row(level) + b.y, 0);
            uint4 c = bytes(_Blocks.Load(at)), m = bytes(_Blocks.Load(at + int3(1, 0, 0)));
            lo = c.r | c.g << 8;
            hi = c.b | c.a << 8;
            mask = m.r | m.g << 8;
        }

        // A block's kind after the blocks before it in its unit, and its bytes (up to four, then two).
        void block_code(uint level, uint2 b, inout uint flat, inout uint pair, out uint kind, out uint size, out uint data0, out uint data1) {
            uint lo, hi, mask;
            block_at(level, b, lo, hi, mask);
            data1 = 0;
            if (mask == 0) {
                kind = lo == flat ? 0 : 1;
                size = kind == 0 ? 0 : 2;
                flat = lo;
                data0 = lo;
            } else {
                uint both = lo | hi << 16;
                kind = both == pair ? 2 : 3;
                size = kind == 2 ? 2 : 6;
                pair = both;
                data0 = kind == 2 ? mask : both;
                data1 = mask;
            }
        }

        // A unit's texel: its 16 bits, its bytes, 1 + its class (0: it is not there).
        uint4 unit_at(uint level, uint index) {
            uint across = units_across(level);
            return bytes(_Units.Load(int3(index % across, units_row(level) + index / across, 0)));
        }

        // The order units go in: stale before the rest, coarse before sharp, each level from the
        // place the last packet stopped at (_Turn, as a unit of level 0). A place's class, level and unit.
        void place(uint p, out uint cls, out uint level, out uint index) {
            cls = p >= 6380 ? 1 : 0;
            uint q = p - cls * 6380;
            level = q < 80 ? 3 : q < 380 ? 2 : q < 1580 ? 1 : 0;
            uint n = units_of(level), local = q - (level == 3 ? 0 : level == 2 ? 80 : level == 1 ? 380 : 1580);
            index = (local + (uint)_Turn * n / 4800) % n;
        }

        uint place_of(uint cls, uint level, uint index) {
            uint n = units_of(level);
            return cls * 6380 + (level == 3 ? 0 : level == 2 ? 80 : level == 1 ? 380 : 1580) + (index + n - (uint)_Turn * n / 4800) % n;
        }

        uint bytes_at_place(uint p) {
            uint cls, level, index;
            place(p, cls, level, index);
            uint4 u = unit_at(level, index);
            return u.a == cls + 1 ? u.b : 0;
        }

        uint chunk_before(uint j) {
            uint4 v = bytes(_Prefix.Load(int3(j, 0, 0)));
            return v.r | v.g << 8 | v.b << 16;
        }

        // The bytes before a place.
        uint before_place(uint p) {
            uint sum = chunk_before(p / Chunk);
            [loop] for (uint q = p / Chunk * Chunk; q < p; q++) sum += bytes_at_place(q);
            return sum;
        }

        // The place whose record holds byte n of the order, and where that record starts.
        bool find(uint n, out uint p, out uint start, out uint size) {
            uint low = 0, high = 200;
            [loop] for (uint k = 0; k < 8; k++) {
                uint mid = (low + high + 1) / 2;
                if (chunk_before(mid) <= n) low = mid;
                else high = mid - 1;
            }
            uint sum = chunk_before(low);
            bool found = false;
            p = 0;
            start = 0;
            size = 0;
            [loop] for (uint q = low * Chunk; q < low * Chunk + Chunk && q < Places; q++) {
                uint s = bytes_at_place(q);
                if (!found && n < sum + s) {
                    found = true;
                    p = q;
                    start = sum;
                    size = s;
                }
                sum += s;
            }
            return found;
        }

        ENDCG

        // 0: picture pixel (x, y) to texel (x, y), as sRGB values; _Src.z display pixels a texel.
        Pass
        {
            Name "Capture"
            CGPROGRAM
            sampler2D _MainTex;
            float4 _Src;       // the display's width and height, and display pixels a texel
            float4 _TexSize;   // of _MainTex

            float4 frag(float4 at : SV_Position) : SV_Target {
                float2 q = (floor(at.xy) + 0.5) * _Src.z;
                if (q.x >= _Src.x || q.y >= _Src.y) return 0;
                float3 c = tex2Dlod(_MainTex, float4(q.x / _TexSize.x, 1.0 - q.y / _TexSize.y, 0, log2(_Src.z))).rgb;
#ifndef UNITY_COLORSPACE_GAMMA
                c = c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
#endif
                return float4(c, 1);
            }
            ENDCG
        }

        // 1: every block of every level: its two colours (5:6:5: the means of the samples darker
        // and lighter than the block's mean), then a bit a sample. All in whole numbers.
        Pass
        {
            Name "Blocks"
            CGPROGRAM
            uint pack(uint3 total, uint count) {
                uint d = count * 255;
                uint3 v = (2 * total * uint3(31, 63, 31) + d) / (2 * d);
                return v.r << 11 | v.g << 5 | v.b;
            }

            float4 frag(float4 at : SV_Position) : SV_Target {
                uint x = (uint)at.x, y = (uint)at.y;
                uint level = y < 240 ? 0 : y < 360 ? 1 : y < 420 ? 2 : 3;
                uint2 b = uint2(x / 2, y - blocks_row(level));
                if (b.x >= (320u >> level)) return 0;
                uint n = 1u << level, i;
                uint3 s[16];
                uint luma[16], total = 0;
                [loop] for (i = 0; i < 16; i++) {
                    uint2 corner = (b * 4 + uint2(i & 3, i >> 2)) * n;
                    uint3 sum = 0;
                    [loop] for (uint v = 0; v < n; v++) {
                        [loop] for (uint u = 0; u < n; u++) sum += bytes(_Now.Load(int3(corner + uint2(u, v), 0))).rgb;
                    }
                    s[i] = sum;
                    luma[i] = 299 * sum.r + 587 * sum.g + 114 * sum.b;
                    total += luma[i];
                }
                uint3 low = 0, high = 0;
                uint highs = 0, mask = 0;
                [loop] for (i = 0; i < 16; i++) {
                    if (16 * luma[i] > total) {
                        high += s[i];
                        highs++;
                        mask |= 1u << i;
                    } else low += s[i];
                }
                uint lo = pack(low, (16 - highs) * n * n);
                uint hi = highs > 0 ? pack(high, highs * n * n) : lo;
                if (lo == hi) mask = 0;
                if (mask == 0) hi = lo;
                if ((x & 1) == 0) return out_bytes(lo & 255, lo >> 8, hi & 255, hi >> 8);
                return out_bytes(mask & 255, mask >> 8, 0, 0);
            }
            ENDCG
        }

        // 2: whether a block of level 1, 2 or 3 shows every pixel as level 0 does.
        Pass
        {
            Name "Exact"
            CGPROGRAM
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint x = (uint)at.x, y = (uint)at.y;
                uint level = y < 120 ? 1 : y < 180 ? 2 : 3;
                uint2 b = uint2(x, y - (level == 1 ? 0 : level == 2 ? 120 : 180));
                if (b.x >= (320u >> level)) return 0;
                uint lo, hi, mask, k = 1u << level;
                block_at(level, b, lo, hi, mask);
                bool same = true;
                [loop] for (uint j = 0; j < k; j++) {
                    [loop] for (uint i = 0; i < k; i++) {
                        uint lo0, hi0, mask0;
                        block_at(0, b * k + uint2(i, j), lo0, hi0, mask0);
                        [loop] for (uint s = 0; s < 16; s++) {
                            uint2 p = uint2(i * 4 + (s & 3), j * 4 + (s >> 2)) / k;
                            uint mine = (mask >> (p.y * 4 + p.x)) & 1 ? hi : lo;
                            uint its = (mask0 >> s) & 1 ? hi0 : lo0;
                            if (mine != its) same = false;
                        }
                    }
                }
                return same ? 1.0 : 0.0;
            }
            ENDCG
        }

        // 3: the coarsest level a cell needs: its block exact there and at each finer level.
        Pass
        {
            Name "Need"
            CGPROGRAM
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint2 c = (uint2)at.xy;
                bool e1 = _Exact.Load(int3(c, 0)).r > 0.5;
                bool e2 = _Exact.Load(int3(c.x / 2, 120 + c.y / 2, 0)).r > 0.5;
                bool e3 = _Exact.Load(int3(c.x / 4, 180 + c.y / 4, 0)).r > 0.5;
                return out_bytes(e1 ? e2 ? e3 ? 3 : 2 : 1 : 0, 0, 0, 0);
            }
            ENDCG
        }

        // 4: a cell that changed since the last capture, or went with a lost packet, is stale.
        Pass
        {
            Name "Marks"
            CGPROGRAM
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint2 c = (uint2)at.xy;
                if (c.x * 8 >= (uint)_Size.x || c.y * 8 >= (uint)_Size.y) return 0;
                uint4 was = bytes(_State.Load(int3(c, 0)));
                bool stale = _Reset > 0.5;
                [loop] for (uint j = 0; j < 8; j++) {
                    [loop] for (uint i = 0; i < 8; i++) {
                        int3 p = int3(c * 8 + uint2(i, j), 0);
                        if (any(_Now.Load(p).rgb != _Before.Load(p).rgb)) stale = true;
                    }
                }
                if (was.r < 4 && ((was.g - (uint)_Lost.x) & 255) < (uint)_Lost.y) stale = true;
                return out_bytes(stale ? 4 : was.r, stale ? 0 : was.g, 0, 0);
            }
            ENDCG
        }

        // 5: the stale cells as sharp as one packet can carry all of them, the rest as sharp as
        // two can (a cell reckoned at 12, 3, 0.75 or 0.19 bytes by level): r and g.
        Pass
        {
            Name "Stats"
            CGPROGRAM
            uint first(uint sums[4], uint room, uint packets) {
                uint level = 3;
                for (int k = 3; k >= 0; k--)
                    if (sums[k] <= packets * room * 16) level = k;
                return level;
            }

            float4 frag(float4 at : SV_Position) : SV_Target {
                static const uint cost[4] = { 192, 48, 12, 3 };
                uint es[4] = { 0, 0, 0, 0 }, et[4] = { 0, 0, 0, 0 }, most = 0;
                [loop] for (uint y = 0; y < CellsY; y++) {
                    [loop] for (uint x = 0; x < CellsX; x++) {
                        uint have = bytes(_State.Load(int3(x, y, 0))).r, need = bytes(_Need.Load(int3(x, y, 0))).r;
                        bool stale = have == 4, todo = have > need && have < 4;
                        if (todo) most = max(most, have);
                        for (uint k = 0; k < 4; k++) {
                            uint level = max(k, need);
                            if (stale) es[k] += cost[level];
                            if (todo && level < have) et[k] += cost[level];
                        }
                    }
                }
                uint room = (uint)_Room, fs = first(es, room, 1);
                uint ft = first(et, room - min(room, es[fs] / 16), 2);
                if (most > 0) ft = min(ft, most - 1);
                return out_bytes(fs, ft, 0, 0);
            }
            ENDCG
        }

        // 6: the level each wanted cell goes at: the coarsest whose block spoils nothing beside it.
        Pass
        {
            Name "Plan"
            CGPROGRAM
            // A cell's level if it is wanted and of this class, else -1; and what it has and needs.
            int wanted(uint2 c, int cls, uint2 floors, out int have, out int need) {
                have = bytes(_State.Load(int3(c, 0))).r;
                need = bytes(_Need.Load(int3(c, 0))).r;
                int mine = have == 4 ? 0 : have > need ? 1 : -1;
                int level = max((int)(mine == 0 ? floors.x : floors.y), need);
                return mine == cls && level < have ? level : -1;
            }

            float4 frag(float4 at : SV_Position) : SV_Target {
                uint2 c = (uint2)at.xy, floors = bytes(_Stats.Load(int3(0, 0, 0))).rg;
                int have, need, h, n;
                h = bytes(_State.Load(int3(c, 0))).r;
                n = bytes(_Need.Load(int3(c, 0))).r;
                int cls = h == 4 ? 0 : h > n ? 1 : -1;
                int level = wanted(c, cls, floors, have, need);
                if (cls < 0 || level < 0) return 0;
                bool ok3 = true, ok2 = true;
                uint i;
                [loop] for (i = 0; i < 16; i++) {
                    int t = wanted((c & ~3u) + uint2(i & 3, i >> 2), cls, floors, h, n);
                    if (t < 3 && n < 3 && h < 3) ok3 = false;
                }
                if (level >= 3 && !ok3) level = 2;
                [loop] for (i = 0; i < 4; i++) {
                    int t = wanted((c & ~1u) + uint2(i & 1, i >> 1), cls, floors, h, n);
                    if (t >= 3 && !ok3) t = 2;
                    if (t < 2 && n < 2 && h < 2) ok2 = false;
                }
                if (level >= 2 && !ok2) level = 1;
                return out_bytes(level + 1, cls, 0, 0);
            }
            ENDCG
        }

        // 7: for each unit a bit a block that goes, the bytes of its record, 1 + its class.
        Pass
        {
            Name "Units"
            CGPROGRAM
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint x = (uint)at.x, y = (uint)at.y;
                uint level = y < 8 ? 3 : y < 23 ? 2 : y < 53 ? 1 : 0;
                uint2 u = uint2(x, y - units_row(level));
                if (u.x >= units_across(level)) return 0;
                uint n = level == 0 ? 1 : 1u << (level - 1);   // cells across a block
                uint mask = 0, count = 0, size = 0, flat = None, pair = None;
                bool stale = false;
                [loop] for (uint b = 0; b < 16; b++) {
                    uint2 block = u * 4 + uint2(b & 3, b >> 2);
                    uint2 corner = level == 0 ? block / 2 : block * n;
                    bool goes = false;
                    [loop] for (uint j = 0; j < n; j++) {
                        [loop] for (uint i = 0; i < n; i++) {
                            uint4 p = bytes(_Plan.Load(int3(corner + uint2(i, j), 0)));
                            if (corner.y < CellsY && p.r == level + 1) {
                                goes = true;
                                if (p.g == 0) stale = true;
                            }
                        }
                    }
                    if (goes) {
                        uint kind, bytes_, data0, data1;
                        block_code(level, block, flat, pair, kind, bytes_, data0, data1);
                        mask |= 1u << b;
                        count++;
                        size += bytes_;
                    }
                }
                if (count == 0) return 0;
                return out_bytes(mask & 255, mask >> 8, 4 + (count + 3) / 4 + size, stale ? 1 : 2);
            }
            ENDCG
        }

        // 8: the bytes before each 64 places of the order; texel 200 and on: all of them, but
        // 201: those of the stale cells' units (the places before 6380).
        Pass
        {
            Name "Prefix"
            CGPROGRAM
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint end = (uint)at.x == 201 ? 6380 : min((uint)at.x * Chunk, Places), sum = 0;
                [loop] for (uint p = 0; p < end; p++) sum += bytes_at_place(p);
                return out_bytes(sum & 255, (sum >> 8) & 255, sum >> 16, 0);
            }
            ENDCG
        }

        // 9: where the packet is cut. The stale cells' units go while they fit in the room less a
        // quarter (kept for the rest, when there is any: what stands still sharpens while
        // something else moves), then the rest's while they fit. Texel 0: the stale units' bytes
        // that go, and all that go; texel 1: where the next packet starts, which is the unit
        // the first cut fell on (as a unit of level 0), or where this one did.
        Pass
        {
            Name "Cut"
            CGPROGRAM
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint room = (uint)_Room, all = chunk_before(200), staleAll = chunk_before(201);
                uint stale = staleAll, turn = (uint)_Turn, p = 0, q, start, size;
                bool cut = false;
                if (staleAll > (all > staleAll ? room - room / 4 : room)) {
                    find(all > staleAll ? room - room / 4 : room, p, start, size);
                    stale = start;
                    cut = true;
                }
                uint rest = all - staleAll;
                if (rest > room - stale) {
                    find(staleAll + room - stale, q, start, size);
                    rest = start - staleAll;
                    if (!cut) p = q;
                    cut = true;
                }
                if (cut) {
                    uint cls, level, index;
                    place(p, cls, level, index);
                    turn = index * (4800 / units_of(level));
                }
                uint length = stale + rest;
                if ((uint)at.x == 0) return out_bytes(stale & 255, stale >> 8, length & 255, length >> 8);
                return out_bytes(turn & 255, turn >> 8, 0, 0);
            }
            ENDCG
        }

        // 10: the packet: the records of the units that go, in their order. Texel 288: how many
        // bytes that is; texel 289: where the next packet starts.
        Pass
        {
            Name "Packet"
            CGPROGRAM
            uint record_byte(uint p, uint o) {
                uint cls, level, index;
                place(p, cls, level, index);
                uint across = units_across(level), v = level << 13 | index;
                uint4 unit = unit_at(level, index);
                uint mask = unit.r | unit.g << 8;
                if (o < 4) return o == 0 ? v & 255 : o == 1 ? v >> 8 : o == 2 ? mask & 255 : mask >> 8;
                uint2 u = uint2(index % across, index / across);
                uint head = 4 + (countbits(mask) + 3) / 4, m = 0, where = 0, flat = None, pair = None, value = 0;
                [loop] for (uint b = 0; b < 16; b++) {
                    if ((mask >> b) & 1) {
                        uint kind, size, data0, data1;
                        block_code(level, u * 4 + uint2(b & 3, b >> 2), flat, pair, kind, size, data0, data1);
                        if (o == 4 + m / 4) value |= kind << (2 * (m & 3));
                        if (o >= head + where && o < head + where + size) {
                            uint i = o - head - where;
                            value = i < 4 ? (data0 >> (8 * i)) & 255 : (data1 >> (8 * (i - 4))) & 255;
                        }
                        where += size;
                        m++;
                    }
                }
                return value;
            }

            // (after the stale units' bytes that go come the rest's, from where those begin in the order)
            uint packet_byte(uint n, uint stale, uint length) {
                uint p, start, size;
                if (n >= length) return 0;
                uint m = n < stale ? n : n - stale + chunk_before(201);
                if (find(m, p, start, size)) return record_byte(p, m - start);
                return 0;
            }

            float4 frag(float4 at : SV_Position) : SV_Target {
                uint x = (uint)at.x;
                uint4 cut = bytes(_Cut.Load(int3(0, 0, 0)));
                uint stale = cut.r | cut.g << 8, length = cut.b | cut.a << 8;
                if (x >= 288) return x == 288 ? out_bytes(length & 255, length >> 8, 0, 0) : x == 289 ? _Cut.Load(int3(1, 0, 0)) : 0;
                return out_bytes(packet_byte(x * 4, stale, length), packet_byte(x * 4 + 1, stale, length), packet_byte(x * 4 + 2, stale, length), packet_byte(x * 4 + 3, stale, length));
            }
            ENDCG
        }

        // 11: the state when that packet has gone: a cell is at the sharpest level a block of
        // the packet covers it with, and carries the packet's number.
        Pass
        {
            Name "State"
            CGPROGRAM
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint2 c = (uint2)at.xy;
                uint best = 9, staleAll = chunk_before(201);
                uint4 cut = bytes(_Cut.Load(int3(0, 0, 0)));
                uint stale = cut.r | cut.g << 8, length = cut.b | cut.a << 8;
                [loop] for (uint level = 0; level < 4; level++) {
                    uint2 block = level == 0 ? c * 2 : c >> (level - 1);
                    uint index = (block.y / 4) * units_across(level) + block.x / 4;
                    uint4 unit = unit_at(level, index);
                    uint mask = unit.r | unit.g << 8;
                    if (unit.a != 0 && ((mask >> ((block.y & 3) * 4 + (block.x & 3))) & 1) != 0) {
                        uint before = before_place(place_of(unit.a - 1, level, index));
                        if (unit.a == 1 ? before + unit.b <= stale : before - staleAll + unit.b <= length - stale) best = min(best, level);
                    }
                }
                if (best == 9) return _State.Load(int3(c, 0));
                return out_bytes(best, (uint)_Seq & 255, 0, 0);
            }
            ENDCG
        }

        // 12: for the test with two clients (EmuShareProbe): 24 x 18 pixels of the picture, the
        // middle one of each part, of a display or of a receiver's picture of one (the same way up).
        Pass
        {
            Name "Signature"
            CGPROGRAM
            sampler2D _MainTex;
            float4 _Src;       // the picture's width and height
            float4 _TexSize;   // of _MainTex

            float4 frag(float4 at : SV_Position) : SV_Target {
                float2 q = floor((floor(at.xy) + 0.5) * _Src.xy / float2(24, 18)) + 0.5;
                float3 c = tex2Dlod(_MainTex, float4(q.x / _TexSize.x, 1.0 - q.y / _TexSize.y, 0, 0)).rgb;
#ifndef UNITY_COLORSPACE_GAMMA
                c = c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
