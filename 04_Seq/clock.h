// =============================================================================
// Seq — clock.h
//
// Combined external/internal step clock with live mode detection.
//
// Behaviour:
//   * If Pulse In 1 is patched (Connected(Pulse1) is true) AND we've seen
//     a rising edge recently, we're in EXTERNAL mode: each rising edge is
//     one step.
//   * If Pulse In 1 is unpatched, OR no edges have arrived for ~500 ms,
//     we fall back to INTERNAL mode at 120 BPM (16th-note resolution =
//     6000 frames per step at 48 kHz).
//   * Patching/unpatching mid-song flips modes seamlessly. The user will
//     hear at most ~125 ms of clock interruption when unpatching (one step
//     of the internal clock catching up).
//
// Future hook (Decision 2 deferred): when we add a tempo control in M4+,
// expose INTERNAL_FRAMES_PER_STEP as a settable member, not a constexpr.
// =============================================================================

#pragma once
#include <stdint.h>

class Clock
{
public:
    // Call once per audio frame. Returns true on the exact frame a step
    // boundary occurs. Edge detection is the caller's responsibility — we
    // expect a one-frame-wide `pulse1_rising_edge` pulse.
    bool Tick(bool pulse1_rising_edge, bool pulse1_connected)
    {
        // ----- External edge: definitive "we're driven externally" -----
        if (pulse1_connected && pulse1_rising_edge) {
            external          = true;
            frames_since_edge = 0;
            internal_counter  = 0;     // resync so fallback is in phase
            return true;
        }

        // ----- External mode but no edge this frame --------------------
        if (external) {
            ++frames_since_edge;
            if (!pulse1_connected ||
                frames_since_edge > EXTERNAL_TIMEOUT_FRAMES)
            {
                external         = false;
                internal_counter = 0;
            }
            return false;
        }

        // ----- Internal mode -------------------------------------------
        if (++internal_counter >= INTERNAL_FRAMES_PER_STEP) {
            internal_counter = 0;
            return true;
        }
        return false;
    }

    bool IsExternal() const { return external; }

private:
    bool     external          = false;
    uint32_t frames_since_edge = 0;
    uint32_t internal_counter  = 0;

    // 120 BPM, 16th-note resolution: 48000 / (120 * 4 / 60) = 6000 frames.
    static constexpr uint32_t INTERNAL_FRAMES_PER_STEP = 6000;

    // ~500 ms with no external edge → fall back to internal.
    // Chosen so a sequencer at a sluggish 30 BPM (500 ms/16th) still holds
    // external mode, but a removed cable drops back within a beat or two.
    static constexpr uint32_t EXTERNAL_TIMEOUT_FRAMES = 24000;
};