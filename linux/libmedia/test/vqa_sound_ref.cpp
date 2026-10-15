// The games' own VQA sound decoding (vanilla-conquer's soscodec.cpp, as vqaloader.cpp calls it):
// every SND2 chunk of FILE.VQA through one stream, 16-bit samples to OUT.raw (check_vqa.py --game).
//   g++ -O2 -I$WORK/src/vc-occ/common vqa_sound_ref.cpp $WORK/src/vc-occ/common/soscodec.cpp -o vqa_sound_ref
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "soscomp.h"

int main(int argc, char** argv)
{
    FILE* f = fopen(argv[1], "rb");
    std::vector<unsigned char> d;
    int c;
    while ((c = fgetc(f)) != EOF)
        d.push_back((unsigned char)c);
    fclose(f);
    // (the header's channels and bits, as vqaloader reads them)
    _SOS_COMPRESS_INFO info;
    sosCODECInitStream(&info);
    info.wBitSize = 16;
    info.wChannels = 1;
    std::vector<char> out;
    for (size_t at = 12; at + 8 <= d.size();) {
        unsigned size = d[at + 4] << 24 | d[at + 5] << 16 | d[at + 6] << 8 | d[at + 7];
        if (!memcmp(&d[at], "VQHD", 4)) {
            info.wChannels = d[at + 8 + 0x1a];
            info.wBitSize = d[at + 8 + 0x1b];
        }
        if (!memcmp(&d[at], "SND2", 4)) {
            unsigned decomp = size * (info.wBitSize / 4);
            size_t was = out.size();
            out.resize(was + decomp);
            info.lpSource = (char*)&d[at + 8];
            info.lpDest = &out[was];
            sosCODECDecompressData(&info, decomp);
        }
        at += 8 + size + (size & 1);
    }
    FILE* o = fopen(argv[2], "wb");
    fwrite(out.data(), 1, out.size(), o);
    fclose(o);
    printf("%s: %u samples, %d channels, %d bits\n", argv[1], (unsigned)(out.size() / 2), info.wChannels, info.wBitSize);
    return 0;
}
