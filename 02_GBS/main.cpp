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
    static constexpr uint32_t bufSize = 96000; // 4 seconds at 24kHz (downsampled 48kHz)
    int16_t buffer[bufSize];
    
    // Rhythmic Buffer Size
    uint32_t activeBufferLength = bufSize; 

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
    
    // Clock & Sync Trackers
    uint32_t samplesSinceLastClock = 0;
    uint32_t smoothedPulseLength = 0;
    bool clockWasStopped = true;

    // Slicing Timers
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
    bool reverseCh2 = false;

    // Switch Down Gesture Tracking
    uint32_t switchHoldDownTimer = 0;
    bool reverseTriggered = false;
    bool resetTriggered = false;

    // Y-Knob Multi-Mode State
    // 0: Gate, 1: Ratchet, 2: Jitter, 3: Pitch, 4: Bar Length
    uint8_t yKnobMode = 0; 
    
    // True Catch-up Takeover State
    bool yKnobActive = true;
    // Defaults: Gate=Full, Ratchet=1, Jitter=0, Pitch=Normal, BarLength=4 QN
    int32_t latchedYKnobRaw[5] = {4095, 0, 0, 2048, 1536}; 
    
    // Latched Y-Knob Parameters
    uint32_t latchedGateInt = 4096; // 1.0 multiplier mapped to 0-4096 for fast math
    int latchedRatchets = 1;
    int latchedPitchState = 1; 
    int latchedJitterAmt = 0;
    int latchedBarLength = 4;

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
        // 1. Read Knobs & Update Latched Memory
        // ---------------------------------------------------------
        int totalSlices = (KnobVal(Knob::X) * 64) / 4096;
        if (totalSlices < 1) totalSlices = 1;

        int32_t yVal = KnobVal(Knob::Y);

        // True Catch-Up Takeover Logic
        if (!yKnobActive) {
            int32_t targetVal = latchedYKnobRaw[yKnobMode];
            int32_t diff = yVal - targetVal;
            
            // If the physical knob is within ~2.5% of the stored value, take over
            if (diff > -100 && diff < 100) {
                yKnobActive = true;
            }
        }

        // Only update memory if the knob has caught up to the stored value
        if (yKnobActive) {
            latchedYKnobRaw[yKnobMode] = yVal; // Update the raw storage

            // --- UPDATE LATCHED PARAMETERS ---
            if (yKnobMode == 0) {
                latchedGateInt = yVal + 1; // Maps 0-4095 to 1-4096
                if (latchedGateInt < 40) latchedGateInt = 40; // ~1% minimum gate
            }
            else if (yKnobMode == 1) {
                latchedRatchets = 1 + (yVal * 4) / 4096; 
            }
            else if (yKnobMode == 2) {
                latchedJitterAmt = yVal;
            }
            else if (yKnobMode == 3) {
                if (yVal < 1365) latchedPitchState = 0; 
                else if (yVal > 2730) latchedPitchState = 2;
                else latchedPitchState = 1; 
            }
            else if (yKnobMode == 4) {
                latchedBarLength = 1 + (yVal * 8) / 4096; // 1 to 8 Quarter Notes
            }
        }

        // ---------------------------------------------------------
        // 2. STATE MANAGER & UI GESTURES
        // ---------------------------------------------------------
        if (s == Switch::Down) {
            switchHoldDownTimer++;

            if (switchHoldDownTimer >= 24000 && !reverseTriggered) {
                reverseCh2 = !reverseCh2; 
                reverseTriggered = true;
            }
            if (switchHoldDownTimer >= 72000 && !resetTriggered) {
                currentStep = 0;
                sliceTimer = 0;
                resetTriggered = true;
            }
        } else {
            if (lastSwitch == Switch::Down) {
                if (switchHoldDownTimer < 24000) {
                    yKnobMode++;
                    if (yKnobMode > 4) yKnobMode = 0; // Wraps through 5 modes now
                    yKnobActive = false; 
                }
            }
            switchHoldDownTimer = 0;
            reverseTriggered = false;
            resetTriggered = false;
        }

        if (s == Switch::Up && lastSwitch != Switch::Up) {
            currentMode = RECORD;
        } 
        else if (s == Switch::Middle && lastSwitch != Switch::Middle) {
            currentMode = SLICE;
            frozenWriteInd = writeInd; 
            playPos1 = frozenWriteInd; 
            playPos2 = frozenWriteInd;
        }
        lastSwitch = s;

        // ---------------------------------------------------------
        // 3. 24 PPQN CLOCK & DAW-SYNCED BUFFER MAXIMIZATION
        // ---------------------------------------------------------
        if (Connected(Input::Pulse1)) {
            samplesSinceLastClock++;
            
            // Auto-reset: If clock stops for > 1 sec, prime for DAW start
            if (samplesSinceLastClock > 48000) {
                clockWasStopped = true;
            }

            if (PulseInRisingEdge(1)) {
                
                if (clockWasStopped) {
                    currentStep = 0;
                    sliceTimer = 0;
                    writeInd = 0; 
                    smoothedPulseLength = samplesSinceLastClock; 
                    clockWasStopped = false;
                }

                // Smooth jitter from external analog clock signals
                uint32_t currentPulseLength = samplesSinceLastClock;
                smoothedPulseLength = (smoothedPulseLength * 3 + currentPulseLength) / 4;
                samplesSinceLastClock = 0;

                // 24 PPQN MATH: 24 pulses = 1 Quarter Note
                uint32_t beatLengthSamples = smoothedPulseLength * 24;
                uint32_t bufferSamplesPerBeat = beatLengthSamples / DOWNSAMPLE_FACTOR;
                
                // --- APPLY BAR LENGTH ---
                uint32_t targetBufferSamples = latchedBarLength * bufferSamplesPerBeat;

                // Check if the requested bar length physically fits in RAM
                if (targetBufferSamples > 0 && targetBufferSamples <= bufSize) {
                    activeBufferLength = targetBufferSamples;
                } 
                // If it's too long, fall back to the maximum whole beats that DO fit
                else if (targetBufferSamples > bufSize && bufferSamplesPerBeat <= bufSize) {
                    uint32_t maxBeats = bufSize / bufferSamplesPerBeat;
                    activeBufferLength = maxBeats * bufferSamplesPerBeat;
                } 
                else {
                    activeBufferLength = bufSize; 
                }
            }
        } else {
            activeBufferLength = bufSize; 
        }

        // --- INDEPENDENT SLICING TIMER ---
        uint32_t triggerInterval = (activeBufferLength * DOWNSAMPLE_FACTOR) / totalSlices;
        if (triggerInterval == 0) triggerInterval = 1; 

        bool triggerNextSlice = false;
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
            currentSliceLength = activeBufferLength / totalSlices;
            
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
            if (latchedJitterAmt > 0) {
                uint32_t maxJitter = currentSliceLength / 2;
                jitterOffset = (rnd12() * latchedJitterAmt * maxJitter) / (4095 * 4095);
            }

            newPlayPos1 = (frozenWriteInd + (targetSlice1 * currentSliceLength) + jitterOffset) % activeBufferLength;
            newPlayPos2 = (frozenWriteInd + (targetSlice2 * currentSliceLength) + jitterOffset) % activeBufferLength;
            
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
                if (writeInd >= activeBufferLength) writeInd = 0; 
            }
            AudioOut1(audioIn);
            AudioOut2(audioIn);
        } 
        else if (currentMode == SLICE) {
            
            // FAST INTEGER MATH for gate calculation (>> 12 is divided by 4096)
            uint32_t allowedPlaySamples = (currentSliceLength * latchedGateInt) >> 12;
            
            int ratchets = latchedRatchets;
            uint32_t ratchetInterval = triggerInterval / ratchets;

            int pitchAdvance = 1;
            if (latchedPitchState == 0) {
                pitchAdvance = 0; 
                pitchSubTick1++;
                if (pitchSubTick1 >= 2) { pitchAdvance = 1; pitchSubTick1 = 0; }
            } else if (latchedPitchState == 2) {
                pitchAdvance = 2; 
            }

            // --- CH1 DSP ---
            int32_t out1 = 0; 

            // Ratchet Trigger Check (Always runs, relies on 'ratchets' value)
            ratchetSubTimer1++;
            if (ratchetSubTimer1 >= ratchetInterval && ratchets > 1) {
                ratchetSubTimer1 = 0;
                playPos1 = newPlayPos1; 
                xfadeInd1 = xfadeLen;
            }

            // Gate Muting
            if (samplesPlayedInSlice1 < allowedPlaySamples) {
                if (xfadeInd1 > 0) {
                    int32_t oldVal = buffer[playPos1];
                    int32_t newVal = buffer[newPlayPos1];
                    out1 = (oldVal * xfadeInd1 + newVal * (xfadeLen - xfadeInd1)) / xfadeLen;
                    
                    if (dsTick == 0) {
                        playPos1 = (playPos1 + pitchAdvance) % activeBufferLength;
                        newPlayPos1 = (newPlayPos1 + pitchAdvance) % activeBufferLength;
                        samplesPlayedInSlice1++; 
                        xfadeInd1--;
                        if (xfadeInd1 == 0) playPos1 = newPlayPos1;
                    }
                } else {
                    out1 = buffer[playPos1];
                    if (dsTick == 0) {
                        playPos1 = (playPos1 + pitchAdvance) % activeBufferLength;
                        samplesPlayedInSlice1++; 
                    }
                }
            }
            AudioOut1(out1);

            // --- CH2 DSP ---
            int32_t out2 = 0; 
            
            // Ratchet Trigger Check (Always runs, relies on 'ratchets' value)
            ratchetSubTimer2++;
            if (ratchetSubTimer2 >= ratchetInterval && ratchets > 1) {
                ratchetSubTimer2 = 0;
                playPos2 = newPlayPos2;
                xfadeInd2 = xfadeLen;
            }

            if (samplesPlayedInSlice2 < allowedPlaySamples) {
                if (xfadeInd2 > 0) {
                    int32_t oldVal = buffer[playPos2];
                    int32_t newVal = buffer[newPlayPos2];
                    out2 = (oldVal * xfadeInd2 + newVal * (xfadeLen - xfadeInd2)) / xfadeLen;
                    
                    if (dsTick == 0) {
                        playPos2 = (playPos2 + pitchAdvance) % activeBufferLength;
                        newPlayPos2 = (newPlayPos2 + pitchAdvance) % activeBufferLength;
                        samplesPlayedInSlice2++; 
                        xfadeInd2--;
                        if (xfadeInd2 == 0) playPos2 = newPlayPos2;
                    }
                } else {
                    out2 = buffer[playPos2];
                    if (dsTick == 0) {
                        playPos2 = (playPos2 + pitchAdvance) % activeBufferLength;
                        samplesPlayedInSlice2++; 
                    }
                }
            }
            AudioOut2(out2);
        }
        
        // ---------------------------------------------------------
        // 6. UI INDICATORS
        // ---------------------------------------------------------
        
        // LED 0: Solid in Record Mode, Pulses with triggers in Slice Mode
        if (currentMode == RECORD) {
            LedOn(0, true);
        } else {
            LedOn(0, sliceTimer < 1000); // ~20ms flash at the start of each slice
        }

        // LED 1: Reverse Status
        LedOn(1, reverseCh2); 
        
        // LEDs 2-5: Always show the current Y-Knob Mode
        LedOn(2, yKnobMode == 0 || yKnobMode == 4); // Gate
        LedOn(3, yKnobMode == 1 || yKnobMode == 4);  // Ratchet
        LedOn(4, yKnobMode == 2 || yKnobMode == 4); // Jitter
        LedOn(5, yKnobMode == 3 || yKnobMode == 4);// Pitch

        CVOut1((currentStep * 4095) / totalSlices);
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