#!/usr/bin/env python3
"""Turn one Compound brief (JSON from the Compound app) into an EPUB for the
X4 Pro, and update the feed index the device syncs from.

Usage: compound_epub.py <brief.json> <doc_id> <feed_dir> [--no-thai] [--keep N]

Writes <feed_dir>/<doc_id>.epub and rewrites <feed_dir>/index.txt, one line
per brief, newest first, tab separated:
  <doc_id>.epub, type, date, title, created_at, content hash
The device re-downloads a brief when its hash changes. Only the newest N
briefs (default 30) are kept; older EPUBs are deleted. Thai translations are
included unless --no-thai is given (they need a Thai reader font such as
Sarabun on the device).
"""
import hashlib
import html
import json
import os
import sys
import uuid
import zipfile

INDEX = "index.txt"


def sort_key(row):
    """created_at in UTC (briefs mix +00:00 and +07:00); falls back to the date."""
    from datetime import datetime, timezone
    try:
        t = datetime.fromisoformat(row[4].replace("Z", "+00:00"))
        if t.tzinfo is None:
            t = t.replace(tzinfo=timezone.utc)
        return t.astimezone(timezone.utc).isoformat()
    except (ValueError, IndexError):
        return (row[2] or "") + "T00:00:00+00:00"
FIXED_TIME = (1980, 1, 1, 0, 0, 0)


def esc(text):
    return html.escape(str(text or ""), quote=True)


def para(text, cls=None):
    if not text:
        return ""
    attr = f' class="{cls}"' if cls else ""
    return f"<p{attr}>{esc(text)}</p>\n"


def body_for(b, thai):
    is_news = b.get("type") == "news"
    out = []
    eyebrow = "Morning market brief" if is_news else (b.get("category") or "Idea")
    out.append(f'<p class="eyebrow">{esc(eyebrow)} · {esc(b.get("date", ""))}</p>\n')
    out.append(f"<h1>{esc(b.get('title_en'))}</h1>\n")
    if thai:
        out.append(para(b.get("title_th"), "th"))
    if b.get("hook_en"):
        out.append(para(b["hook_en"], "dek"))

    core = b.get("core_idea") or {}
    if core.get("en"):
        out.append('<div class="core"><p class="label">Core idea</p>\n')
        out.append(para(core["en"]))
        if thai:
            out.append(para(core.get("th"), "th"))
        out.append("</div>\n")

    snap = b.get("snapshot") or []
    if snap:
        out.append("<h2>Snapshot</h2>\n<table>\n")
        for s in snap:
            out.append(f"<tr><td>{esc(s.get('label'))}</td><td>{esc(s.get('value'))}</td>"
                       f"<td>{esc(s.get('change'))}</td></tr>\n")
        out.append("</table>\n")
        out.append(para(b.get("snapshot_note"), "note"))

    for i, s in enumerate(b.get("sections") or []):
        label = s.get("framework")
        if label:
            out.append(f'<p class="eyebrow">{esc(label)}</p>\n')
        out.append(f"<h2>{esc(s.get('headline_en'))}</h2>\n")
        if thai:
            out.append(para(s.get("headline_th"), "th"))
        out.append(para(s.get("en")))
        if thai:
            out.append(para(s.get("th"), "th"))
        if s.get("impact_en"):
            out.append(f'<p class="impact"><b>Why it matters:</b> {esc(s["impact_en"])}</p>\n')
            if thai:
                out.append(para(s.get("impact_th"), "th"))

    terms = b.get("terms") or []
    if terms:
        out.append("<h2>Key terms</h2>\n")
        for t in terms:
            th = f" ({esc(t.get('th'))})" if thai and t.get("th") else ""
            out.append(f'<p class="term"><b>{esc(t.get("en"))}</b>{th}: {esc(t.get("explain_en") or t.get("explain"))}</p>\n')
            if t.get("example_en"):
                out.append(para(t["example_en"], "example"))

    if b.get("followups"):
        out.append("<h2>Think about it</h2>\n")
        for q in b["followups"]:
            out.append(para(q))

    if b.get("sources"):
        out.append("<h2>Sources</h2>\n")
        for s in b["sources"]:
            out.append(para(s.get("title") if isinstance(s, dict) else s, "source"))
    return "".join(out)


CSS = """body{margin:0 4%;line-height:1.45}
h1{font-size:1.5em;margin:.2em 0 .4em}
h2{font-size:1.15em;margin:1.2em 0 .3em}
.eyebrow{font-size:.75em;text-transform:uppercase;letter-spacing:.06em;margin:1.2em 0 0}
.dek{font-style:italic}
.core{border-left:3px solid #000;padding-left:.8em;margin:1em 0}
.core p,.term,.example{text-align:left}
.label{font-weight:bold;font-size:.8em;text-transform:uppercase;margin:0}
.impact{font-size:.95em}
.example{font-style:italic;font-size:.9em;margin-top:0}
.note,.source{font-size:.8em}
table{border-collapse:collapse;width:100%;font-size:.9em}
td{padding:.15em .3em;border-bottom:1px solid #999}
"""


def build_epub(b, doc_id, path, thai):
    title = b.get("title_en") or doc_id
    book_id = "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, "compound/" + doc_id))
    # Derived from the brief, so rebuilding an unchanged brief gives identical bytes.
    modified = (str(b.get("date") or "1980-01-01")[:10]) + "T00:00:00Z"
    xhtml = ('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n'
             '<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en"><head>'
             f'<title>{esc(title)}</title><link rel="stylesheet" type="text/css" href="style.css"/></head>'
             f"<body>\n{body_for(b, thai)}</body></html>\n")
    opf = f"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="id">{book_id}</dc:identifier>
<dc:title>{esc(title)}</dc:title>
<dc:creator>Compound</dc:creator>
<dc:language>en</dc:language>
<dc:date>{esc(b.get('date', ''))}</dc:date>
<meta property="dcterms:modified">{modified}</meta>
</metadata>
<manifest>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
<item id="css" href="style.css" media-type="text/css"/>
<item id="brief" href="brief.xhtml" media-type="application/xhtml+xml"/>
</manifest>
<spine toc="ncx"><itemref idref="brief"/></spine>
</package>
"""
    nav = ('<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n'
           '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head>'
           f'<body><nav epub:type="toc"><ol><li><a href="brief.xhtml">{esc(title)}</a></li></ol></nav></body></html>\n')
    ncx = f"""<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
<head><meta name="dtb:uid" content="{book_id}"/></head>
<docTitle><text>{esc(title)}</text></docTitle>
<navMap><navPoint id="p1" playOrder="1"><navLabel><text>{esc(title)}</text></navLabel><content src="brief.xhtml"/></navPoint></navMap>
</ncx>
"""
    container = ('<?xml version="1.0" encoding="utf-8"?>\n'
                 '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
                 '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
                 "</rootfiles></container>\n")
    tmp = path + ".tmp"
    with zipfile.ZipFile(tmp, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype", FIXED_TIME), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        for name, data in [("META-INF/container.xml", container), ("OEBPS/content.opf", opf),
                           ("OEBPS/nav.xhtml", nav), ("OEBPS/toc.ncx", ncx), ("OEBPS/style.css", CSS),
                           ("OEBPS/brief.xhtml", xhtml)]:
            z.writestr(zipfile.ZipInfo(name, FIXED_TIME), data, compress_type=zipfile.ZIP_DEFLATED)
    os.replace(tmp, path)


def clean(text):
    return " ".join(str(text or "").split())


def build_cb(b, path):
    """Plain text brief for the native Compound screen on the device.

    One record per line, fields separated by TAB:
      TYPE, DATE, TITLE en th, HOOK, NOTE, M label value change note,
      CORE en th, S framework headline_en headline_th, EN, TH, IMP en th,
      W term th explain example, Q followup.
    """
    out = []
    def rec(*fields):
        out.append("\t".join(clean(f) for f in fields))
    rec("TYPE", b.get("type", ""))
    rec("DATE", b.get("date", ""))
    rec("TITLE", b.get("title_en", ""), b.get("title_th", ""))
    if b.get("hook_en"):
        rec("HOOK", b["hook_en"])
    if b.get("snapshot_note"):
        rec("NOTE", b["snapshot_note"])
    for m in b.get("snapshot") or []:
        rec("M", m.get("label", ""), m.get("value", ""), m.get("change", ""), m.get("note", ""))
    core = b.get("core_idea") or {}
    if core.get("en"):
        rec("CORE", core.get("en", ""), core.get("th", ""))
    for sec in b.get("sections") or []:
        rec("S", sec.get("framework", ""), sec.get("headline_en", ""), sec.get("headline_th", ""))
        if sec.get("en"):
            rec("EN", sec["en"])
        if sec.get("th"):
            rec("TH", sec["th"])
        if sec.get("impact_en"):
            rec("IMP", sec["impact_en"], sec.get("impact_th", ""))
    for t in b.get("terms") or []:
        rec("W", t.get("en", ""), t.get("th", ""), t.get("explain_en", ""), t.get("example_en", ""))
    for q in b.get("followups") or []:
        rec("Q", q)
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def read_index(feed_dir):
    rows = []
    p = os.path.join(feed_dir, INDEX)
    if os.path.exists(p):
        with open(p, encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 4 and not line.startswith("#"):
                    rows.append(parts[:6] + [""] * (6 - len(parts[:6])))
    return rows


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    thai = "--no-thai" not in sys.argv
    keep = 30
    if "--keep" in sys.argv:
        keep = int(sys.argv[sys.argv.index("--keep") + 1])
        args = [a for a in args if a != str(keep)]
    if len(args) != 3:
        sys.exit(__doc__)
    src, doc_id, feed_dir = args
    with open(src, encoding="utf-8") as f:
        b = json.load(f)
    if "data" in b and isinstance(b["data"], dict) and "title_en" in b["data"]:
        b = b["data"]
    os.makedirs(feed_dir, exist_ok=True)
    epub_name = doc_id + ".epub"
    build_epub(b, doc_id, os.path.join(feed_dir, epub_name), thai)
    build_cb(b, os.path.join(feed_dir, doc_id + ".cb"))

    title = " ".join(str(b.get("title_en") or doc_id).split())
    h = hashlib.sha1()
    for name in (epub_name, doc_id + ".cb"):
        with open(os.path.join(feed_dir, name), "rb") as f:
            h.update(f.read())
    digest = h.hexdigest()[:12]
    row = [epub_name, b.get("type", ""), b.get("date", ""), title, b.get("created_at", ""), digest]
    rows = [r for r in read_index(feed_dir) if r[0] != epub_name] + [row]
    rows.sort(key=sort_key, reverse=True)
    kept, dropped = rows[:keep], rows[keep:]
    for r in dropped:
        for name in (r[0], r[0][:-5] + ".cb"):
            try:
                os.remove(os.path.join(feed_dir, name))
            except FileNotFoundError:
                pass
    with open(os.path.join(feed_dir, INDEX), "w", encoding="utf-8") as f:
        f.write("# Compound feed: file, type, date, title, created_at, hash (newest first)\n")
        for r in kept:
            f.write("\t".join(r) + "\n")
    print(f"{epub_name}: written; index has {len(kept)} briefs, removed {len(dropped)}")


if __name__ == "__main__":
    main()
