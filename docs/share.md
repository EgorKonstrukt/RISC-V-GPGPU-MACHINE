# Seeing each other's machines

In the VRChat world every player runs a machine of their own. A player can put another
player's console and display on the screens in front of them, and, when its owner allows it,
type and point at it. Nothing of the machine itself is shared: only what its screens show.

## What a player does

- The panel between the display and the memory view asks "Whose computer is on these
  screens?": a row for "Automatic", one for "My own", and one for each other player with
  whether their machine is on and may be used, and how many have it on their wall (eight a
  page, "More players" with the page for the rest). Clicking a row picks it; the panel says
  what is shown, how many watch yours, and under the rows who watches the machine shown.
- With another player's machine on the wall, everything of the den is theirs: the console,
  the display, the control panel's read-out with core 0's registers, the wall's speed
  window, the den's tower's lamps.
- Automatic (the start): your own machine while it is on; while it is off, the machine of
  the first player (lowest player number) whose machine is on.
- "Others may use my computer", on the same panel, lets whoever is watching yours type on your console and use
  your display with their keyboards and beams. It starts at no.
- While another player's machine is shown, the keyboards and the beam on the display are
  theirs (if allowed) or nobody's: nothing typed then reaches your own machine.

## How it travels

`EmuShare` is a VRChat player object: one a player, owned by that player, synced by hand
(`RequestSerialization`). `EmuShareHub` is local, one a client: it fills this player's
object. `EmuStreams`, also one a client, takes what the others' objects bring, for every
screen that shows a player: the wall, a classroom tube (`docs/stations.md`), a holodeck's
screen. A player sends console and display while their machine is on and anybody else is in
the world, shown or not, and every client keeps what every sender brings: a screen that
turns to a player has their picture at once (it started from nothing, and took seconds, when
only watched players sent; in a full classroom they all did anyway). It goes out on two
ticks of three (6.7 packets a second) of at most 1,150 bytes: 7.7 KB/s, and the
other fields beside it. VRChat's limit is for everything a client sends, whatever the
object: 11 KB/s on paper, 8 to 10 in practice.

| Synced field | Contents |
|---|---|
| `flags` | bit 0: the machine is on; bit 1: watchers may use it |
| `watching` | the player whose machine this player has on the wall, 0 for their own |
| `seq`, `packet` | a packet's number and its bytes (empty when only the other fields changed) |
| `inputSeq`, `input` | key events for the watched machine: a count and a ring of the last 16 |
| `pointer` | x and y in the watched display's pixels (12 bits each), buttons << 24, present << 28 |
| `station`, `stationMode` | this player's classroom place and what its tube shows |
| `ask` | sixteen words, one for each sender this player keeps: the sender's number, and a count << 16 that goes up with each asking |
| `lost` | beside each: what is asked for again, a packet number's low 16 bits and how many from it << 16; none: everything |
| `holo`, `holding` | the holodeck this player sits in, and whether the controller is in their hands |
| `netSeq`, `net` | the network's packets from this player's machine since the last sending (`docs/lan.md`) |

A packet: 19 bytes (on, the console's first row, cursor column and row, the stream's width
and height, the display's width and height, the number of rows, the machine's speed:
tenths of a million instructions a second for all cores and for core 0, two bytes each, and
the length of a status), the status if any, the console rows, then display tiles to the end.

The status goes every two seconds while the machine is on (`PutStatus`): sixteen bits of
activity (`EmuMachine.activity`: one an eighth of a second, set where core 0's registers had
changed, which is the red disk lamp's flicker on every tower), pc and x1 to x31 as the last
readback had them, and the control panel's text (`statsLine`, at most 560 characters). It is
not kept for repairs: the next one comes anyway.

- Everything in a packet is state, not a difference: a row or a tile replaces what the
  receiver had.
- **A slot a sender.** `EmuStreams` has sixteen slots: a store of tiles, a picture, a console.
  Every other player whose machine is on has one while slots last, and is decoded once
  however many screens show it. Each screen says whose it wants (`SetWant`): a sender a
  screen wants and that has no slot takes one from a sender no screen shows. A slot whose
  sender left, or is off and not shown, is emptied and its picture made black.
- **A sender knows who keeps it** from the others' `ask`: a word that names it. A new word is
  a new receiver, and the sender sends everything (at most once in five seconds, whoever
  asks).
- Who watches whom is `watching` alone: the rows' counts and the names under them are
  counted from it (`Watchers`, `WatcherNames` in `EmuShareHub`).
- **A lost packet is repaired, not the whole picture.** A receiver that sees a gap in `seq`
  names the missing numbers in `lost` and counts its word up. The sender keeps what each of
  its last 32 packets carried (tiles and console rows) and marks only those as not sent. A
  gap older than that is everything again.
- A slot whose sender is on and has brought nothing two seconds after it was made asks for
  everything, and again every five seconds: so a screen never waits for its owner's picture
  to change.
- A late joiner gets the last packet from VRChat, makes its slots, and is a new screen.
- A player's object is destroyed with the player. The hub takes it off its list in
  `OnPlayerLeft` and again (`Prune`) before anything reads the list: reading a destroyed
  object halts an Udon behaviour for the rest of the session, which is what froze the
  classroom's tubes in October 2026.
- `EmuStreams` writes a line a minute to VRChat's log (`[Streams]`): packets, gaps, repairs,
  refreshes, and each slot's sender, last packet and how long ago it was heard.

## Console

`EmuTerminal` notes which rows of its grid were written. A row travels as: row number,
length without the blanks at its end, the characters, then 0, or 1 and a byte of colours a
cell. A typed character is one short row; a full screen is about 1.2 KB.

## What the display stream is, in short

- There are no differences between frames. A tile is always sent whole, coded from the
  current picture alone, and replaces what the receiver had: losing a packet costs
  sharpness for a moment, never a wrong picture.
- What is saved between frames is whole tiles: one that did not change is not sent.
- The GPU decides which tiles changed and codes every tile at all three sizes, every time.
- Udon decides which tiles and which size go into a packet, and copies their bytes.
- The stream is the display at half size when the display is larger than 640x480. A tile
  that holds still ends at the fine level: the display's own pixels.
- The stream's three sizes are not compressed further. The fine level leaves out colours
  that repeat.

## Display

Udon is too slow to touch pixels, so the GPU does all of it (`ShareEncode.shader`) and Udon
only copies bytes.

- Pass 0 draws the decoded display at the stream's size: the display's own size up to
  640x480, else divided by the smallest whole number that fits it in.
- Pass 1 packs that into the first 300 rows of a 128x1500 target, a row for each 32x32 tile (20 across). A row
  holds the tile three times: 64 blocks of 4x4 pixels (384 bytes), 16 blocks of 8x8 (96),
  4 blocks of 16x16 (24). A block is six bytes: two colours (5:6:5 each) and a bit a sample
  saying which; the colours are the means of the samples darker and lighter than the block's
  mean. The larger blocks are cut from the capture's mipmaps.
- The row's last texels say whether the tile differs from the capture before (the last two
  captures are kept), and the coarsest size that still shows it exactly: 2 when its four
  quarters are each one colour.
- The target is read back (768,000 bytes). For each packet Udon picks tiles: those that
  changed, as sharp as two packets can carry all of them; when none is left, the coarsest of
  the rest one size sharper, never sharper than a flat tile needs. So a typed character
  arrives sharp at once, a moving picture arrives coarse at whatever rate fits, and a still
  one sharpens over a few seconds.
- Where the stream is half the display's size there is a fine level: the display's own
  pixels, a row of the target (from row 300) for each quarter of a tile, 32x32 pixels as 64
  blocks of 4x4. Two bits a block say what follows for it: nothing, when it is of one colour
  and that is the colour of the last such block; one colour (2 bytes); 16 bits, when its two
  colours are those of the last block that had two; or two colours and 16 bits (6 bytes). A
  quarter is 16 to 400 bytes, about 200 on a desktop: half of what the blocks alone would be.
  Only tiles that changed are coded again (passes 2 to 4: which tiles changed, in the
  display's own pixels; their blocks; their rows).
- Room that the changed tiles leave in a packet goes to sharpening: first the coarsest of
  the rest one size sharper, then quarters at the fine level of tiles that are at the
  stream's sharpest. A still 1280x720 desktop sent from nothing is at the stream's sharpest after 15 s and all there after 43 s (281 KB), at 36.7 dB
  from the display; the display at half size, however exact, is 30.4 dB from it there (21 to 29 dB on other screens).
- The receiver copies each tile into the same layout (a slot's store, with the size it came
  at in the row's last texel) and `ShareDecode.shader` draws the picture from it into the
  slot's picture (1280 x 960), which the wall shows in place of the machine's own: two texels
  a stream pixel where there is a fine level, and only the tiles the last packet brought
  (each row has the packet's stamp).

## Units

How the display is sent: `units` on the hub ("Share" in the scene) is on, and may be changed
while running; off, the tiles above are sent. The GPU makes the whole of a packet's display
part (`ShareUnits.shader`); Udon copies one row of bytes. A packet says which it is in bit 1
of its first byte, and a receiver takes either. "ShaderEmu/Add the units stream to the open
scene" makes its textures and fills the hub's and the receiver's fields. Watched in the editor
on 12 October 2026, as it first was, it was much worse than the tiles (a packet's first bytes
were written over by the next one's header, and a packet waited a second for the link with
the picture it was made of); what it is judged by since is below, "Judged while it moves".

- The picture is the display's own pixels (divided by a whole number when it is over 1280 x
  960), cut into blocks of four sizes: 4x4 samples of 1, 2, 4 or 8 pixels (levels 0 to 3),
  two colours (5:6:5) and a bit a sample, as a tile's blocks are. Nothing is on the tiles' grid.
- A unit is 4x4 blocks of one level, on that level's own grid, with a bit a block that is
  there: 16, 32, 64 or 128 pixels square. Its record: two bytes of level and number, two of
  bits, two bits a block of kinds, then the blocks, repeated colours left out as in a quarter.
- The sender keeps, in a texture, for each 8x8 cell the level the receivers have it at (4:
  stale) and the number of the packet that carried it. A cell that changed since the last
  capture is stale. A cell needs the coarsest level whose block shows every pixel as level 0
  does: a flat area goes as a few large blocks, text at level 0.
- A packet: stale cells first, as sharp as one packet can carry all of them (what keeps
  changing is then whole in every packet, if coarse; with two packets' worth a window being
  dragged was half a sending behind), then the cells that are coarser than they need, as sharp
  as two can. A cell goes by the coarsest block that spoils nothing
  beside it. Units go in an order (stale before the rest, coarse before sharp, each level from
  a place that moves with every packet) and the packet is that order's start, cut where the
  room ends: which units go is a sum, so the GPU can pick them.
- The passes: blocks, exact, need, marks, stats, plan, units, prefix, packet, state. They cost
  0.36 ms a packet together in `share_test --time`.
- A packet is made a tick ahead: its header and console rows when the GPU is asked, its units
  when they are read back (`AskUnits`, `TakeUnits` in `EmuShareHub`). While the link is busy,
  what is ready and two ticks old is made again of the display as it is then (`UnitsWait`): a
  packet that waits a second for the link is not a second old when it goes.
- A lost packet: the cells that carry its number go stale in the next packet made. Everything
  again: all of them do.
- The receiver keeps no store: a pass finds each unit's record in the packet's bytes, a second
  draws the pixels those units cover into the slot's picture, the sharpest unit where two
  overlap (`ShareUnitsDecode.shader`).
- `tools/share_units.py` is the same in numpy, pass for pass, and `bin\share_test.exe` runs the
  two shaders outside Unity on pictures of a display: `share_test A.png B.png --out FOLDER`,
  then `python tools/share_units.py A.png B.png --against FOLDER` must say every packet has the
  same bytes, the same state after it and the same picture (`--lost-at N` for a lost packet).
  On 12 October 2026: 1,140 packets of six runs, all the same (the picture after 48 of them).
- In a model of both senders on the desktop at 800 x 600 (not in Unity): a letter typed, 4
  packets and 3,766 bytes as tiles, 1 and 372 as units; the editor opening, 54 packets against
  15; the browser opening, 91 against 38.

## Judged while it moves

The stream is right when the picture a receiver shows is close to the display while the
display keeps changing: a window dragged, the pointer, windows with activity, text scrolling,
a game. Bytes and the time to a settled picture do not say that.

- `rvc_harness --cpu --cpu-ips 3000000 --desktop ... --screen-log SCENE.scr --screen-every 33
  --screen-frames 300` records the guest's display at the world's speed (`docs/cpu-harness.md`).
- `python tools/share_motion.py SCENE.scr ... [--video FOLDER]` plays a recording through a
  model of each sender on the recording's own clock, a packet every 0.15 s (VRChat) and every
  second (the editor), and prints the mean difference (levels of 255) between what the
  receiver shows and the frame current at each moment, over the pixels that change at all.
  `--video` writes what each receiver showed as .mp4 files.
- Five recordings of 300 frames, 12 October 2026, tiles then units; lower is better:

| Scene | of the screen changes | every 0.15 s | every second |
|---|---|---|---|
| a window dragged about | 77% | 23.7, 23.1 | 48.3, 42.2 |
| the pointer over two windows | 1.7% | 2.56, 2.56 | 3.03, 2.85 |
| glxgears and the system monitor | 5.5% | 28.2, 5.8 | 30.0, 16.6 |
| a terminal whose text scrolls | 24% | 13.2, 8.8 | 13.9, 10.8 |
| Doom's first demo | 66% | 16.9, 13.7 | 34.1, 19.7 |

- In Unity itself (the editor, `selfTest`, the receiver's picture read back four times a second
  against the display, 12 s each, tiles then units): glxgears 7.0 and 5.4 at VRChat's rate
  (the hub's `sending` cleared every frame), 9.4 and 6.1 at the editor's; Doom 15.2 and 12.3,
  27.4 and 17.8. Over only the pixels that had just changed the two are level (28 and 28, 30
  and 29, 19 and 18, 25 and 25).
- Not measured: two real clients, and a lost packet's repair in Unity (the shader's side of it
  is in `share_test --lost-at`).

## Using the watched machine

The hub takes what the keyboards queued and the beam's place on the display, and sends them
in its own object; the owner's hub hands them to `EmuMachine` (`RemoteChar`, `RemoteKey`,
`SetRemotePointer`), which reads them beside its own keyboards. When both the owner's
beam and a watcher's pointer are on the display, the one holding a button has the machine,
else the one that moved last: a desktop player's beam is the middle of their view, so it is
on the display whenever they look at it. A pointer is repeated twice a second while it is on
the display and dropped by the owner after two seconds of silence.

## Testing

- `selfTest` on the hub ("Share" in the scene) shows this player's own machine through the
  stream: set it in play mode with `SetProgramVariable`. Compare the slot's picture
  (`StreamPicture0`) with `ShareCaptureA`/`B`, and its cells with the terminal's `grid`.
- Several players with one client: `ClientSimMain.SpawnRemotePlayer`, and an editor hook
  that gives each new `EmuShare` a `station`, copies this player's `seq` and `packet` into
  it when `seq` changes, mirrors this player's own `ask` and `lost` words back (with this
  player's number in place of the sender's) and sends it `_onDeserialization`. `dropTest`
  on `EmuStreams` loses every tenth packet: its `repairs` and the hub's `repaired` must go
  up and the hub's `refreshed` must not.
- Destroy a spawned player's "Share player" object before its `OnPlayerLeft`: no behaviour
  may halt (`enabled` stays true), and the place reads FREE with a black picture.
- ClientSim never raises `OnPostSerialization` for these objects, so `EmuShare.Busy()` runs
  out its one-second limit there: a packet a second in the editor, six or seven in VRChat.
- Two real clients: set the hub's `netTest` in the scene, set the SDK's number of clients
  to 2 and "Build & Test". The first player in the instance turns their machine on and
  allows use; the second watches it by itself and, 45 s later, types `echo NET-2-OK` at it.
  Every client writes a `[ShareTest]` line every two seconds to its own
  `LocalLow/VRChat/VRChat/output_log_*.txt`: packets and bytes sent and received, gaps,
  tiles at each size, and the `NET-` lines on the console it shows. Nothing has to be fed
  in or read off the screen. Turn `netTest` off again before saving the scene.
- That run (2026-10-07, both clients on one PC): 133 packets, 143,881 bytes in 27 s, about
  5 a second and 5.3 KB/s, none lost, never clogged; every tile had arrived coarse 4 s after
  the second client started watching, and the typed line came back in the stream.
