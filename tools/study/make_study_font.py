#!/usr/bin/env python3
"""Build /study/font.ttf: a small CJK font for the Study decks.

Subsets Noto Sans CJK SC (SIL OFL 1.1) to Latin, pinyin tone letters,
CJK punctuation and the 3,755 most common Simplified characters
(GB2312 level 1), which covers HSK 1-6.

Usage: make_study_font.py <NotoSansCJK-Regular.ttc> <out.ttf> [deck.tsv ...]
Characters used in the given decks are always included.

Outlines are converted from CFF to TrueType (quadratic): the firmware's font
engine is built without the CFF driver.
"""
import sys
from fontTools.ttLib import TTCollection
from fontTools import subset
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import newTable

MAX_ERR = 1.0  # font units, for cubic to quadratic conversion


def cff_to_truetype(font):
    """Replace the CFF table with glyf/loca (standard otf2ttf recipe)."""
    glyph_order = font.getGlyphOrder()
    glyph_set = font.getGlyphSet()
    glyf = newTable("glyf")
    glyf.glyphOrder = glyph_order
    glyf.glyphs = {}
    for name in glyph_order:
        pen = TTGlyphPen(glyph_set)
        glyph_set[name].draw(Cu2QuPen(pen, MAX_ERR, reverse_direction=True))
        glyf[name] = pen.glyph()
    font["glyf"] = glyf
    font["loca"] = newTable("loca")
    font["maxp"] = newTable("maxp")
    maxp = font["maxp"]
    maxp.tableVersion = 0x00010000
    for attr in ("maxZones", "maxTwilightPoints", "maxStorage", "maxFunctionDefs", "maxInstructionDefs",
                 "maxStackElements", "maxSizeOfInstructions", "maxComponentElements"):
        setattr(maxp, attr, 0)
    maxp.maxZones = 1
    maxp.numGlyphs = len(glyph_order)
    del font["CFF "]
    if "VORG" in font:
        del font["VORG"]
    font["post"].formatType = 2.0
    font["post"].extraNames = []
    font["post"].mapping = {}
    font["post"].glyphOrder = glyph_order
    font["head"].glyphDataFormat = 0
    font.sfntVersion = "\x00\x01\x00\x00"
    font.recalcBBoxes = True


def gb2312_level1():
    chars = []
    for row in range(0xB0, 0xD8):  # rows 16-55
        for col in range(0xA1, 0xFF):
            try:
                chars.append(bytes([row, col]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    src, out, decks = sys.argv[1], sys.argv[2], sys.argv[3:]
    cps = set(range(0x20, 0x7F)) | set(range(0xA0, 0x180))  # Latin + Latin Ext-A
    cps |= {0x01CE, 0x01D0, 0x01D2, 0x01D4, 0x01D6, 0x01D8, 0x01DA, 0x01DC}  # ǎ ǐ ǒ ǔ ǖ ǘ ǚ ǜ
    cps |= set(range(0x3000, 0x3020)) | set(range(0xFF01, 0xFF5F))  # CJK + full-width punctuation
    cps |= {ord(c) for c in gb2312_level1()}
    for deck in decks:
        with open(deck, encoding="utf-8") as f:
            cps |= {ord(c) for c in f.read() if ord(c) >= 0x2E80}

    collection = TTCollection(src)
    font = next(f for f in collection.fonts if "SC" in f["name"].getDebugName(1))
    options = subset.Options()
    options.layout_features = ["kern", "vert"]
    options.name_IDs = ["*"]
    options.notdef_outline = True
    options.desubroutinize = False
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=cps)
    subsetter.subset(font)
    cff_to_truetype(font)
    font.save(out)
    print(f"{out}: {len(cps)} code points")


if __name__ == "__main__":
    main()
