#!/usr/bin/env python3
"""Convert an Anki deck (.apkg or .colpkg) into a Study deck for the X4 Pro.

Usage:
  anki_to_deck.py <file.apkg|.colpkg> <out_dir> --name chinese
      [--deck "SpoonfedChinese"] [--fields Hanzi,Pinyin,English] [--card 0]

Writes <out_dir>/<name>.tsv (one card per line, fields in the given order,
first field is the front) and <out_dir>/<name>.srs with the review progress of
card template --card (0 = Anki's "Card 1"), so learned cards keep their
schedule. Copy both to /study/ on the SD card. Media and HTML are dropped.
Needs: pip install zstandard (for new-format collections).
"""
import argparse
import html
import io
import os
import re
import sqlite3
import struct
import sys
import tempfile
import zipfile
from datetime import datetime, timedelta, timezone

BANGKOK = timezone(timedelta(hours=7))
EPOCH = datetime(1970, 1, 1).date()


def open_collection(path):
    z = zipfile.ZipFile(path)
    names = set(z.namelist())
    if "collection.anki21b" in names:
        import zstandard  # newer exports are zstd-compressed

        data = zstandard.ZstdDecompressor().stream_reader(io.BytesIO(z.read("collection.anki21b"))).read()
    elif "collection.anki21" in names:
        data = z.read("collection.anki21")
    else:
        data = z.read("collection.anki2")
    tmp = tempfile.NamedTemporaryFile(suffix=".db", delete=False)
    tmp.write(data)
    tmp.close()
    db = sqlite3.connect(tmp.name)
    db.create_collation("unicase", lambda a, b: (a.lower() > b.lower()) - (a.lower() < b.lower()))
    return db, tmp.name


def clean(text):
    text = re.sub(r"\[sound:[^\]]*\]", "", text)
    text = re.sub(r"<br\s*/?>|</div>|</p>", " ", text, flags=re.I)
    text = re.sub(r"<[^>]+>", "", text)
    text = html.unescape(text).replace("\t", " ").replace("\xa0", " ")
    return re.sub(r"\s+", " ", text).strip()


def day_of(ts):
    """Local (Bangkok) calendar day number since 1970 for a unix timestamp."""
    return (datetime.fromtimestamp(ts, BANGKOK).date() - EPOCH).days


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("out_dir")
    ap.add_argument("--name", required=True)
    ap.add_argument("--deck", default="", help="part of the deck name (default: largest deck)")
    ap.add_argument("--fields", default="", help="note fields in card order, front first")
    ap.add_argument("--card", type=int, default=0, help="card template whose progress to keep")
    args = ap.parse_args()

    db, tmp = open_collection(args.src)
    try:
        decks = db.execute(
            "select d.id, d.name, count(c.id) from decks d left join cards c on c.did = d.id group by d.id"
        ).fetchall()
        matches = [d for d in decks if args.deck.lower() in d[1].lower()]
        if not matches:
            sys.exit(f"no deck matches {args.deck!r}: {[d[1] for d in decks]}")
        did, deck_name, _ = max(matches, key=lambda d: d[2])

        ntid = db.execute(
            "select n.mid from cards c join notes n on n.id = c.nid where c.did = ? "
            "group by n.mid order by count(*) desc limit 1", (did,)).fetchone()[0]
        field_names = [r[0] for r in db.execute("select name from fields where ntid = ? order by ord", (ntid,))]
        wanted = [f.strip() for f in args.fields.split(",")] if args.fields else \
            [f for f in field_names if not re.search("audio|sound|image|picture", f, re.I)]
        missing = [f for f in wanted if f not in field_names]
        if missing:
            sys.exit(f"unknown fields {missing}; note fields are {field_names}")
        index = [field_names.index(f) for f in wanted]

        crt = db.execute("select crt from col").fetchone()[0]
        crt_day = day_of(crt)
        rows = db.execute(
            "select n.id, n.flds, c.type, c.queue, c.due, c.ivl, c.factor, c.reps, c.lapses "
            "from notes n left join cards c on c.nid = n.id and c.ord = ? and c.did = ? "
            "where n.mid = ? and n.id in (select nid from cards where did = ?) order by n.id",
            (args.card, did, ntid, did)).fetchall()

        os.makedirs(args.out_dir, exist_ok=True)
        tsv_path = os.path.join(args.out_dir, args.name + ".tsv")
        srs_path = os.path.join(args.out_dir, args.name + ".srs")
        records = []
        learned = 0
        with open(tsv_path, "w", encoding="utf-8") as tsv:
            tsv.write(f"# {deck_name} (from Anki). Fields: {' <TAB> '.join(wanted)}\n")
            for _, flds, ctype, queue, due, ivl, factor, reps, lapses in rows:
                parts = flds.split("\x1f")
                fields = [clean(parts[i]) if i < len(parts) else "" for i in index]
                if not fields[0]:
                    continue
                tsv.write("\t".join(fields).rstrip("\t") + "\n")

                state = (0, 0, 0, 0, 0)  # new
                if ctype is not None and ctype != 0 and queue is not None and queue >= 0:
                    if ctype == 2:  # review: due is a day number from the collection's creation
                        next_day = crt_day + due
                    elif queue == 1:  # intraday learning: due is a timestamp
                        next_day = day_of(due)
                    else:  # interday learning or relearning: day number
                        next_day = crt_day + due
                    ease = max(130, (factor or 2500) // 10)
                    state = (max(0, min(next_day, 65535)), min(ivl if ctype == 2 else 0, 3650), ease,
                             min(max(reps - lapses, 0), 255), min(lapses, 255))
                    learned += 1
                records.append(state)

        with open(srs_path, "wb") as srs:
            srs.write(b"SRS1" + struct.pack("<HHIHH", 1, 0, len(records), 0, 0))
            for r in records:
                srs.write(struct.pack("<HHHBB", *r))
        print(f"{deck_name}: {len(records)} cards ({learned} with progress) -> {tsv_path}, {srs_path}")
    finally:
        db.close()
        os.unlink(tmp)


if __name__ == "__main__":
    main()
