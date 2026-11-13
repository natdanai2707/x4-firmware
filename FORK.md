# x4-firmware

Surf's personal firmware for the Xteink X4 Pro, forked from
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).

Goal: a daily device for English (books + IELTS/TOEFL vocab), Thai reading,
Chinese flashcards, and the Compound morning brief.

## Base

| Part | Source | Pinned at |
|---|---|---|
| Firmware | crosspoint-reader `master` | `5d73104` (2026-11-06) |
| SDK (submodule) | Free-Ink/freeink-sdk | `fb7e926` |
| Desktop simulator | crosspoint-reader/crosspoint-simulator, vendored in `tools/` | 2026-11-06 + 3 HAL stubs |

Upstream is kept as the `upstream` remote. Pull upstream fixes with
`git fetch upstream && git merge upstream/master`.

## Run the simulator (no device needed)

```bash
git clone --recursive https://github.com/natdanai2707/x4-firmware
cd x4-firmware
pip install platformio
mkdir -p fs_/books        # put .epub files here (maps to /books on the SD card)
```

macOS:

```bash
brew install sdl2
pio run -e simulator_x4_pro_mac -t run_simulator
```

Linux / WSL:

```bash
sudo apt install libsdl2-dev libssl-dev
pio run -e simulator_x4_pro -t run_simulator
```

Controls: mouse = touch and swipe, H = Home key. Arrow keys and Enter for the side buttons.

## Build for the real device

```bash
pio run -e x4pro                  # build
pio run -e x4pro -t upload        # flash over USB-C
```

Before the first flash: back up the stock firmware with
`esptool.py read_flash 0 ALL stock-backup.bin`.

## Roadmap

1. [x] Fork + X4 Pro simulator build
2. [x] App-style Home: Continue reading, app tiles, device menu from the right edge
3. [x] SRS engine shared by IELTS/TOEFL vocab and Chinese flashcards
4. [x] Chinese font for study cards (`/study/font.ttf`)
5. [x] Compound briefs synced over WiFi (feed setup below)
6. [x] Thai: vowel and tone placement, word breaking, Thai on study cards and in Compound

## Study decks

Copy decks to the SD card under `/study/`. Starters live in `data/study/`.

| File | Deck |
|---|---|
| `/study/vocab.tsv` | IELTS / TOEFL |
| `/study/chinese.tsv` | Chinese |
| `/study/font.ttf` | Chinese font for cards (copy `data/study/font.ttf`) |

One card per line, fields separated by TAB. Field 1 is the front; the rest
show on the back (meaning, then example, then Thai). Lines starting with `#`
are comments. Add new cards at the end only: progress in `/study/<deck>.srs`
is matched by card order.

The Chinese deck is Surf's SpoonfedChinese Simplified deck from AnkiDroid
(8,017 sentences; Hanzi, pinyin, English), with the review progress of
Anki's "Card 1" (Chinese to English) in `chinese.srs`. Convert another Anki
export (.apkg or .colpkg) with:
`python3 tools/study/anki_to_deck.py <file> data/study --name chinese --deck <part of name> --fields Hanzi,Pinyin,English`
then rebuild the font with `make_study_font.py` so it covers the new characters.

Each session: due reviews first (max 100), then up to 10 new cards per day.
Grades: Again (see it again this session), Good, Easy. Progress saves after
every answer. Thai fields are hidden until Thai rendering is done.

Chinese text on cards uses `/study/font.ttf`, separate from the reader font,
so English books keep their font. The included file is Noto Sans CJK SC cut
down to the 3,755 most common characters (covers HSK 1-6), about 1.1 MB.
Rebuild it with more characters, or for a new deck:
`python3 tools/study/make_study_font.py NotoSansCJK-Regular.ttc data/study/font.ttf data/study/chinese.tsv`.
It must be TrueType outlines: the firmware font engine has no CFF (.otf) driver.

Scheduler tests: `cmake -S test -B build/test && cmake --build build/test --target SrsSchedulerTest && build/test/srs_scheduler/SrsSchedulerTest`

## SD card setup

Copy from `data/`:

| SD card path | From | For |
|---|---|---|
| `/study/vocab.tsv`, `/study/chinese.tsv`, `/study/chinese.srs` | `data/study/` | Study decks (+ Anki progress) |
| `/study/font.ttf` | `data/study/font.ttf` | Chinese on cards |
| `/study/thai.ttf` | `data/study/thai.ttf` | Thai meanings on cards |
| `/fonts/Sarabun/*.ttf` | `data/fonts/Sarabun/` | Thai + English reader font |
| `/thai/words.dic` | `data/thai/words.dic` | Thai line breaking |

Then Settings > Reader > Font Family > Sarabun. Sarabun has Latin too, so it
works for English books, and it lets the menus and Compound briefs show Thai.

## Thai

- Vowels and tone marks are zero-advance marks placed by `thaiMark::place`
  (`lib/EpdFont/EpdFontData.h`): tone marks stack above upper vowels and
  SARA AM, upper marks move left of the ascender on ป ฝ ฟ ฬ, lower vowels drop
  under ฎ ฏ. Fonts need no GSUB/GPOS for this.
- Line breaks between Thai words come from `lib/Epub/Epub/ThaiSegmenter`
  (maximal matching on `/thai/words.dic`, loaded into PSRAM). Without the file
  Thai still breaks, at character clusters only.
- Justified text stretches real spaces only, not the joins between Thai words.
- Not covered: rotated (landscape) text uses the generic mark placement.

## Home

Home shows the clock, a Continue reading card and app tiles (Compound,
IELTS, Chinese, Training). A tile opens its app directly. Swipe in from the
right edge, or press Back, for the device menu: Browse Files, Library, File
Transfer (USB, WiFi), Settings. `src/activities/home/LauncherActivity.*`
replaces the list home; `HomeActivity` is no longer used.

## Compound

The Compound app reads briefs natively with tabs: Markets, News, Words for the
morning news, and Idea, Words for case studies. Left/Right switch tabs,
Up/Down and Next turn pages, taps on the tab bar or the screen halves work too.

Sync downloads `index.txt` and the plain text `.cb` briefs into the hidden
`/.crosspoint/compound/`, so briefs never show up in Library or Browse Files.
Only today and the two days before are kept; older briefs are deleted on open
and on sync. The first time the app opens each day after 07:00 it syncs by
itself when a WiFi network is saved. Words of new briefs are appended to
`/study/vocab.tsv` (IELTS deck), skipping words already in it. An old
`/compound/` folder from earlier builds is removed on first open.

The feed url is built in. `/.crosspoint/compound/feed.txt` can override it:

    url=https://raw.githubusercontent.com/natdanai2707/compound-feed/main/feed/
    token=<GitHub token with read access, for a private feed>

Publishing: the Compound scheduled tasks run `tools/publish.sh` in
[compound-feed](https://github.com/natdanai2707/compound-feed) after each
brief; it writes an EPUB and a `.cb` file per brief. `tools/compound/compound_epub.py`
here is a copy of the feed repo's builder. The feed keeps the newest 30 briefs.

## Training

The Training app (`src/activities/study/TrainingActivity.*`,
`TrainingLogActivity.*`, `src/study/Training.*`) is the device version of the
Training Hub phone app. Both use the Notion "Sessions" database of
"10k Plan: Block 1", so phone and device stay in step.

- The app opens on a picker: Run, Gym, Grip, Hyrox. It does not use WiFi;
  only Run syncs on open. Back from a tab returns to the picker.
- Run: week strip and the sessions of the chosen day. Tap a session to log
  status, distance, pace, average HR, RPE and a note.
- Gym: Push, Legs, Pull (no one-arm row). Tick sets, change loads with - and +. A ticked set
  shows when the rest ends. Save creates a session in Notion; Legs fills in the
  week's planned Strength session instead.
- Grip: the grip work of each weekday, saved the same way.
- Hyrox: Mini Hyrox 30 or 45 min. Each station is one round (run first, then
  the station); tick it when done. No timer. Saved as a Hyrox session, which
  the Today week does not show.

Logs go into `/.crosspoint/training/outbox.tsv` first and are sent on the next
sync, so a workout without WiFi is not lost. Sync sends the outbox, then pulls
the sessions from last week to two weeks ahead. It runs when Run opens and the last
sync is more than an hour old or logs are waiting, and after each save, when a
WiFi network is saved. Loads and ticked sets are kept on the card.

Setup: create a Notion integration, connect it to the "10k Plan: Block 1" page
and save its key as `notion.txt` on the card root (just the key, or
`token=<key>`). It moves to `/.crosspoint/training/notion.txt` on first use.
Notion writes it rejects as invalid go to `/.crosspoint/training/rejected.tsv`.

## Local changes vs upstream

- `src/study/`, `src/activities/study/`: Study hub, SRS scheduler, deck store, study fonts, session screen, Compound sync, Training app and Notion sync.
- `lib/Utf8/Utf8.h`, `lib/EpdFont/`, `lib/GfxRenderer/GfxRenderer.cpp`: Thai marks.
- `lib/Epub/Epub/ThaiSegmenter.*`, `ThaiDictionaryStore.*`, `ParsedText.cpp`, `Section.cpp` (cache v74): Thai line breaking.
- `src/activities/home/LauncherActivity.*`, `ActivityManager.cpp`: app-style Home with a right-edge device menu.
- `lib/LibraryIndex/LibraryBuilder.cpp`: the `study` folder is not scanned for books.
- `platformio.ini`: added `simulator`, `simulator_x4_pro`, `simulator_x4_pro_mac` envs;
  libraries pulled from GitHub instead of the PlatformIO registry.
- `tools/crosspoint-simulator/src/HalDisplay.h`, `HalStorage.h`: stubbed
  `controllerName()`, `sdTotalBytes()`, `sdUsedBytes()` that current firmware calls
  but the simulator did not yet provide.
