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
    halflife                       (in the image; the desktop's and the start menu's Half-Life: Uplink)
    halflife -dev 1 +map hldemo1   (straight into the level, its messages on the console)

The icon is Xash3D FWGS's own (`linux/apps/icons/halflife.png`).

The engine's changes are `linux/halflife/xash3d.patch`, `hlsdk.patch` and `mainui.patch` (the
menus); the machine's own parts are beside them: OpenGL 1.1 on the GPU (`gl_shaderemu.c`), the
level kept in GPU memory (`gl_kept.c`), studio models on workers (`gl_studio_job.c`), the server's
frame on a worker (`sv_shaderemu.c`), the window (`vid_shaderemu.c`), the films
(`avi_shaderemu.c`), the sound card (`snd_shaderemu.c`) and libmedia's pictures and sounds
(`img_media.c`, `snd_media.c`).

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

## The start

As the demo's launcher did: its splash (`logo.bmp`, drawn into the window as soon as the engine
starts, `SE_Splash` in `vid_shaderemu.c`; `-nosplash` leaves it out), the opening film
(`media/intro.avi`, the "Game of the Year" montage, 54 s, which Escape skips), then the main menu
with the animated Uplink logo over it (`media/uplink.avi` as `logo.avi`). The menu is mainui's
with `linux/halflife/mainui.patch`: for a game of one player (`type "singleplayer_only"` in
`gameinfo.txt`, which the engine itself overrides for a folder called `valve`) it has the demo's
buttons, New game, Configuration, Load game, View Readme.txt, Order Half-Life and Quit, without
Multiplayer, Custom game or Previews; View Readme.txt shows the demo's `readme.txt` in a list
(`menus/Readme.cpp`), and Order Half-Life opens the demo's own order page with `open`
(`Platform_ShellExecute` in `sys_posix.c`). New game asks Easy, Medium or Difficult and starts
hldemo1 at that skill. The menu's sounds are the launcher's (`media/launch_*.wav`, in pak0). No
`liblist.gam` is copied: newer than `gameinfo.txt`, the engine would take Multiplayer and the
first map from it. The engine reaches its first frame 2.5 s after it starts (`rvc_harness --cpu`).

The films are played by `avi_shaderemu.c`, which replaces FFmpeg's: Microsoft RLE and plain
8-bit frames, a picture a frame into one texture. A film's sound is played by the card where it
lies in the ROM when it is one chunk: `data.py` moves the opening film's sound in front of its
frames.

## Speed

`HL_MARK=FROM,TO` prints `hlmark: begins` and `hlmark: ends, N frames` at those seconds of the
level, for `rvc_cpu --count-from` and `--until`. On 18 October 2026, hldemo1 on `rvc_cpu` (core 0
a frame, frames a second at 428 passes a second):

| | core 0 | fps |
|---|---|---|
| the main menu (`FILM_LOOPS=1`, the logo's loops 4 to 8) | 185k | 19.0 |
| the start, 3 to 25 s | 182k | 18.2 |
| the fight at 31 to 45 s (19 studio models, 51 solid entities) | 407k | 8.1 |

The menu was 2,070k a frame (1.7). The logo film (640 x 100, 110 frames, mostly deltas) is
decoded whole once on a worker and kept (`film_give` in `avi_shaderemu.c`; a film of 8 MB
decoded at most and no sound; `FILM=read` decodes as it plays), and a frame's tiles are copied
into its texture on a worker (`hlglTexLater`). The menu's text is drawn again from its quads of
the last time (`UI_DrawString` in `mainui.patch`, `CL_DrawString` in `xash3d.patch`, through
`hlglCaptureTo` and `hlglDrawQuads`): the first character is drawn and compared, the last drawn
to leave the engine's state as it was; `HL_MENU_TEXT=now` and `HL_TEXT=now` draw it as before.
`HLGL_PICSUM=1` prints a sum of the frame's 2D quads every 64 frames: the menu's must be the
same both ways (`8ffbd714`).

A studio model's bones are set up ahead on four workers when the entities are emitted
(`R_BonesAheadGive` in `gl_kept.c`), and the draw takes its rotation from there too
(`SE_StudioRotationTaken`; `HL_ROTATION=inline` makes it at the draw, `HL_ROTATION=check`
makes both and prints how many differ: it must say `0 of them wrong`). What is left of the fight
on core 0 is many pieces: the studio models' draws 93k (their setup and hand-off to the jobs
45k, light 12k, culling 8k), the player's prediction 48k, the entities' parse 40k and their
emission 42k. A frame of 15 there needs the studio draws off core 0 whole, with their GL state.

### The drawing a frame behind

`HL_RENDER=worker` draws frame n's 3D on a worker core of its own (`rnd_shaderemu.c`, worker
`workers - 8`) from copies of its entities (`R_SceneGive` in `gl_kept.c`) while core 0 makes
frame n + 1; frame n is shown with frame n + 1's 2D. `HL_RENDER=wait` puts the drawing on the
worker with core 0 waiting; the default draws on core 0. Every renderer function core 0 calls
goes through a thunk (`se_ref_guard`): the scene's additions go to core 0's own list, and the
frame's start and its 2D (`R_BeginFrame`, `R_Set2DMode`, `R_DrawStretchPic`, `FillRGBA`,
`Color4ub` and a few more, all void with values for arguments) are recorded while the drawing
is out and replayed in order when it is settled, at the swap (`se_ref_replay`: a0 to a7 and
the caller's first four stack words, floats too under ilp32). Any other call settles first.
`HL_LATER=0` draws the 2D after a settle instead. `HL_RENDER_CALLS=1` prints what core 0 calls
of the renderer between frames, what made it settle, and the job's timeline every 32 frames.

The studio models' points go to ring jobs on the other workers (`ring_post` in
`gl_shaderemu.c`); the job takes a ring that is free before waiting on one, and its ring jobs
are waited for at the swap by core 0, not at the job's end (`jobs_wait`; the give waits for the
late ones, which may read the copies). A job's other parts copy it on their own worker.

The server's frame is on a worker (`sv_shaderemu.c`) and paced: core 0 gives it a frame only
when the last is done, with the time of the frames skipped, so the server runs below the
client's frame rate when it is the longer (`HL_SERVER=every` gives every frame, `inline` runs
it on core 0). The packet's entities are packed by four parts on three packers and the
server's helper (`SE_PackJob` in `xash3d.patch`); the client and the server step their
entity rings by whole blocks, and the light styles skip the time of styles not used.

On 18 October 2026 (`rvc_cpu`, the card's model: `RVC_SOUND`, `RVC_WCACHE`, `RVC_ADFAULT`):

| | start, 3 to 25 s | the fight, 31 to 45 s |
|---|---|---|
| the default | 24.35 passes a frame, 17.6 fps | 48.15, 8.9 |
| `HL_RENDER=worker` | 23.58, 18.2 | 32.97, 13.0 |
| `HL_RENDER=worker HL_LATER=0` | 20.41, 21.0 | 36.39, 11.8 |

In the fight the drawing's worker is the bound (25 busy passes of 155k instructions, 22
studio models at 4.8k each), then core 0 from the settle to the give (the scene's copy 18k,
the replay 18k). At the start core 0 is: recording the HUD's text and replaying it costs 30k
more than drawing it. `HL_RENDER=worker` is not the default: `RVC_RACES` finds stores of core 0
and the drawing's worker to the same words in one pass (`RI`, `r_stats`, the light's
`trace_of`), which must be 0 first.

## The controls

On a desktop the left button held drags the view round, a quarter of a degree a pixel, as
Quake's and Quake III's; the right button fires and the middle one is the second fire. The
pointer's travel is read from the machine's input words (`Platform_RunEvents`), and the window
system is asked for events only when a button or a key changed and four times a second.

## From the seat

Played from the holodeck's seat the game reads the host's words (`docs/world/holodeck.md`,
"Played from the seat"; `SE_SeatMove` and `SE_SeatView` in `vid_shaderemu.c`): the eye is 64
units over the floor, the sticks walk and turn, the right trigger fires, the left is the second
fire, Space jumps; with tracked hands the right hand aims and the view model is held in it; with
a mouse it turns the view, 0.066 degrees a count. The picture drawn round the seat leaves no
surface out (`r_nocull`). `HALFLIFE_SEAT=1` prints how it is seated every 20 frames, and
`rvc_cpu --host 00000112,0,0,0,0,00007f00,0,0` walks a desktop's sitter forward.
