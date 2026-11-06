# Forking the simulator

This simulator replaces the firmware's HAL rather than extending it. The consuming firmware's `[env:simulator]` lists `hal` in `lib_ignore`, which drops the firmware's entire `lib/hal/` from the build, and this library supplies `HalDisplay`, `HalStorage`, `HalGPIO`, and the rest in its place.

That means the simulator is tied to one firmware's HAL by construction. If your fork changes a HAL signature, adds a HAL method, or removes one, you need a simulator that matches. **Fork this repo and point your firmware at your fork.** That is the supported path, not a workaround.

> [!WARNING]
> Editing the checked-out copy under `.pio/libdeps/simulator/` is not a fork. PlatformIO owns that directory and will wipe your changes on the next `pio clean` or dependency refetch. If you are re-applying the same patch by hand, you want a fork.

## What belongs where

The split is about the kind of change, not which file it lands in.

**Fork it. Do not send it upstream:**

- Adding, removing, or changing a method on any `Hal*` class to match your firmware's HAL.
- Anything that only compiles because of a change in your firmware fork.
- Device profiles, board capabilities, or input mappings for hardware this project does not target.

**Send it upstream. We want these:**

- Gaps in the platform emulation layer: `Arduino.h`, `WiFi.h`, `esp_http_client.h`, `freertos/`, `driver/`, `soc/`, `nvs.h`, and friends. These emulate ESP-IDF and Arduino, which are external APIs that do not belong to any fork. A missing enum value or an unstubbed function is a bug here whether or not upstream firmware calls it yet.
- Bugs in how the simulator *behaves*: display rendering, orientation, dithering, storage, threading, input handling, the web server shims. These live in `Hal*.cpp` files too, but they are simulator internals rather than API surface, and a fix helps everyone.
- Host portability fixes: new distros, new SDL versions, compiler flag corrections.

The short version: if the change is about **what the simulator does**, upstream wants it. If it is about **what your firmware's HAL looks like**, keep it in your fork.

> [!NOTE]
> If a HAL change from your fork later lands in [crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader), open an issue here. Once it is in upstream firmware, the simulator will track it, and you can drop that patch from your fork.

## Setting up a fork

1. Fork this repository on GitHub.

2. Repoint the `lib_deps` entry in your firmware's `[env:simulator]`:

   ```ini
   lib_deps =
     simulator=https://github.com/YOUR-USER/crosspoint-simulator
   ```

   Pin to a branch or commit if you want reproducible builds:

   ```ini
   simulator=https://github.com/YOUR-USER/crosspoint-simulator#my-branch
   ```

   For local development against a checkout on the same machine:

   ```ini
   simulator=symlink://../crosspoint-simulator
   ```

3. Add this repo as an upstream remote in your fork:

   ```bash
   git remote add upstream https://github.com/crosspoint-reader/crosspoint-simulator
   ```

4. Make your HAL changes, commit, and build. Run `pio run -e simulator -t run_simulator` from the firmware repo to confirm.

## Staying in sync

Merge from upstream whenever you pull firmware updates:

```bash
git fetch upstream && git merge upstream/main
```

Conflicts should be rare, and keeping them rare is mostly under your control:

- **Keep your diff small and additive.** Stubs for fork-only HAL methods are usually one-line no-ops. Add them next to the existing methods rather than reorganizing the class.
- **Do not reformat.** A whitespace or clang-format sweep across a `Hal*.cpp` file will conflict with every upstream change to that file, forever.
- **Do not touch the shared internals unless you mean to.** The SDL main-thread split in `HalDisplay.cpp`, the POSIX fd handling in `HalStorage.cpp`, and the shutdown path in `simulator_main.cpp` all encode fixes for subtle bugs. See [.claude/CONTEXT-sim-notes.md](.claude/CONTEXT-sim-notes.md) before changing any of them. If you do find a real bug there, fix it upstream and merge it back down rather than carrying a local patch.

## The most common breakage

When your firmware adds a HAL method and calls it, the simulator fails to link until a matching stub exists. The linker error names the missing symbol. Add the declaration to the corresponding `Hal*.h` and a definition to the matching `Hal*.cpp`, mirroring your firmware's signature exactly. Most are one-line no-ops:

```cpp
// src/HalDisplay.cpp
void HalDisplay::setDitherMode(int) {}
```

Return a sensible default for anything non-void. The simulator has no hardware behind these calls, so "accept the arguments and do nothing" is almost always correct.

## Reporting bugs from a fork

Please still open issues here for anything in the upstream-owned list above. Say which fork you are running and, if you can, whether the same problem reproduces against unmodified `crosspoint-reader`. Fork-local HAL divergence is expected and is not something this repo can debug, but a rendering or storage bug found in a fork is almost certainly a bug for everyone.
