# Half-Life: Uplink

Xash3D FWGS with the HLSDK, built for Linux without an MMU (`linux/halflife/build.sh`, one of
`linux/nommu/build.sh`'s scripts), playing Valve's Uplink demo. The engine is in the shared image
and in `linux/prebuilt`; Uplink's data is not ours to pass on, so it is never committed: when an
image is made (`linux/nommu/image.py`, through `tools/make_image.ps1`) `linux/halflife/data.py`
fetches the installer from archive.org (md5-checked), unpacks it with REWise (`data.sh`, in WSL),
repacks it and puts it into that image's overlay. `HALFLIFE_DATA=0` makes an image without it,
and the starter then says the data is missing. The download and unpacking happen once
(`build/halflife`); a made image takes 2.5 s more with the repack cached.

    wsl -- bash linux/nommu/build.sh halflife/build.sh
    pwsh tools/make_image.ps1
    halflife +map hldemo1          (in the image; the desktop's and the start menu's Half-Life: Uplink)

The icon is Xash3D FWGS's own (`linux/apps/icons/halflife.png`).

The engine's changes are `linux/halflife/xash3d.patch` and `hlsdk.patch`; the machine's own
parts are beside them: OpenGL 1.1 on the GPU (`gl_shaderemu.c`), the level kept in GPU memory
(`gl_kept.c`), studio models on workers (`gl_studio_job.c`), the server's frame on a worker
(`sv_shaderemu.c`), the window (`vid_shaderemu.c`), the sound card (`snd_shaderemu.c`) and
libmedia's pictures and sounds (`img_media.c`, `snd_media.c`).

## The data

Uplink's `pak0.PAK` is 79.2 MB; `repack.py` makes it 34.6 MB when the image is made (cached in
`build/halflife/repack-cache`), and the game's folder is 39.7 MB in all:

| | Before | After | How |
|---|---|---|---|
| sounds | 29.8 MB | 18.8 MB | the card's IMA ADPCM with checkpoints, in each WAV (format 0x11, a `ckpt` chunk) |
| maps | 26.5 MB | 5.8 MB | textures once in `uplink.wad` (3.75 MB) without mips; the rest deflate in segments |
| models | 7.8 MB | 5.8 MB | deflate in segments, files of 32 KB or more |
| sprites | 6.7 MB | 2.7 MB | the same |
| pictures | 7.9 MB | 1.4 MB | 24-bit ones JPEG at quality 70 under their own names; the sky's BMP copies left out |

- **Sounds** play where they lie in the ROM, as before: no decoding and no RAM. A voice
  started mid-sound starts at a checkpoint, a multiple of 32 samples. MP3 was tried
  (`REPACK_SOUNDS=mp3`, 8.7 MB, decoded by libmedia when a level registers its sounds): the
  272 sounds of hldemo1, 368 s of audio, took 11.4 s on 42 workers and 22 s with their
  setup, and 8 MB of RAM, after which a 2 MB allocation failed. FLAC decodes only some 10%
  cheaper. The decoder stays in `snd_shaderemu.c` for that switch.
- **Deflate in segments** (`docs/system/media.md`): the filesystem inflates such a file when it
  is opened (`FS_OpenInflated` in `filesystem/pak.c`), its pieces on the workers, and serves
  reads from memory. Smaller files stay as they are: an 8 KB piece is about 40 ms on the card
  and a small file waits for one whole (176 files took 7 s; 64 files of 32 KB or more, 3.6 s).
- **Textures** of a name with two pictures in two maps stay in their map. A picture with
  `offsets[1] == 0` is one level with its palette after it (`Image_LoadMIP`). WAD textures are
  kept as 8-bit, as a map's own are, so the GPU draws them paletted.
- A JPEG is told by its first bytes whatever its name (`Image_ProbeLoadBuffer_`).

Smaller data does not make a load faster here: the ROM is copied, not read through a disk,
and every byte saved is decoding work. Launch to "client connected" on the card: 61 to 66 s
before, 65 s after.

## GPU memory

The pool is 7.27 MB. The menu's pictures (`pfnPIC_Load`) and the static light maps are marked
`hlglSpillable`: when the pool has no room, those not drawn for two frames move to RAM and come
back when drawn. Blocks of 64 KB and more are taken from the top of the pool. Greyscale and
4-bit pictures are paletted too. Without this the menu's pictures left no room for the light
maps' atlas or the kept level, and the level was drawn unlit.
