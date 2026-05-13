// =============================================================================
// Seq — Music Thing Workshop System drum machine
// File: main.cpp
// Milestone: M0 — passthrough baseline
//
// What this does in M0:
//   - Audio In 1/2 → Audio Out 1/2  (clean stereo passthrough)
//   - CV In 1/2    → CV Out 1/2     (uncalibrated; sanity test only)
//   - Pulse In 1/2 → Pulse Out 1/2  (echo)
//   - Left LED column shows switch position (Up / Middle / Down)
//   - Right LED column brightness = the three knob values
//
// Why this version exists:
//   Before adding any sequencer/sample/humanizer logic, we want absolute
//   confidence that the toolchain, board configuration, and wiring are all
//   healthy:
//       1. The .uf2 builds and links.
//       2. The board flashes and boots.
//       3. After a power cycle and after a reset, it STILL boots — this
//          is the single most common new-card regression and is governed
//          by PICO_XOSC_STARTUP_DELAY_MULTIPLIER in CMakeLists.txt.
//       4. All six jacks, three knobs, the switch and all six LEDs respond.
//   Once this milestone is committed, every later milestone has a known-good
//   baseline to bisect back to.
//
// This file will grow into the real Seq card. Keep it intentionally tiny
// for M0 so the comparison "does the unmodified passthrough work?" is a
// useful debugging tool when something goes wrong later.
// =============================================================================

#include "ComputerCard.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"

class Seq : public ComputerCard
{
public:
    // ProcessSample is called once per audio sample at 48 kHz, in ISR
    // context. Hard limit: ~20 µs. The M0 body is trivial; we are using
    // well under 1 µs.
    virtual void ProcessSample() override
    {
        // ----- Audio passthrough ---------------------------------------------
        // Inputs are signed 12-bit, -2048..+2047. Direct copy is fine.
        AudioOut1(AudioIn1());
        AudioOut2(AudioIn2());

        // ----- CV passthrough ------------------------------------------------
        // CVOut1/2 here are uncalibrated. That is acceptable for a sanity
        // test — Seq does not need pitch-accurate CV anywhere.
        CVOut1(CVIn1());
        CVOut2(CVIn2());

        // ----- Pulse passthrough ---------------------------------------------
        // ComputerCard handles the on-board inversion internally, so
        // "high in" → "high out" is what the front panel sees.
        PulseOut1(PulseIn1());
        PulseOut2(PulseIn2());

        // ----- Switch indication on the LEFT LED column ----------------------
        // The Workshop Computer switch is (ON)-OFF-ON: Up is a stable toggle,
        // Middle is the spring-rest position, Down is spring-loaded momentary.
        // Lighting the LED that matches the current position confirms the
        // firmware sees the same thing your fingers do.
        Switch s = SwitchVal();
        LedOn(0, s == Switch::Up);      // top-left
        LedOn(2, s == Switch::Middle);  // middle-left
        LedOn(4, s == Switch::Down);    // bottom-left

        // ----- Knob brightness on the RIGHT LED column -----------------------
        // KnobVal returns 0..4095, which maps directly onto LedBrightness.
        // Useful as a live visual that all three pots are wired and that
        // the smoothing filter is delivering reasonable values (no jumps,
        // no stuck-at-1780 — the classic ISR-overrun symptom).
        LedBrightness(1, KnobVal(Knob::Main));  // top-right    → Main pot
        LedBrightness(3, KnobVal(Knob::X));     // middle-right → X pot
        LedBrightness(5, KnobVal(Knob::Y));     // bottom-right → Y pot
    }
};

int main()
{
    // ---------------------------------------------------------------------
    // Run the system clock at 144 MHz (default is 125 MHz).
    //
    // Why: at 144 MHz the ADC sample-rate harmonics fall on less audible
    // frequencies, reducing the faint tonal whine that 125 MHz produces in
    // the audio inputs. The directive recommends this as the default for
    // any audio-rate Workshop Computer card.
    //
    // The `true` argument tells the SDK we are willing to wait for the PLL
    // to relock; without it, set_sys_clock_khz can silently return false.
    //
    // MUST be called BEFORE constructing the ComputerCard, because the
    // ComputerCard constructor configures peripherals (SPI baud, ADC clkdiv,
    // PWM wraps) that are derived from the system clock.
    // ---------------------------------------------------------------------
    set_sys_clock_khz(144000, true);

    Seq card;

    // Enable the GPIO4 pseudo-random "is this jack actually plugged in?"
    // probe. We don't *use* Connected()/Disconnected() in M0, but turning
    // it on here exercises the same init path as later milestones — so any
    // wiring/init issue (UART contention, probe pin not floating, etc.)
    // surfaces now while the rest of the code is still trivial.
    card.EnableNormalisationProbe();

    card.Run();   // never returns — enters the audio ISR loop
    return 0;
}