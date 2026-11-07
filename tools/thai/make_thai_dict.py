#!/usr/bin/env python3
"""Build /thai/words.dic, the Thai word list the firmware uses for line breaks.

Usage: make_thai_dict.py <words.txt> [more.txt ...] <out.dic>

Input: one word per line, UTF-8 (for example PyThaiNLP's words_th.txt, CC0).
Only words made entirely of Thai letters are kept. Output layout:
  "THD1" | u32 count | u32 offsets[count + 1] | words in TIS-620, sorted
"""
import struct
import sys


def to_tis(word):
    out = bytearray()
    for ch in word:
        cp = ord(ch)
        if not 0x0E01 <= cp <= 0x0E5B:
            return None
        out.append(cp - 0x0E00 + 0xA0)
    return bytes(out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    words = set()
    for path in sys.argv[1:-1]:
        with open(path, encoding="utf-8") as f:
            for line in f:
                tis = to_tis(line.strip())
                if tis and len(tis) >= 2:
                    words.add(tis)
    ordered = sorted(words)
    offsets, pos = [], 0
    for w in ordered:
        offsets.append(pos)
        pos += len(w)
    offsets.append(pos)
    with open(sys.argv[-1], "wb") as f:
        f.write(b"THD1")
        f.write(struct.pack("<I", len(ordered)))
        f.write(struct.pack(f"<{len(offsets)}I", *offsets))
        f.write(b"".join(ordered))
    print(f"{sys.argv[-1]}: {len(ordered)} words, {pos} bytes of text")


if __name__ == "__main__":
    main()
