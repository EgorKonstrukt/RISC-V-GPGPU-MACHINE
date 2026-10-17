"""A cartridge of our own for checking the 6502 (docs/games/nes.md, "Testing"): mapper 0, 16 KB,
pattern RAM. Its program never waits for long: every turn it reads the PPU's status through
a pointer and across a page, pushes the flags, and folds what it read into RAM; now and then
it writes the scroll, counts a delay down and waits for the interrupt. So its RAM sums are
the interpreter's (NES_CPU=interp, NES_PLAIN=1) only if every cycle and line is.

    python linux/nes/exact_rom.py OUT.nes [VARIANT]

VARIANT: 0 to 2 move sprite 0 down by sevens, and 2 pulls the flags from data; 3 is 1 with no
delay, 4 is 1 with no wait.
"""
import sys

ORG = 0xC000
code = bytearray()
labels = {}
fixes = []   # (offset, label, kind)


def here():
    return ORG + len(code)


def label(name):
    labels[name] = here()


def op(*b):
    code.extend(b)


def absolute(opcode, name):
    code.append(opcode)
    fixes.append((len(code), name, 'abs'))
    code.extend((0, 0))


def branch(opcode, name):
    code.append(opcode)
    fixes.append((len(code), name, 'rel'))
    code.append(0)


variant = int(sys.argv[2]) if len(sys.argv) > 2 else 0

# ---- reset
label('reset')
op(0x78, 0xD8, 0xA2, 0xFF, 0x9A)            # sei cld ldx #$ff txs
op(0xA9, 0x00, 0x8D, 0x00, 0x20, 0x8D, 0x01, 0x20)   # lda #0 sta $2000 sta $2001
for _ in range(2):                          # two blankings waited for
    label('w%d' % len(code))
    op(0x2C, 0x02, 0x20)                    # bit $2002
    op(0x10, 0xFB)                          # bpl back
# RAM cleared
op(0xA9, 0x00, 0xAA)                        # lda #0 tax
label('clear')
op(0x95, 0x00, 0x9D, 0x00, 0x02, 0x9D, 0x00, 0x03, 0xE8)   # sta 0,x sta $200,x sta $300,x inx
branch(0xD0, 'clear')
# a solid tile 1 in pattern RAM, and tile 1 over the first name table
op(0xA9, 0x00, 0x8D, 0x06, 0x20, 0xA9, 0x10, 0x8D, 0x06, 0x20)   # address $0010
op(0xA2, 0x10, 0xA9, 0xFF)                  # ldx #16 lda #$ff
label('tile')
op(0x8D, 0x07, 0x20, 0xCA)                  # sta $2007 dex
branch(0xD0, 'tile')
op(0xA9, 0x20, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20)   # address $2000
op(0xA0, 0x04, 0xA2, 0x00, 0xA9, 0x01)      # ldy #4 ldx #0 lda #1
label('names')
op(0x8D, 0x07, 0x20, 0xCA)                  # sta $2007 dex
branch(0xD0, 'names')
op(0x88)                                    # dey
branch(0xD0, 'names')
# a palette
op(0xA9, 0x3F, 0x8D, 0x06, 0x20, 0xA9, 0x00, 0x8D, 0x06, 0x20)
op(0xA2, 0x20)
label('colours')
op(0x8A, 0x8D, 0x07, 0x20, 0xCA)            # txa sta $2007 dex
branch(0xD0, 'colours')
# sprite 0: tile 1 at line 99 + variant * 7, x 77
op(0xA9, 99 + (1 if variant in (3, 4) else variant) * 7, 0x8D, 0x00, 0x02, 0xA9, 0x01, 0x8D, 0x01, 0x02)
op(0xA9, 0x00, 0x8D, 0x02, 0x02, 0xA9, 77, 0x8D, 0x03, 0x02)
# pointers: $10 to $2002, $12 to $20FD (y 3 crosses into $2100), $14 into RAM
op(0xA9, 0x02, 0x85, 0x10, 0xA9, 0x20, 0x85, 0x11)
op(0xA9, 0xFD, 0x85, 0x12, 0xA9, 0x20, 0x85, 0x13)
op(0xA9, 0x40, 0x85, 0x14, 0xA9, 0x03, 0x85, 0x15)
op(0xA9, 0x00, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20)   # scroll 0 0
op(0xA9, 0x80, 0x8D, 0x00, 0x20)            # interrupts at the blanking
op(0xA9, 0x1E, 0x8D, 0x01, 0x20)            # background and sprites
op(0xA2, 0x00, 0xA0, 0x00)

# ---- the loop: nothing in it waits. No read comes to the data register: Nofrendo reads
# memory it does not have when the address there is what a picture left.
label('loop')
op(0xE6, 0x2C)                              # inc $2c: the turn
op(0xA5, 0x2C, 0x29, 0x03, 0x85, 0x29)      # $29 = turn & 3
op(0xA5, 0x2C, 0x4A, 0x4A, 0x29, 0x01, 0x85, 0x2A, 0x0A, 0x65, 0x2A, 0x85, 0x2A)   # $2a = 0 or 3
op(0xA5, 0x2C, 0x29, 0x08, 0x85, 0x2B)      # $2b = 0 or 8
op(0xA4, 0x29, 0xB1, 0x10)                  # ldy $29 lda ($10),y: $2002 to $2005
op(0x45, 0x20, 0x85, 0x20)                  # eor $20 sta $20
op(0xA6, 0x2B, 0xBD, 0xFA, 0x20)            # ldx $2b lda $20FA,x: the status, a page crossed when x is 8
op(0x18, 0x65, 0x21, 0x85, 0x21)            # clc adc $21 sta $21
op(0x08, 0x68, 0x45, 0x22, 0x85, 0x22)      # php pla eor $22 sta $22
op(0xA4, 0x2A, 0xB1, 0x12)                  # ldy $2a lda ($12),y: $20FD, or $2100 with a page crossed
op(0x65, 0x23, 0x85, 0x23)                  # adc $23 sta $23
op(0xA4, 0x29)                              # ldy $29
absolute(0x20, 'mix')                       # jsr mix
op(0xA5, 0x2C, 0x29, 0x3F)                  # lda $2c and #63
branch(0xD0, 'no_write')
op(0xA5, 0x20, 0x29, 0xF8, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20)   # a scroll written from what was read
op(0xA5, 0x21, 0x09, 0x18, 0x29, 0x1E, 0x8D, 0x01, 0x20)         # and the mask, some of it on
label('no_write')
op(0xA5, 0x2C, 0x29, 0x7F, 0xC9, 0xFF if variant == 3 else 0x55)      # lda $2c and #127 cmp #$55
branch(0xD0, 'no_delay')
op(0xA6, 0x23, 0xE8)                        # ldx $23 inx: a delay of what was read
label('delay')
op(0xCA)                                    # dex
branch(0xD0, 'delay')
label('no_delay')
op(0xA5, 0x2C, 0xC9, 0xF0)                  # lda $2c cmp #$f0
branch(0xD0, 'no_idle')
if variant == 4:
    absolute(0x4C, 'no_idle')
op(0xA5, 0x30)                              # lda $30: until the next interrupt, once in 256 turns
label('idle')
op(0xC5, 0x30)                              # cmp $30
branch(0xF0, 'idle')
label('no_idle')
absolute(0x4C, 'loop')

label('mix')
op(0x48, 0xA5, 0x20, 0x0A, 0x26, 0x25, 0x4A, 0x4A, 0x66, 0x26)   # pha lda $20 asl rol $25 lsr lsr ror $26
op(0x38, 0xE5, 0x21, 0x91, 0x14)            # sec sbc $21 sta ($14),y
op(0x28 if variant == 2 else 0xEA)          # (plp of what pha left, in one variant: flags from data)
op(0x48 if variant == 2 else 0xEA)
op(0x68, 0x60)                              # pla rts

# ---- the interrupt at the blanking: the sprites copied, the scroll set, a count
label('nmi')
op(0x48, 0x8A, 0x48)                        # pha txa pha
op(0xA9, 0x00, 0x8D, 0x03, 0x20, 0xA9, 0x02, 0x8D, 0x14, 0x40)   # sprites from $0200
op(0xAD, 0x02, 0x20)                        # lda $2002
op(0x45, 0x27, 0x85, 0x27)                  # eor $27 sta $27
op(0xA9, 0x00, 0x8D, 0x05, 0x20, 0x8D, 0x05, 0x20)
op(0xA9, 0x80, 0x8D, 0x00, 0x20)
op(0xE6, 0x30)                              # inc $30
op(0xAD, 0x16, 0x40, 0x45, 0x28, 0x85, 0x28)   # lda $4016 eor $28 sta $28
op(0x68, 0xAA, 0x68, 0x40)                  # pla tax pla rti
label('irq')
op(0x40)

for at, name, kind in fixes:
    target = labels[name]
    if kind == 'abs':
        code[at] = target & 0xFF
        code[at + 1] = target >> 8
    else:
        off = target - (ORG + at + 1)
        assert -128 <= off <= 127, name
        code[at] = off & 0xFF
# the two branches back of the waits for a blanking are relative to themselves already
prg = bytearray([0xFF]) * 0x4000
prg[:len(code)] = code
for i, name in enumerate(('nmi', 'reset', 'irq')):
    prg[0x3FFA + 2 * i] = labels[name] & 0xFF
    prg[0x3FFB + 2 * i] = labels[name] >> 8
header = bytes([0x4E, 0x45, 0x53, 0x1A, 1, 0, 0, 0]) + bytes(8)
open(sys.argv[1], 'wb').write(header + bytes(prg))
print('%d bytes of code, variant %d' % (len(code), variant))
