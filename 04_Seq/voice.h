// =============================================================================
// Seq — voice.h
//
// One drum voice: holds a fractional playhead into a sample in drum_bank[],
// advances it once per 48 kHz audio frame, returns 12-bit signed PCM.
//
// Design notes:
//   * Header-only. The whole thing is small enough that the compiler should
//     inline Process() into ProcessSample(). No virtuals, no allocations.
//   * One sample of polyphony per voice — retriggering instantly restarts
//     the playhead. This is "drum-correct": a hat hit during another hat
//     should cancel the previous, not layer.
//   * Truncating (nearest-neighbour) interpolation for M2. Linear interp
//     can come in M7 if anyone hears aliasing. Drum samples + 22.05→48k
//     up-rate typically don't need it.
//   * Per-frame cost: one float compare, one float add, one int cast,
//     one array load, ~maybe 10 cycles total. Three voices ≈ negligible.
// =============================================================================

#pragma once
#include <stdint.h>
#include "samples.h"

struct Voice
{
    // Playhead in source-sample units (22.05 kHz domain).
    // -1.0f = idle. Using a sentinel value avoids carrying a separate bool.
    float   playhead     = -1.0f;

    // Index into drum_bank[]. Defaults to 0; caller sets before/at Trigger.
    uint8_t sample_index = 0;

    // 22.05 kHz source → 48 kHz output: advance playhead by this each frame.
    static constexpr float phase_inc = 22050.0f / 48000.0f;

    // Start (or restart) this voice on a given sample.
    inline void Trigger(uint8_t idx)
    {
        if (idx < NUM_SAMPLES) {
            sample_index = idx;
            playhead     = 0.0f;
        }
    }

    // Hard stop — used by mute, reset, etc.
    inline void Kill() { playhead = -1.0f; }

    inline bool IsActive() const { return playhead >= 0.0f; }

    // Returns the next output frame in signed 12-bit (-2047..+2047),
    // or 0 if the voice is idle / ran past the end of its sample.
    inline int16_t Process()
    {
        if (playhead < 0.0f) return 0;

        const SampleData& s = drum_bank[sample_index];
        const uint32_t    i = (uint32_t)playhead;

        if (i >= s.length) {
            playhead = -1.0f;
            return 0;
        }

        int16_t v = s.data[i];     // already 12-bit from the baker
        playhead += phase_inc;
        return v;
    }
};