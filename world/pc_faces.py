# What is printed beside each control and socket of the classroom's computers: a legend each, so
# world/pc.py can put it at its own control's place (world/textures.py draws them, one picture a
# legend). A name: its text and what it is printed on.
LEGEND_PX = 16          # a legend's letters in the atlas
LEGEND_M = 0.0002       # a texel of a legend on the model: letters 3.2 mm high

LEGENDS = {
    # the tower's front
    "power": ("POWER", "front"), "turbo": ("TURBO", "front"), "hdd": ("H.D.D.", "front"), "reset": ("RESET", "front"),
    "mips_all": ("MIPS ALL", "front"), "mips_cpu": ("MIPS CPU", "front"),
    # the tower's back, stamped on a strip of foil
    "keyboard": ("KEYBOARD", "steel"), "com1": ("COM 1", "steel"), "com2": ("COM 2", "steel"), "printer": ("PRINTER", "steel"),
    "volts": ("115/230V", "steel"), "lan": ("10BASE-T", "steel"),
    # the monitor's terminal board
    "rgb1": ("RGB 1", "board"), "rgb2": ("RGB 2", "board"), "r": ("R", "board"), "g": ("G", "board"), "b": ("B", "board"),
    "hcs": ("H/CS", "board"), "v": ("V", "board"), "term": ("75 OHM", "board"), "video": ("VIDEO", "board"),
    "audio_l": ("AUDIO L", "board"), "audio_r": ("AUDIO R", "board"), "speaker": ("EXT SPEAKER 8 OHM", "board"),
    "lp": ("L+", "board"), "lm": ("L-", "board"), "rp": ("R+", "board"), "rm": ("R-", "board"),
    "acin": ("AC IN", "back"),
}
# paper and ink of each ground: the front's plastic, the back's, bare steel, the dark board
TONES = {"front": ((212, 202, 172), (60, 58, 54)), "back": ((189, 181, 158), (50, 48, 44)),
         "steel": ((132, 134, 139), (24, 24, 28)), "board": ((52, 52, 54), (214, 214, 208))}

# A key's cap seen from above, in the layout's units (world/keyboard.json): how far in from its
# rectangle its shoulder and its top are at the sides, behind and before. Both keyboards' caps
# and Keyboard.png (world/textures.py) are of these.
CAP_SHOULDER, CAP_TOP = (4.0, 3.0, 6.0), (6.0, 4.5, 8.0)
CAP_STEP = 1.0 / 6.0    # the shoulder under the top, of the cap's height

# Stickers on the classroom's computers: wide and high in metres, and whether it is round.
STICKERS = {"st_qc": (0.021, 0.021, True), "st_smiley": (0.022, 0.022, True), "st_asset": (0.045, 0.018, False), "st_y2k": (0.036, 0.016, False),
            "st_inside": (0.024, 0.024, False), "st_label": (0.070, 0.054, False), "st_warranty": (0.050, 0.011, False),
            "st_rainbow": (0.060, 0.011, False), "st_property": (0.050, 0.020, False), "st_price": (0.028, 0.018, False),
            "note_0": (0.076, 0.076, False), "note_1": (0.076, 0.076, False), "note_2": (0.076, 0.076, False)}
