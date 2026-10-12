# The video player

`nxvideo` plays an MPEG-1 file (video, with layer II sound) on the worker cores. The decoder is
[pl_mpeg](https://github.com/phoboslab/pl_mpeg) (Dominic Szablewski, MIT), fetched by the build
at one commit; `linux/video/mpeg.c` arranges it for the workers and `linux/video/nxvideo.c` is
the player.

    / # nxvideo                 # the image's sample, again and again
    / # nxvideo FILE.mpg
    / # nxvideo --decode FILE   # no window: every picture decoded on this core, with sums

    wsl -- bash /mnt/c/.../linux/video/build.sh     # 7 s, after Nano-X's; then the image
    python tools\make_video.py [WIDTHxHEIGHT [SECONDS [KBITS]]]   # the sample, on the host

## How a film is shared out

One core cannot play it: a picture of the sample (320 x 176) is 1.8 million instructions,
which is 43 million a second at 24 pictures, and a second of its sound is 17 million more.

- **A group of pictures is a job.** The file must be encoded in closed groups with no B
  pictures (`-g 6 -bf 0 -flags +cgop`): a group then needs nothing outside itself. The first
  core takes the file apart once (the two streams, where each group begins, where each sound
  frame begins); after that a worker decodes a whole group into memory of its own.
- **A decoder is used again.** pl_mpeg's decoder is put back as it was made (a copy of its
  struct, its frames' memory kept) and pointed at the next group's bytes. The one change to
  pl_mpeg is a `sed` in the build: it moved a stream's bytes down as it read them, and here
  the stream is every core's to read.
- **A frame is three planes of bytes**, and the GPU makes the colours (fragment mode 6,
  `gpu.md`): a worker stores a byte and a half a pixel, not four, and nothing converts.
- **Two stages.** A texture has a physical address, so frames to show are in GPU memory, and
  that holds 88 of the sample's (the 3.25 MB a program has, and the display's spare
  framebuffer). Twenty groups at once would not fit. So a worker decodes into ordinary memory
  and, when the frames before its group's have been shown, copies the group into its places.
- **The sound is chunks.** Fourteen frames to a chunk, each chunk a job. A chunk decodes the
  frame before its first too and throws it away (the filter's past), and starts the filter's
  ring where sequential decoding would have it (`v_pos`): the samples are then the same to
  the last bit as the stream decoded in one piece. Without the ring's place they were a
  level off here and there, from sums made in another order.
- **The clock is the sound card's.** Decoded sound goes into a stream voice's ring
  (`sound.md`, as `nxplay` does) and the picture shown is the one the card has got to. A
  picture that is late is left out; when none is ready the sound waits.

With every worker the machine has, `nxvideo` takes 20 for pictures and 8 for the sound: 24
pictures a second need about 14, and more decoders are only more memory to bring in before
it starts (each is most of a megabyte). It starts about six seconds after its window: the
first groups are being decoded.

## What it costs

- **A picture is 1.39 million instructions** of the sample (`nxvideo --decode`), 1.79 as the
  decoder was first built: `mpeg.c` is compiled `-O3 -fno-tree-vectorize -fsched-pressure
  -funroll-loops` (`-O3` alone 1.40, `-O2 -funroll-loops` 1.56; the sums are the same). A
  second of its sound went from 17.5 to 16.7 million.
- **The first core only looks after the others**, and that was most of a pass: it asked the
  window system for events at every turn of its loop (a question and an answer are 13
  thousand instructions), walked every group of pictures to know how many are ready, and
  took each worker's mailbox to see whether it wanted a page. It asks the window system
  when the button or key words change, for a tenth of a second after, while a button is
  down, and else seven times a second; `ready()` goes on from where it stopped; the
  worker library looks before it takes a mailbox. With three workers the first core ran
  443 million instructions over the film and runs 247.
- **And then it rests** (`mcw_rest()`, `multicore.md`, "A core that rests"): a turn of its
  loop with no picture to show ends in the kernel, which has the core for anything else,
  where it ended in a pause of the program's own. A picture is shown without waiting for
  the GPU to have drawn it (`seglSwapEarly()`; no group is copied into the shown picture's
  place, and `play_from()` settles first), which was a pause a picture, and the sound goes
  into the ring 4 KB a turn while the card has an eighth of a second in hand (a chunk at
  once was more than a pass's stores). The sample on a fixed timestep of 2 ms a pass
  (`--fixed-dt 0.002`): 11,892 passes, of which 10,148 ended in the loop's pause and 532 in
  the swap's, the first core running 47.2% of them; now 11,912, of which 10,521 are rests
  and the first core runs 98.0% of the other 1,387. None left out, the film's clock the same.
  (Measured before the sound's 4 KB a turn went in: that has only been run under `rvc_cpu`,
  where the film plays through and the sums hold.)
- **The workers run 84% of their passes** (78% of them whole; the others run 2,200
  instructions): those are passes that end on a full write cache, 3 KB of stores. A group
  copied into GPU memory is nothing but stores (507 KB, some 170 passes of a worker), and
  so is a block copied from the picture before. It costs the film nothing: 20 decoders
  where 14 keep up. The first core taking a decoder's place would add a fifteenth of the
  decoding and answer the window a picture's 166 passes late; taking the copies (6 KB a
  pass) would need 340 of its 500 passes a second and leave pictures out when passes are
  long. Neither is done.
- **Opening**: the stream's start codes were looked for with `memchr` in two passes, a
  quarter of the start. One pass now, reading one byte in three (a byte over 1 where a
  code's 1 would be rules out the two places after it), and its notes of the headers and
  pictures: 16.7 million instructions to the program's first line became 12.5.
- The film's clock between the sound card's steps is the kernel's (`clock_gettime`), a
  system call a turn. Reading the machine's clock word instead was tried with the rest.

## Not this way

- Decoders made one a turn of the player's loop, so that the window need not wait: bringing
  memory in on the first core holds up the workers that are running, and no picture was
  ready after 13 seconds. They are all made before the first job.
- 49 decoders: 39 MB cleared before the window showed. The buffers are touched, not cleared.

## The window

The picture, and under it a strip: Play or Pause, a bar that shows where the film is and is
clicked or dragged to go elsewhere, the time, and Open (the file chooser, in `/root/Videos`).
Keys: Space pauses, Left and Right go five seconds back and on, Home to the start, `o` opens
another file, `q` leaves. The picture is a window of its own inside the program's, which is
what the GPU draws into; frames a second are in the caption.

- The window is there before the file is taken apart (`ui_framed`, `nanox.md`): two seconds
  after the program is started, with "opening it..." in the strip.
- Going elsewhere starts at the group of pictures the place is in. What was decoded is
  dropped, and the film goes on when a group has been decoded again: about five seconds on
  the shader machine, which is what one group takes one worker (the same as from the start).
  The strip says so meanwhile.

## Checking it

(`NXVIDEO_SUM=1` has the first core sum every picture: on the shader machine the film then
leaves most pictures out, whatever the build. Look at "left out" in a run without it.)

- `nxvideo --decode FILE` under `rvc_cpu` prints a sum of all pictures and of all sound, and
  what each costs. The build also makes the same program for the host
  (`~/shaderemu-linux/src/video/nxvideo-host --decode FILE`): the sums must be equal. The
  sample's: pictures `d124dc14`, sound `96a296a2`. `NXVIDEO_WHOLE=1` adds the sound decoded
  in one piece, which must be the chunks' sum.
- `NXVIDEO_SUM=1 NXVIDEO_ONCE=1 nxvideo` prints one sum of the pictures as they reached GPU
  memory: `034e8a1b` for the sample, with `NXVIDEO_WORKERS=0`, `3` and unset.
- `NXVIDEO_HOLD=100 nxvideo` stops at a picture (`nxvideo: holding`): with `--save-state`
  and `--gpu-capture`, `tools\gpu_reference.py SNAP BMP --size 640x352` must find every pixel
  within 2 levels.
- `nxvideostat:` lines, one a second: the picture shown, how many were left out, how many are
  ready, how many workers are busy. The sample plays with none left out.
- `nxvideo & sleep 9; nxkey q; sleep 4; pidof nxvideo` must find none, and `mctest 255 20000`
  must pass after it.
- `nxvideo & sleep 12; nxkey right; sleep 6`: the `nxvideostat:` lines' "film at" goes five
  seconds on, stands for some seconds, and counts again.

## The sample

`/root/Videos/Big Buck Bunny.mpg`: the first 20 seconds of the trailer of Big Buck Bunny,
(c) copyright 2008, Blender Foundation / www.bigbuckbunny.org, Creative Commons Attribution
3.0; 320 x 176, 24 pictures a second, 895 KB. `tools/make_video.py` fetches the trailer from
Wikimedia Commons and encodes it; the original is not kept in the repository.

## Not done

- Seeking, and a bar to show where the film is.
- Pictures larger than the sample at full rate: 640 x 352 is four times the work, some 56
  workers' worth, and 22 frames of GPU memory.
- B pictures, and groups that are not closed.

The sound's synthesis window adds four blocks of products a turn with the sum in a register
(`plm_window.py`, applied to the fetched `pl_mpeg.h`): the same additions in the same order,
so the same samples (`96a296a2`, on the host too), 17.5 million instructions a second of
sound to 12.1. With the pictures at 1.36 million each, the sample film's whole decode
(`nxvideo --decode FILE`) is 1,405 million instructions to 1,074.
