// =============================================================================
// Seq — Music Thing Workshop System drum machine
// File: main.cpp
// Milestone: M3 — Sequencer + external clock + queued length + reset
//
// What this does in M3:
//   - Three Sequencer instances (one per track), three Voice instances.
//   - Clock auto-detects Pulse In 1: external mode while it's patched and
//     edges are arriving; internal mode (fixed 120 BPM) otherwise.
//   - Pulse In 2 = reset all tracks to step 0 immediately.
//   - Pattern data (hits bitmask) is hardcoded at construction to the M2
//     4-on-the-floor values. M5 will populate from Euclidean parameters
//     instead — same data shape, different writer.
//   - Pulse Out 1 = downbeat (low-track step 0 firing).
//   - Switch Up still mutes the mix.
//
// Diagnostic LED layout for M3 (M4's UI will replace this):
//   Left column:  0 = external-clock-active (steady)
//                 2 = clock tick (flash, ~10 ms)
//                 4 = reset received (flash, ~10 ms)
//   Right column: 1/3/5 = per-track trigger flash (~50 ms)
//
// CPU: 3 voice processes + 3 sequencer ticks every ~125 ms + clock state
// machine. Comfortably under 5% of one core at 144 MHz.
// =============================================================================

#include "ComputerCard.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "samples.h"
#include "voice.h"
#include "sequencer.h"
#include "clock.h"

class Seq : public ComputerCard
{
private:
    static constexpr int NUM_TRACKS = 3;

    Voice     voices[NUM_TRACKS];
    Sequencer seq   [NUM_TRACKS];
    Clock     clock;

    // ----- LED / pulse-out hold counters ---------------------------------
    // We hold visual / pulse states active for N frames after the triggering
    // event, then let them decay. All values are in 48 kHz audio frames.
    static constexpr uint32_t TRIGGER_LED_FRAMES = 2400;   // ~50 ms
    static constexpr uint32_t CLOCK_LED_FRAMES   = 480;    // ~10 ms
    static constexpr uint32_t PULSE_OUT_FRAMES   = 480;    // ~10 ms

    uint32_t led_hold      [NUM_TRACKS] = { 0, 0, 0 };
    uint32_t clock_tick_led             = 0;
    uint32_t reset_led                  = 0;
    uint32_t pulse_out1_hold            = 0;

public:
    Seq()
    {
        // ----- M3 hardcoded patterns -------------------------------------
        // hits bitmask: bit n = step n triggers a hit.
        // M2 had:  kick 0x1111, snare 0x1010, hat 0xFFFF.
        // M5 will recompute these from euc_hits / euc_rotation per track.
        seq[0].track.hits = 0x1111;   // kick  on 0, 4, 8, 12
        seq[1].track.hits = 0x1010;   // snare on 4, 12
        seq[2].track.hits = 0xFFFF;   // hat   on every step

        // Track → sample index mapping. M4 will move this under a knob.
        seq[0].track.base_sample = 0;  // Low
        seq[1].track.base_sample = 1;  // Mid
        seq[2].track.base_sample = 2;  // High

        // All tracks start at the default length 16 (set in Track's
        // member initialisers). Polymetric configurations will be exercised
        // in M4 when the UI can change them.
    }

    virtual void ProcessSample() override
    {
        // ----- 1. Sample inputs (cheap, do once) -------------------------
        const bool p1_edge = PulseIn1RisingEdge();
        const bool p2_edge = PulseIn2RisingEdge();
        const bool p1_conn = Connected(Input::Pulse1);

        // ----- 2. Clock ---------------------------------------------------
        const bool step_now = clock.Tick(p1_edge, p1_conn);

        // ----- 3. Reset has priority over the step that may also fire ----
        // If a reset edge and a clock edge land on the same frame, we
        // first jump every track to step 0, then let the clock fire step 0.
        // That matches the natural "reset then play downbeat" pattern users
        // expect when feeding Seq from a master clock + reset pair.
        if (p2_edge) {
            for (int t = 0; t < NUM_TRACKS; ++t) seq[t].Reset();
            reset_led = CLOCK_LED_FRAMES;
        }

        // ----- 4. Step boundary: advance all sequencers ------------------
        if (step_now) {
            clock_tick_led = CLOCK_LED_FRAMES;
            for (int t = 0; t < NUM_TRACKS; ++t) {
                const StepEvent e = seq[t].Tick();
                if (e.hit) {
                    voices[t].Trigger(seq[t].track.base_sample);
                    led_hold[t] = TRIGGER_LED_FRAMES;
                }
                // Downbeat clock-out: low track fires step 0.
                if (t == 0 && e.step == 0 && e.hit) {
                    pulse_out1_hold = PULSE_OUT_FRAMES;
                }
            }
        }

        // ----- 5. Voice mix ----------------------------------------------
        int32_t mix = 0;
        for (int t = 0; t < NUM_TRACKS; ++t) {
            mix += voices[t].Process();
        }
        mix >>= 2;   // headroom; proper soft-clip in M7

        if (SwitchVal() == Switch::Up) mix = 0;   // mute

        // ----- 6. Outputs -------------------------------------------------
        AudioOut1(mix);
        AudioOut2(mix);

        CVOut1(CVIn1());   // unused-but-passthrough, removed in M4
        CVOut2(CVIn2());

        PulseOut1(pulse_out1_hold > 0);
        PulseOut2(false);  // M9 wires the accent output here

        // ----- 7. Decay hold counters ------------------------------------
        if (pulse_out1_hold > 0) --pulse_out1_hold;
        if (clock_tick_led  > 0) --clock_tick_led;
        if (reset_led       > 0) --reset_led;
        for (int t = 0; t < NUM_TRACKS; ++t) {
            if (led_hold[t] > 0) --led_hold[t];
        }

        // ----- 8. LEDs ----------------------------------------------------
        // Left column: clock diagnostics.
        LedOn(0, clock.IsExternal());
        LedOn(2, clock_tick_led > 0);
        LedOn(4, reset_led      > 0);

        // Right column: per-track trigger flashes.
        LedOn(1, led_hold[0] > 0);   // Low
        LedOn(3, led_hold[1] > 0);   // Mid
        LedOn(5, led_hold[2] > 0);   // High
    }
};

int main()
{
    set_sys_clock_khz(144000, true);

    Seq card;
    card.EnableNormalisationProbe();  // required for Connected() to work
    card.Run();
    return 0;
}