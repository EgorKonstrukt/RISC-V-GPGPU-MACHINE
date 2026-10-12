# An audio player: `nxplay`

A program of the desktop that plays WAV, MOD, FLAC and MP3 files through the sound card's
stream voice (`sound.md`). WAV and tracker tunes play as they are decoded. FLAC and MP3 at
44.1 kHz stereo cost more than the machine has, with every core at work: the player decodes
them ahead, an MP3 file's pieces on every worker core (`multicore.md`), and plays when what
is left can be decoded while it plays.

    wsl -- bash /mnt/c/Development/ShaderX86/linux/nanox/build.sh     (once)
    wsl -- bash /mnt/c/Development/ShaderX86/linux/play/build.sh      (8 s; the first time it also fetches)
    python tools\make_linux_image.py                                  (then a new shell snapshot)

In the guest: `nxplay FILE` plays at once; the Start menu's Other has it as "Music", and
`open` starts it for a sound file (`nxopen.84-play`). The window has the file's name, the
place in the tune (a bar that can be dragged), keys with signs for Previous, Play or Pause,
Stop and Next (`ui_glyph.h`), Repeat, Shuffle, Open..., a loudspeaker key that mutes and the
volume; under them a list of the tunes of the folder the tune is in (`/root/Music` when no
file was asked for), the one played picked out. A click on the picked row, or Enter, plays
it; the tune after it follows by itself when one ends (the list's first after its last only
with Repeat; any other with Shuffle). Space pauses, Left and Right go ten seconds back and
on, the arrows up and down and the paging keys are the list's, `+` and `-` (and the wheel
outside the list) the volume, `m` mutes, `n` and `p` are Next and Previous, `r` and `s`
Repeat and Shuffle, `o` opens a file, `q` or Escape leaves. Without a window system, or with
`NXPLAY_NOWINDOW=1`, it plays the file and leaves.

## What the build is made of (`linux/play`)

- `nxplay.c`: the window, the pieces, the card, WAV, and the MP3 file cut into pieces.
- `mod.h`: a ProTracker player in integers (31 samples; 4 channels, or the tags `6CHN`,
  `8CHN`, `nnCH`): the sequencer a tick at a time and a mixer of the nearest sample, at
  22,050 Hz, which the card brings to its own rate. XM is not played.
- `flac.c` over [dr_flac](https://github.com/mackron/dr_libs) (public domain or MIT No
  Attribution), one or two channels.
- `mp3.c` over [minimp3](https://github.com/lieff/minimp3) (CC0), compiled twice: as it is,
  and at half rate. `minimp3.patch` adds the half rate (the lower 16 of the 32 bands through
  the transform, every second sample out of the filter) and a way to give a frame for its
  bits only (`MP3D_SKIP`), which fills the bit reservoir without decoding.
- `build.sh` fetches the two headers at one commit each (checked by their sums) into
  `~/shaderemu-linux/src/play`; they are not in the repository. Everything that has floats
  is built with `-fsingle-precision-constant`: minimp3 compares each sample with `32766.5`,
  a double otherwise, and the machine has no doubles.
- `make_test.py` (on the host, with ffmpeg) makes `test/play-test.*`, which the build puts
  in `/usr/share`: a sweep with noise as WAV (1.6 s), FLAC (2 s) and MP3 (18 s, 128 kbit/s),
  all 44.1 kHz stereo, and a four-channel MOD of 31 s written by the script.
- `check_sum.py` works out a WAV file's sum on the host.

## How it plays

A tune is decoded in **pieces** of about half a second, and a piece is decoded where it will
lie in the stream's ring (262,144 frames of sound memory, six seconds at 44.1 kHz; 65,536
when sound memory has no megabyte left). The stream's frames are the pieces' in order, so a
piece's place is known before it is made (`piece_place`); when it is its turn the card is
told it is there (`feed`), and nothing is copied. The voice is 16-bit, mono or stereo, at
the file's own rate: the card resamples. A step of 0 holds the voice (pause, and while more
is decoded).

- A piece was decoded into memory of its own and copied into the ring: the same stores
  twice, and the second time all at once. A core keeps about 6 KB of new stores a pass of
  the machine (`docs/boot.md`, "Passes that end early"), so a piece of 18 KB to 88 KB was
  three to fifteen passes of the first core that ran a quarter of their instructions.
- A piece has no place in the ring when it would go round the ring's end (one in 57 of an
  MP3 file's at 44.1 kHz), when the ring is full of sound not yet heard, or when there is
  no card: it is decoded into memory of its own and copied, as before. With workers a piece
  the ring has no room for waits instead (there are seconds in hand then).
- A worker stores into the ring itself. A piece's bytes are a multiple of 16 whatever the
  file (a frame of 576 or 1,152 samples, one or two channels, whole or half rate), so two
  cores never store to the same 16 bytes; the ring's pages are touched before the first job.
- After going elsewhere in the tune no piece is given out until the workers have dropped
  theirs (`dropping`): a dropped piece's place is another piece's in the new stream.

| the shader machine (`tools\tick_util.ps1`) | the first core's busy passes, before | now | its instructions |
|---|---|---|---|
| `play-test.mp3`, 12 s, 48 workers | 89.1% of their instructions (295 passes ended on a full write cache) | 98.7% (17) | 24.3M to 20.8M |
| `play-test.mod`, 15 s | 98.5% (148) | 99.8% (8) | 43.4M to 40.8M |

- WAV, MOD and FLAC are decoded in order by the first core, a slice at a time between two
  looks at the window.
- An MP3 file is cut where its frames begin (a table made when it is opened). A piece is 20
  frames, and any core can decode any piece: it starts 12 frames early, gives the first ten
  for their bits only and decodes two for the filters' memory, after which its samples are
  those of a decoder that began at the file's start. That costs 5% over decoding in order.
- With 24 workers or more (it asks for all 63, and has its share of them) a piece is 4
  frames, not 20: 63 workers make
  8 s of the test file's sound in 2.5 s, so 44.1 kHz stereo plays at its full rate with no
  gap, heard 3.2 s after the file is opened (the kernel's fifteen workers made 0.6 s of
  sound a second: decoded ahead, heard after 4.2 s, and run dry once). What sets the time
  to the first sound is how long one piece takes a worker, not how many there are.
- The player asks for every worker that is free (`mcw_open(MC_MAX_CORES - 1)`) when an MP3 file is opened
  and gives them back with the tune. A job is one piece (`mp3_job`): its decoder is on the
  worker's stack, its samples go into the piece's own pages, and it says how far it is in 16
  bytes of its own, which is how the speed is known before a piece is finished. The first
  core takes pieces as well. No workers is the common case; the first core then decodes in
  order, with one decoder and no early frames.
- It measures how much sound a second of decoding makes. Fast enough: it plays with half a
  second in hand and decodes a few pieces ahead. With workers it says nothing about speed
  from the first pieces: they are being started then, and a verdict from those seconds was
  wrong ("cannot decode this as fast as it plays", for a file the workers then played at its
  full rate). It plays when one and a half seconds are ready, and only a ring that has run
  dry twice sends an MP3 file to half the rate. With no workers and too slow: it says so,
  decodes ahead as far as memory goes (half of what is free), and plays once the rest will
  be decoded by the time it is needed. A ring that runs dry holds the voice ("it ran dry:
  waiting for more to be decoded").
- Its window is there before the file is opened ("opening it..."), as `nanox.md` has it.

## Variables

| | |
|---|---|
| `NXPLAY_EXIT=1` | leaves when the tune ends |
| `NXPLAY_SECONDS=N` | plays only the first N seconds |
| `NXPLAY_NOWINDOW=1` | no window: plays and leaves |
| `NXPLAY_WORKERS=N` | asks for N workers (48); 0: this core alone |
| `NXPLAY_SUM=1` | prints a sum of every sample in the order played (FNV-1a, a 16-bit sample at a time) |
| `NXPLAY_HALF=1`, `0` | an MP3 file at half rate from the start, or never |
| `NXPLAY_PIECE=N` | MP3 frames a piece (20) |
| `NXPLAY_MODRATE=N` | the rate a tracker tune is mixed at (22050) |
| `NXPLAY_MEMORY=N` | megabytes of decoded sound it may keep |
| `NXPLAY_VOLUME=N` | 0 to 256 (200) |

It prints when it starts to play (`nxplay: plays from ...`), and at a tune's end a
`playstat:` line: the sound decoded, the time, and the instructions a second of sound cost
(`rdcycle` round the decoding on this core and in each job).

## What a second of sound costs

Instructions for a second of sound, counted under `rvc_cpu` (the test files; the figures
are the same within 3% on the shader machine):

| | Thousand instructions | |
|---|---|---|
| WAV, 16-bit stereo | 128 | a copy into the piece; a tune of one second, from the command to its end, was 1.3 million in all |
| MOD, mixed at 22,050 Hz | 1,421 | 2,820 at 44,100 Hz |
| FLAC, 44.1 kHz stereo | 11,742 | 133 a sample: Rice codes and a predictor of 8 terms |
| MP3, 44.1 kHz stereo | 16,649 | in order, one core |
| MP3 in pieces | 17,586 to 17,732 | 4, 8 and 16 cores: the early frames are the difference |
| MP3 at half rate | 10,766 | 11,394 to 11,488 in pieces |

(Those are of the decoders compiled `-O2`. They are `-O3 -fno-tree-vectorize
-fsched-pressure -funroll-loops` now, `nxplay.c` with the tracker's mixer too: the test files
played one after another, with workers and without, were 523 million instructions on the
first core and 702 on the workers, and are 412 and 520, with every sum as it was.)

And what the shader machine makes of it (`rvc_harness --d3d11`, an RTX 5090 with nothing
else on it; seconds of sound decoded in a second):

| | Core 0 alone | 4 cores | 16 cores |
|---|---|---|---|
| WAV | plays | | |
| MOD | 3.5 | | |
| FLAC | 0.44 | | |
| MP3 | 0.20 | 0.31 to 0.36 | 0.64 |
| MP3 at half rate | 0.30 | 0.55 | 0.86 |

- **WAV and MOD play as they are decoded** on one core: the tracker tune is heard 0.45 s
  after the file is opened.
- **FLAC does not**: it is decoded whole first on the one core it has (2 s of sound: heard
  after 4.7 s). Its frames stand by themselves, so it could be given to the workers as MP3
  is, without early frames; that is not built.
- **MP3 at 44.1 kHz stereo does not play as it is decoded on any number of cores.** The
  instructions would be there (16 cores were thought to run 18 to 21 million a second), but
  this work gets 6 million a second out of 4 cores and 11 million out of 16: a core that
  decodes runs at 3.3 million a second alone where integer work runs at 5, and a worker
  with a small write cache (1.5 KB of stores a pass) ran half the instructions of a large
  one in the same passes (minimp3 clears and copies 4 KB at a time, which may be why; it was
  not looked into). So the usual way is the third: half rate, decoded ahead. 8 s of the test file on 4 cores was heard 14 s after it
  was opened and played to its end without a gap.
- Other layouts of 16 cores were slower than the kernel's own (3 large workers and 12 small):
  7 large 0.56, 12 of size 5 0.60. (The card was partly in use by another program during
  the 16-core runs; two of them a while apart agreed.)
- A file of half the rate or one channel costs about half; both, a quarter, which 4 cores
  have. None was measured.

## Checking it

Under `rvc_cpu`, a few seconds each (start it from PowerShell):

    bin\rvc_cpu.exe --quiet --uart-log logs\play_x.log --expect "/ # " --send "NXPLAY_SUM=1 nxplay /usr/share/play-test.mp3; NXPLAY_SUM=1 NXPLAY_WORKERS=0 nxplay /usr/share/play-test.mp3; echo LX-''DONE\n" --until "LX-DONE"

| File | Sum |
|---|---|
| `play-test.wav` | `e5fd8f8c` of 141,120 samples; `python linux\play\check_sum.py linux\play\test\play-test.wav` says the same |
| `play-test.flac` | `1a14a562` of 176,400: the same script on ffmpeg's decoding of the file to WAV |
| `play-test.mod` | `98e66905` of 1,354,752 |
| `play-test.mp3` | `809816e7` of 1,594,368, with any number of workers and with none |
| the same with `NXPLAY_HALF=1` | `9efedb8f` of 797,184 |

- The MP3 sums are also what the same two files compiled on the host give (x86-64, gcc):
  the machine's float instructions under `rvc_cpu` decode as the host does.
- **On the shader machine an MP3 file's sum is another** (`a6c2aba3`, `c1e4a1b9` at half
  rate): its floats are the card's (`fpu.md`). In the first second of the test file 2 of
  88,200 samples differ from the host's, by 1. What must hold there is that the sum with
  workers is the sum with `NXPLAY_WORKERS=0` (`NXPLAY_SECONDS=3`: `45c4a9a9`
  without them, and with them after an `nxplay` that had them was ended by `kill -9`).
- The card: `NXPLAY_SECONDS=1 NXPLAY_EXIT=1 nxplay /usr/share/play-test.wav` with
  `--fixed-dt 0.004 --sound-capture F.wav --save-state F.snap`, then
  `python tools\sound_reference.py F.wav F.snap --rom build\images\linux\rootfs.bin`: every
  sample and device word matches. (A second fits the ring, so the snapshot still has it.)
  The same holds for a second of the MP3 and of the MOD file decoded in the ring, and their
  captures are those of the build that copied, sample for sample (the MOD's but for the
  length of its one wait for more: under `--fixed-dt` it decodes slower than it plays).
- The window: `nano-X -p & sleep 3; nxplay /usr/share/play-test.mod & sleep 4; nxkey right`
  must print `plays from` a second time, ten seconds on. A click of `nxkey` is only let go
  when the host's pointer moves: with `--pointer-sweep 0`, `nxkey 370,101` (the bar, three
  quarters along) prints `plays from` some 23 s into the tune and `nxkey 130,128` (Stop) goes
  back to 0. The list: two copies of the test tune in a folder, `nxplay` on one, `nxkey n`
  and `p` each print a `playstat` line (the tune left) and play the other;
  `NXPLAY_SECONDS=3` makes one end, and the other follows. The keys by the pointer, the
  window being the server's first: `nxkey 212,128` (Repeat) and `271,128` (Shuffle) each
  print what they turned on, `385,128` (the loudspeaker) `muted`, `438,128` (the volume's
  middle) `volume 128 of 256`, `wheel:300,60:1` `volume 144`; the wheel over the list
  (`wheel:200,250:-1`) prints nothing.

## Not done

XM. FLAC on the workers. A list of tunes. Playing a WAV file where it lies in the ROM (the
card can; a stream keeps one way for every kind). While "Open..." has its window the tune
is held: `ui_choose_file` has the program's events until it is done.

A FLAC subframe is predicted with 32-bit sums whenever its own coefficients allow it
(`flac_bound.py`, applied to the fetched `dr_flac.h` by the build): the sum of the
coefficients' sizes times the largest sample bounds every partial sum, and under 2^31 the
samples are the same. A second of the sample tune went from 3.0 million instructions to 2.2,
its start from 4.9 to 3.5, and the sum of all its samples is `d1c7bfb4` as before.
