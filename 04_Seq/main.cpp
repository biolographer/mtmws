// =============================================================================
// Seq — Music Thing Workshop System drum machine
// File: main.cpp
// Milestone: M1 — first sample plays on Pulse In 1
//
// What this does in M1:
//   - On every rising edge of Pulse In 1, retriggers a single voice that
//     plays drum_bank[0] from samples.h.
//   - Audio Out 1 and 2 carry the same mono signal.
//   - CV / pulse / LED / knob behaviour from M0 is preserved as a sanity
//     fallback so we can still see "is the card alive?" at a glance.
//
// Sample format reminder:
//   samples.h stores signed 12-bit values in int16_t cells (-2047..+2047).
//   That matches AudioOut1/2's native range exactly, so playback writes
//   samples directly with no shift, no scale.
//
// CPU budget for M1: trivially under 1 µs per audio frame. We're at ~5%
// of one core. M2 (3 voices) and M6 (humanizer on core 1) will both fit.
// =============================================================================

#include "ComputerCard.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"

#include "samples.h"

class Seq : public ComputerCard
{
private:
    // Fractional playhead, measured in source-sample units (i.e. at 22.05 kHz).
    // -1.0f means "voice idle." This signed-flag-in-the-value trick costs
    // one float compare per audio frame and avoids carrying a separate bool.
    float playhead = -1.0f;

    // Phase increment per 48 kHz output frame:
    //   advance the source-rate playhead by (source_sr / output_sr) per frame.
    // Stored as constexpr so the compiler folds it; no FP divide at runtime.
    static constexpr float phase_inc = 22050.0f / 48000.0f;

    // Which sample in drum_bank[] this voice is currently playing.
    uint8_t sample_index = 0;

public:
    virtual void ProcessSample() override
    {
        // ----- Trigger ------------------------------------------------------
        // Rising edge of Pulse In 1 retriggers the voice from the start.
        // We hard-code drum_bank[0] for M1 — the bank index becomes a
        // per-track parameter starting in M2.
        if (PulseIn1RisingEdge() && NUM_SAMPLES > 0) {
            sample_index = 0;
            playhead = 0.0f;
        }

        // ----- Voice playback ----------------------------------------------
        // Output is signed 12-bit, exactly what AudioOut wants.
        int16_t voice_out = 0;
        if (playhead >= 0.0f) {
            const SampleData& s = drum_bank[sample_index];
            uint32_t i = (uint32_t)playhead;
            if (i < s.length) {
                voice_out = s.data[i];     // already 12-bit, no shift needed
                playhead += phase_inc;
            } else {
                playhead = -1.0f;          // ran past end → voice idle
            }
        }

        // ----- Audio out: same mono voice on both channels -----------------
        AudioOut1(voice_out);
        AudioOut2(voice_out);

        // ----- CV passthrough (sanity, M0 carryover) -----------------------
        CVOut1(CVIn1());
        CVOut2(CVIn2());

        // ----- Pulse passthrough (lets us see the trigger on Pulse Out 1) --
        PulseOut1(PulseIn1());
        PulseOut2(PulseIn2());

        // ----- LED feedback ------------------------------------------------
        // Top-left LED = "voice playing" (visual confirmation per hit).
        // Other left-column LEDs still show switch position from M0.
        Switch s = SwitchVal();
        LedOn(0, playhead >= 0.0f);
        LedOn(2, s == Switch::Middle);
        LedOn(4, s == Switch::Down);

        LedBrightness(1, KnobVal(Knob::Main));
        LedBrightness(3, KnobVal(Knob::X));
        LedBrightness(5, KnobVal(Knob::Y));
    }
};

int main()
{
    // Run at 144 MHz to push ADC harmonics out of the audio band.
    // Must precede ComputerCard construction (peripheral baud/clkdivs are
    // computed from the current sysclk).
    set_sys_clock_khz(144000, true);

    Seq card;
    card.EnableNormalisationProbe();
    card.Run();
    return 0;
}