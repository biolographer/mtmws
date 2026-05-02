#include "ComputerCard.h"

// 12-bit random number generator
uint32_t __not_in_flash_func(rnd12)()
{
    static uint32_t lcg_seed = 1;
    lcg_seed = 1664525 * lcg_seed + 1013904223;
    return lcg_seed >> 20;
}

class GranularSlicer : public ComputerCard
{
public:
    static constexpr uint32_t bufSize = 96000; // 2 seconds at 48kHz
    int16_t buffer[bufSize];
    
    // Buffer Trackers
    uint32_t writeInd = 0;
    uint32_t frozenWriteInd = 0;
    
    // Playheads & Anti-Click Crossfades
    uint32_t playPos1 = 0;
    uint32_t newPlayPos1 = 0;
    int32_t xfadeInd1 = 0;

    uint32_t playPos2 = 0;
    uint32_t newPlayPos2 = 0;
    int32_t xfadeInd2 = 0;
    
    static constexpr int xfadeLen = 25;

    // Downsampling & Pitch controls
    static constexpr int DOWNSAMPLE_FACTOR = 2; 
    uint32_t dsTick = 0;
    uint32_t pitchSubTick1 = 0;
    uint32_t pitchSubTick2 = 0;
    
    // Clock & Striation Timers
    uint32_t samplesSinceLastClock = 0;
    uint32_t cycleLengthSamples = 48000;
    uint32_t sliceTimer = 0;
    uint32_t currentSliceLength = 0;
    
    // Y-Knob Mode Variables
    uint32_t samplesPlayedInSlice1 = 0;
    uint32_t samplesPlayedInSlice2 = 0;
    uint32_t ratchetSubTimer1 = 0;
    uint32_t ratchetSubTimer2 = 0;
    
    // Turing Machine Sequencer
    uint8_t seq[64];
    uint8_t currentStep = 0;
    
    // UI & State
    enum Mode { RECORD, SLICE };
    Mode currentMode = RECORD;
    Switch lastSwitch = Switch::Down;

    // CH2 Reverse State
    uint32_t switchHoldDownTimer = 0;
    bool switchHoldDownTriggered = false;
    bool reverseCh2 = false;

    // Y-Knob Multi-Mode State
    // 0: Gate, 1: Ratchet, 2: Jitter, 3: Pitch
    uint8_t yKnobMode = 0; 
    uint32_t switchHoldUpTimer = 0;
    bool switchHoldUpTriggered = false;
    uint32_t modeDisplayTimer = 0; // Keeps LEDs on briefly to show the mode

    GranularSlicer()
    {
        for (uint32_t i = 0; i < bufSize; i++) buffer[i] = 0;
        for (int i = 0; i < 64; i++) seq[i] = 0;
    }

    void __not_in_flash_func(ProcessSample)()
    {
        Switch s = SwitchVal();

        // Advance the master downsample clock
        dsTick++;
        if (dsTick >= DOWNSAMPLE_FACTOR) {
            dsTick = 0;
        }

        // ---------------------------------------------------------
        // 1. Read Knobs 
        // ---------------------------------------------------------
        int totalSlices = (KnobVal(Knob::X) * 64) / 4096;
        if (totalSlices < 1) totalSlices = 1;

        int yVal = KnobVal(Knob::Y); // 0 to 4095

        // ---------------------------------------------------------
        // 2. STATE MANAGER & UI GESTURES
        // ---------------------------------------------------------
        
        // DOWN HOLD: Toggle CH2 Reverse (0.5 seconds)
        if (s == Switch::Down) {
            switchHoldDownTimer++;
            if (switchHoldDownTimer >= 24000 && !switchHoldDownTriggered) {
                reverseCh2 = !reverseCh2; 
                switchHoldDownTriggered = true;
            }
        } else {
            switchHoldDownTimer = 0;
            switchHoldDownTriggered = false;
        }

        // UP HOLD: Cycle Y-Knob Modes (1.5 seconds)
        if (s == Switch::Up) {
            switchHoldUpTimer++;
            if (switchHoldUpTimer >= 72000 && !switchHoldUpTriggered) {
                yKnobMode++;
                if (yKnobMode > 3) yKnobMode = 0;
                switchHoldUpTriggered = true;
                modeDisplayTimer = 48000; // Show LEDs for 1 second
            }
        } else {
            switchHoldUpTimer = 0;
            switchHoldUpTriggered = false;
        }

        // Standard Switch Actions
        if (s == Switch::Up && lastSwitch != Switch::Up) {
            currentMode = RECORD;
        } 
        else if (s == Switch::Middle && lastSwitch != Switch::Middle) {
            currentMode = SLICE;
            frozenWriteInd = writeInd; 
            playPos1 = frozenWriteInd; 
            playPos2 = frozenWriteInd;
        }
        else if (s == Switch::Down && lastSwitch != Switch::Down) {
            currentStep = 0;
            sliceTimer = 0;
        }
        lastSwitch = s;

        // Decrement Mode Display Timer
        if (modeDisplayTimer > 0) modeDisplayTimer--;

        // ---------------------------------------------------------
        // 3. PINGABLE CLOCK & X-GRID CALCULATION
        // ---------------------------------------------------------
        bool triggerNextSlice = false;
        
        if (Connected(Input::Pulse1)) {
            samplesSinceLastClock++;
            if (PulseInRisingEdge(1)) {
                cycleLengthSamples = samplesSinceLastClock;
                samplesSinceLastClock = 0;
                currentStep = 0; 
                sliceTimer = 0;
                triggerNextSlice = true;
            }
        } else {
            // Matches the internal timer to the total time it takes to fill the buffer
            cycleLengthSamples = bufSize * DOWNSAMPLE_FACTOR;
        }

        uint32_t triggerInterval = cycleLengthSamples / totalSlices;
        if (triggerInterval == 0) triggerInterval = 1; 

        sliceTimer++;
        if (sliceTimer >= triggerInterval) {
            sliceTimer = 0;
            triggerNextSlice = true;
        }

        // ---------------------------------------------------------
        // 4. THE TURING MACHINE & POLYMETRIC JUMPS
        // ---------------------------------------------------------
        if (currentMode == SLICE && triggerNextSlice) {
            int mainKnob = KnobVal(Knob::Main);
            currentSliceLength = bufSize / totalSlices;
            
            // TURING LOGIC
            if (mainKnob < 100) {
                seq[currentStep] = currentStep % totalSlices; 
            } 
            else if (mainKnob > 4000) {
                // Loop locked
            } 
            else {
                int prob;
                if (mainKnob <= 2048) {
                    prob = (mainKnob - 100) * 4095 / 1948; 
                } else {
                    prob = (4000 - mainKnob) * 4095 / 1952; 
                }
                if (rnd12() < prob) {
                    seq[currentStep] = rnd12() % totalSlices; 
                }
            }

            uint32_t targetSlice1 = seq[currentStep];
            uint32_t targetSlice2 = reverseCh2 ? seq[(totalSlices - 1) - currentStep] : targetSlice1;

            // --- MODE 2: JITTER (Adds random offset to start point) ---
            uint32_t jitterOffset = 0;
            if (yKnobMode == 2) {
                uint32_t maxJitter = currentSliceLength / 2;
                jitterOffset = (rnd12() * yVal * maxJitter) / (4095 * 4095);
            }

            newPlayPos1 = (frozenWriteInd + (targetSlice1 * currentSliceLength) + jitterOffset) % bufSize;
            newPlayPos2 = (frozenWriteInd + (targetSlice2 * currentSliceLength) + jitterOffset) % bufSize;
            
            xfadeInd1 = xfadeLen; 
            xfadeInd2 = xfadeLen;
            samplesPlayedInSlice1 = 0;
            samplesPlayedInSlice2 = 0;
            ratchetSubTimer1 = 0;
            ratchetSubTimer2 = 0;
            
            currentStep++;
            if (currentStep >= totalSlices) currentStep = 0;
            
            PulseOut1(true); 
        } else {
            PulseOut1(false);
        }

        // ---------------------------------------------------------
        // 5. AUDIO ROUTING & DSP
        // ---------------------------------------------------------
        int32_t audioIn = AudioIn1();

        if (currentMode == RECORD) {
            if (dsTick == 0) {
                buffer[writeInd] = audioIn;
                writeInd++;
                if (writeInd >= bufSize) writeInd = 0;
            }
            AudioOut1(audioIn);
            AudioOut2(audioIn);
            LedOn(0, true); 
        } 
        else if (currentMode == SLICE) {
            LedOn(0, false);
            
            // --- MODE 0 & 1 & 3 PRE-CALCULATIONS ---
            
            // Gate Math
            float gatePercent = 1.0f;
            if (yKnobMode == 0) gatePercent = (yVal / 4095.0f);
            if (gatePercent < 0.01f) gatePercent = 0.01f;
            uint32_t allowedPlaySamples = currentSliceLength * gatePercent;

            // Pitch Math (Advance speed)
            int pitchAdvance = 1;
            if (yKnobMode == 3) {
                if (yVal < 1365) {
                    pitchAdvance = 0; // Half speed (advances every other tick)
                    pitchSubTick1++;
                    if (pitchSubTick1 >= 2) { pitchAdvance = 1; pitchSubTick1 = 0; }
                } 
                else if (yVal > 2730) {
                    pitchAdvance = 2; // Double speed (octave up)
                }
            }

            // Ratchet Math
            int ratchets = 1;
            if (yKnobMode == 1) ratchets = 1 + (yVal * 4) / 4096; // 1, 2, 3, or 4
            uint32_t ratchetInterval = triggerInterval / ratchets;


            // --- CH1 DSP ---
            int32_t out1 = 0; 

            // Mode 1: Ratchet Trigger Check
            if (yKnobMode == 1) {
                ratchetSubTimer1++;
                if (ratchetSubTimer1 >= ratchetInterval && ratchets > 1) {
                    ratchetSubTimer1 = 0;
                    playPos1 = newPlayPos1; // Snap back to start of slice
                    xfadeInd1 = xfadeLen;
                }
            }

            // Mode 0: Gate Muting
            if (samplesPlayedInSlice1 < allowedPlaySamples) {
                if (xfadeInd1 > 0) {
                    int32_t oldVal = buffer[playPos1];
                    int32_t newVal = buffer[newPlayPos1];
                    out1 = (oldVal * xfadeInd1 + newVal * (xfadeLen - xfadeInd1)) / xfadeLen;
                    
                    if (dsTick == 0) {
                        playPos1 = (playPos1 + pitchAdvance) % bufSize;
                        newPlayPos1 = (newPlayPos1 + pitchAdvance) % bufSize;
                        samplesPlayedInSlice1++; 
                        xfadeInd1--;
                        if (xfadeInd1 == 0) playPos1 = newPlayPos1;
                    }
                } else {
                    out1 = buffer[playPos1];
                    if (dsTick == 0) {
                        playPos1 = (playPos1 + pitchAdvance) % bufSize;
                        samplesPlayedInSlice1++; 
                    }
                }
            }
            AudioOut1(out1);

            // --- CH2 DSP (Identical but follows CH2 trackers) ---
            int32_t out2 = 0; 
            
            if (yKnobMode == 1) {
                ratchetSubTimer2++;
                if (ratchetSubTimer2 >= ratchetInterval && ratchets > 1) {
                    ratchetSubTimer2 = 0;
                    playPos2 = newPlayPos2;
                    xfadeInd2 = xfadeLen;
                }
            }

            if (samplesPlayedInSlice2 < allowedPlaySamples) {
                if (xfadeInd2 > 0) {
                    int32_t oldVal = buffer[playPos2];
                    int32_t newVal = buffer[newPlayPos2];
                    out2 = (oldVal * xfadeInd2 + newVal * (xfadeLen - xfadeInd2)) / xfadeLen;
                    
                    if (dsTick == 0) {
                        playPos2 = (playPos2 + pitchAdvance) % bufSize;
                        newPlayPos2 = (newPlayPos2 + pitchAdvance) % bufSize;
                        samplesPlayedInSlice2++; 
                        xfadeInd2--;
                        if (xfadeInd2 == 0) playPos2 = newPlayPos2;
                    }
                } else {
                    out2 = buffer[playPos2];
                    if (dsTick == 0) {
                        playPos2 = (playPos2 + pitchAdvance) % bufSize;
                        samplesPlayedInSlice2++; 
                    }
                }
            }
            AudioOut2(out2);
        }
        
        // ---------------------------------------------------------
        // 6. UI INDICATORS
        // ---------------------------------------------------------
        
        // If the mode was just changed, hijack the LEDs to show the mode number
        if (modeDisplayTimer > 0) {
            LedOn(0, yKnobMode >= 0); // Always on
            LedOn(1, yKnobMode >= 1);
            LedOn(2, yKnobMode >= 2);
            LedOn(3, yKnobMode >= 3);
            LedOn(4, false);
            LedOn(5, false);
        } else {
            // Normal LED operation
            LedOn(1, reverseCh2); 
            CVOut1((currentStep * 4095) / totalSlices);
        }
    }
};

int main()
{
    set_sys_clock_khz(160000, true);
    GranularSlicer slicer;
    slicer.EnableNormalisationProbe();
    slicer.Run();
    return 0;
}