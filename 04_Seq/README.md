# Seq — polymetric drum machine for the Music Thing Workshop System Computer

Three-voice sample-based drum machine (Low / Mid / High) with independent
per-track pattern lengths for polymetric playback, an organic timing-feel
engine inspired by the $1/f$ rhythmic-coupling model, and
a curated sample bank baked into a 16 MB program card.

> **Status: WIP — Milestone 0 (passthrough baseline).**
> This commit only proves the toolchain, board configuration and I/O are
> healthy. No drum-machine logic yet. See `docs/PLAN.md` for the full
> roadmap.

## What works in this build

| Behaviour | Status |
|---|---|
| Audio In 1/2 → Audio Out 1/2 passthrough | ✅ |
| CV In 1/2 → CV Out 1/2 passthrough (uncalibrated) | ✅ |
| Pulse In 1/2 → Pulse Out 1/2 echo | ✅ |
| Switch position lit on left LED column | ✅ |
| Knob values lit on right LED column | ✅ |
| Survives reset and power cycle | ✅ (verify on your hardware!) |

## Build

Prerequisites: Pico SDK installed, `PICO_SDK_PATH` exported, `arm-none-eabi-gcc`
on `PATH`, `cmake` ≥ 3.13.

```bash
# First time only — fetch the ComputerCard header
./tools/vendor_computercard.sh

# Configure + build
cmake -B build -S .
cmake --build build -j

# Resulting UF2 is at:
#   build/Seq.uf2
# and is also copied next to the sources as Seq.uf2 for convenience.