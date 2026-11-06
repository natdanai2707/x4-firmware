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
2. [ ] Custom Home screen (Compound, Books, IELTS, Chinese)
3. [ ] SRS engine shared by IELTS/TOEFL vocab and Chinese flashcards
4. [ ] Compound morning sync over WiFi
5. [ ] Thai rendering (vowel/tone stacking, word breaking)

## Local changes vs upstream

- `platformio.ini`: added `simulator`, `simulator_x4_pro`, `simulator_x4_pro_mac` envs;
  libraries pulled from GitHub instead of the PlatformIO registry.
- `tools/crosspoint-simulator/src/HalDisplay.h`, `HalStorage.h`: stubbed
  `controllerName()`, `sdTotalBytes()`, `sdUsedBytes()` that current firmware calls
  but the simulator did not yet provide.
