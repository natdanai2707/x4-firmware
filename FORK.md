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
2. [x] Study hub on Home (Compound, IELTS/TOEFL, Chinese)
3. [x] SRS engine shared by IELTS/TOEFL vocab and Chinese flashcards
4. [x] Chinese font for study cards (`/study/font.ttf`)
5. [x] Compound briefs synced over WiFi (feed setup below)
6. [ ] Thai rendering (vowel/tone stacking, word breaking)

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

## Compound feed

Study > Compound > Sync now joins WiFi, downloads `index.txt` and any new
briefs from the feed into `/compound/`, and removes briefs that left the feed.
Tap a brief to read it in the normal EPUB reader (dictionary, bookmarks).

Device setup: `/compound/feed.txt`

    url=https://raw.githubusercontent.com/natdanai2707/compound-feed/main/feed/
    token=<GitHub token with read access, only for a private repo>

Publishing: each Compound scheduled task runs
`tools/compound/compound_epub.py <brief.json> <doc_id> feed/` in the feed
repo and pushes. The feed keeps the newest 30 briefs. Thai translations are
left out until Thai rendering works (`--thai` adds them).

## Local changes vs upstream

- `src/study/`, `src/activities/study/`: Study hub, SRS scheduler, deck store, study font, session screen, Compound sync.
- `src/activities/home/HomeActivity.*`: Study entry first on the list home.
- `platformio.ini`: added `simulator`, `simulator_x4_pro`, `simulator_x4_pro_mac` envs;
  libraries pulled from GitHub instead of the PlatformIO registry.
- `tools/crosspoint-simulator/src/HalDisplay.h`, `HalStorage.h`: stubbed
  `controllerName()`, `sdTotalBytes()`, `sdUsedBytes()` that current firmware calls
  but the simulator did not yet provide.
