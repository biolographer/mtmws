// =============================================================================
// Seq — Music Thing Workshop System drum machine
// File: main.cpp
// Milestone: M4 — real UI: Perform/Edit, two functional pages, soft takeover
//
// New in M4:
//   * UI is now a separate class (ui.h). main.cpp shrinks to wiring.
//   * Switch position drives Perform / Edit mode. Down is momentary, cycles
//     the edit page. Page 3 is reserved for M5 (Euclidean).
//   * All three tracks editable simultaneously on every page:
//       Page 1 — Sample (Main/X/Y → base_sample for low/mid/high)
//       Page 2 — Length (Main/X/Y → pending_length, queued)
//       Page 3 — Reserved (LED lights, knobs do nothing)
//   * Soft takeover on every (knob, page) pair: a knob must "catch" the
//     parameter before it can move it. Catch state is per-page, so changing
//     pages re-arms all knobs.
//   * LED layout reworked:
//       Left column (0/2/4)  = page indicator (only lit in EDIT)
//       Right column (1/3/5) = per-track state:
//         priority 1 — trigger flash (full bright, ~50 ms)
//         priority 2 — pending length blink (~50%, 2 Hz, Page 2 only)
//         priority 3 — soft-takeover armed dim (~25%, EDIT only)
//         otherwise — off
//
// M3 → M4 carryover unchanged: Clock with live external/internal detection,
// Pulse In 2 reset, Pulse Out 1 downbeat, three Sequencers + three Voices.
//
// What M4 deliberately does NOT do:
//   * Mute on Switch::Up — Perform now means "play with no edits." Pull
//     the cable to silence the card during development.
//   * Tempo control — internal clock fixed at 120 BPM. Deferred to M4.5+.
//   * Euclidean / Pattern pages — M5 wires Page 3 and may add Page 4.
//   * Page-hold-to-jump-to-page-1 — M4.5 polish, deferred.
// =============================================================================

#include "ComputerCard.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "samples.h"
#include "voice.h"
#include "sequencer.h"
#include "clock.h"
#include "ui.h"

class Seq : public ComputerCard
{
private:
    static constexpr int NUM_TRACKS = 3;

    Voice     voices[NUM_TRACKS];
    Sequencer seq   [NUM_TRACKS];
    Clock     clock;
    UI        ui;

    // ----- LED / pulse-out hold counters (frames @ 48 kHz) ---------------
    static constexpr uint32_t TRIGGER_LED_FRAMES = 2400;   // ~50 ms
    static constexpr uint32_t PULSE_OUT_FRAMES   = 480;    // ~10 ms

    // LED brightness levels (PWM 0..4095) used in the right-column logic.
    static constexpr int16_t LED_FULL    = 4095;
    static constexpr int16_t LED_PENDING = 2048;  // ~50% — length blink
    static constexpr int16_t LED_ARMED   = 1024;  // ~25% — soft takeover

    uint32_t led_hold      [NUM_TRACKS] = { 0, 0, 0 };
    uint32_t pulse_out1_hold            = 0;

public:
    Seq()
    {
        // M4 starting pattern — same hardcoded 4-on-the-floor as M3.
        // hits bitmask remains the data shape M5 will populate from
        // Euclidean parameters; for now we write it directly.
        seq[0].track.hits = 0x1111;   // kick on 0, 4, 8, 12
        seq[1].track.hits = 0x1010;   // snare on 4, 12
        seq[2].track.hits = 0xFFFF;   // hat every step

        seq[0].track.base_sample = 0;   // Low  — kick by convention
        seq[1].track.base_sample = 1;   // Mid  — snare
        seq[2].track.base_sample = 2;   // High — hat
    }

    virtual void ProcessSample() override
    {
        // ----- 1. Sample inputs -----------------------------------------
        const bool p1_edge = PulseIn1RisingEdge();
        const bool p2_edge = PulseIn2RisingEdge();
        const bool p1_conn = Connected(Input::Pulse1);

        // ----- 2. Clock --------------------------------------------------
        const bool step_now = clock.Tick(p1_edge, p1_conn);

        // ----- 3. Reset (Pulse In 2) ------------------------------------
        if (p2_edge) {
            for (int t = 0; t < NUM_TRACKS; ++t) seq[t].Reset();
        }

        // ----- 4. Sequencer step boundary -------------------------------
        if (step_now) {
            for (int t = 0; t < NUM_TRACKS; ++t) {
                const StepEvent e = seq[t].Tick();
                if (e.hit) {
                    voices[t].Trigger(seq[t].track.base_sample);
                    led_hold[t] = TRIGGER_LED_FRAMES;
                }
                if (t == 0 && e.step == 0 && e.hit) {
                    pulse_out1_hold = PULSE_OUT_FRAMES;
                }
            }
        }

        // ----- 5. UI update ---------------------------------------------
        // UI mutates tracks in place via these pointers. Knob ordering
        // matches the physical panel: Main=low, X=mid, Y=high.
        Track* track_ptrs[NUM_TRACKS] = {
            &seq[0].track, &seq[1].track, &seq[2].track
        };
        ui.Update(SwitchVal(),
                  KnobVal(Knob::Main),
                  KnobVal(Knob::X),
                  KnobVal(Knob::Y),
                  track_ptrs);

        // ----- 6. Audio mix ---------------------------------------------
        int32_t mix = 0;
        for (int t = 0; t < NUM_TRACKS; ++t) mix += voices[t].Process();
        mix >>= 2;   // headroom; proper soft-clip lands in M7
        AudioOut1(mix);
        AudioOut2(mix);

        // ----- 7. CV out passthrough (M5+ will use these for real) ------
        CVOut1(CVIn1());
        CVOut2(CVIn2());

        // ----- 8. Pulse out ---------------------------------------------
        PulseOut1(pulse_out1_hold > 0);
        PulseOut2(false);                   // M9: accent out

        // ----- 9. Decay hold counters -----------------------------------
        if (pulse_out1_hold > 0) --pulse_out1_hold;
        for (int t = 0; t < NUM_TRACKS; ++t) {
            if (led_hold[t] > 0) --led_hold[t];
        }

        // ----- 10. LEDs --------------------------------------------------
        UpdateLeds();
    }

private:
    void UpdateLeds()
    {
        const UI::Mode mode = ui.CurrentMode();
        const UI::Page page = ui.CurrentPage();

        // ---- Left column: page indicator (EDIT only) -------------------
        if (mode == UI::EDIT) {
            LedBrightness(0, page == UI::PAGE_SAMPLE   ? LED_FULL : 0);
            LedBrightness(2, page == UI::PAGE_LENGTH   ? LED_FULL : 0);
            LedBrightness(4, page == UI::PAGE_RESERVED ? LED_FULL : 0);
        } else {
            LedOff(0); LedOff(2); LedOff(4);
        }

        // ---- Right column: per-track state -----------------------------
        for (int t = 0; t < NUM_TRACKS; ++t) {
            const int led_idx = 1 + t * 2;   // 1, 3, 5

            // Priority 1: trigger flash always wins
            if (led_hold[t] > 0) {
                LedBrightness(led_idx, LED_FULL);
                continue;
            }

            // Priority 2: pending length change (Page 2, EDIT only)
            if (mode == UI::EDIT && page == UI::PAGE_LENGTH) {
                const Track& tr = seq[t].track;
                if (tr.length != tr.pending_length) {
                    LedBrightness(led_idx, ui.BlinkOn() ? LED_PENDING : 0);
                    continue;
                }
            }

            // Priority 3: soft-takeover armed dim
            if (mode == UI::EDIT && !ui.TrackCaught(t)) {
                LedBrightness(led_idx, LED_ARMED);
                continue;
            }

            // Otherwise off
            LedOff(led_idx);
        }
    }
};

int main()
{
    set_sys_clock_khz(144000, true);

    Seq card;
    card.EnableNormalisationProbe();
    card.Run();
    return 0;
}