#include "ComputerCard.h"
#include "tusb.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"

// Define the MIDI Message structure and thread-safe queue
struct MIDIMessage {
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
};
queue_t midi_queue;

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
    
    // Rhythmic Buffer Sizes
    uint32_t activeBufferLength = bufSize; 
    uint32_t ch2ActiveBufferLength = bufSize;

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

    // --- MIDI Clock Trackers ---
    bool midiClockActive = false;
    uint32_t midiClockTimeout = 0;

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
    uint8_t currentStep2 = 0; 
    
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
    // 0: Gate, 1: Ratchet, 2: Jitter, 3: Pitch, 4: Bar Length, 5: Phase Window
    uint8_t yKnobMode = 4; 
    
    bool yKnobActive = true;
    int32_t latchedYKnobRaw[6] = {4095, 0, 0, 2048, 1536, 0}; 
    
    uint32_t latchedGateInt = 4096; 
    int latchedRatchets = 1;
    int latchedPitchState = 1; 
    int latchedJitterAmt = 0;
    int latchedBarLength = 4;
    uint32_t latchedPhaseWindow = 0; 

    GranularSlicer()
    {
        for (uint32_t i = 0; i < bufSize; i++) buffer[i] = 0;
        for (int i = 0; i < 64; i++) seq[i] = 0;
    }

    void __not_in_flash_func(ProcessSample)()
    {
        Switch s = SwitchVal();
        dsTick++;
        if (dsTick >= DOWNSAMPLE_FACTOR) {
            dsTick = 0;
        }

        // ---------------------------------------------------------
        // 1. Read Knobs, CV & Update Latched Memory
        // ---------------------------------------------------------
        
        // CV 1 restores X-Knob modulation
        int32_t combinedX = KnobVal(Knob::X) + CVIn1();
        if (combinedX < 0) combinedX = 0;
        if (combinedX > 4095) combinedX = 4095;
        
        int totalSlices = (combinedX * 64) / 4096;
        if (totalSlices < 1) totalSlices = 1;

        int32_t yVal = KnobVal(Knob::Y);

        if (!yKnobActive) {
            int32_t targetVal = latchedYKnobRaw[yKnobMode];
            int32_t diff = yVal - targetVal;
            if (diff > -100 && diff < 100) yKnobActive = true;
        }

        if (yKnobActive) {
            latchedYKnobRaw[yKnobMode] = yVal; 
            if (yKnobMode == 0) { latchedGateInt = yVal + 1; if (latchedGateInt < 40) latchedGateInt = 40; }
            else if (yKnobMode == 1) latchedRatchets = 1 + (yVal * 4) / 4096; 
            else if (yKnobMode == 2) latchedJitterAmt = yVal;
            else if (yKnobMode == 3) {
                if (yVal < 1365) latchedPitchState = 0; 
                else if (yVal > 2730) latchedPitchState = 2;
                else latchedPitchState = 1; 
            }
            else if (yKnobMode == 4) latchedBarLength = 1 + (yVal * 8) / 4096; 
            else if (yKnobMode == 5) {
                int32_t newPhaseWindow = yVal;
                
                // Add Deadzone to ensure it hits exactly 0
                if (newPhaseWindow < 20) newPhaseWindow = 0;
                
                // Snap-to-Sync: Instantly realign patterns when returned to 0
                if (newPhaseWindow == 0 && latchedPhaseWindow > 0) {
                    currentStep2 = currentStep;
                }
                
                latchedPhaseWindow = newPhaseWindow; 
            } 
        }

        // ---------------------------------------------------------
        // 2. STATE MANAGER & UI GESTURES
        // ---------------------------------------------------------
        if (s == Switch::Down) {
            switchHoldDownTimer++;
            if (switchHoldDownTimer >= 24000 && !reverseTriggered) { reverseCh2 = !reverseCh2; reverseTriggered = true; }
            if (switchHoldDownTimer >= 72000 && !resetTriggered) { currentStep = 0; currentStep2 = 0; sliceTimer = 0; resetTriggered = true; }
        } else {
            if (lastSwitch == Switch::Down) {
                if (switchHoldDownTimer < 24000) { yKnobMode++; if (yKnobMode > 5) yKnobMode = 0; yKnobActive = false; }
            }
            switchHoldDownTimer = 0;
            reverseTriggered = false;
            resetTriggered = false;
        }

        if (s == Switch::Up && lastSwitch != Switch::Up) currentMode = RECORD;
        else if (s == Switch::Middle && lastSwitch != Switch::Middle) {
            currentMode = SLICE; frozenWriteInd = writeInd; playPos1 = frozenWriteInd; playPos2 = frozenWriteInd;
        }
        lastSwitch = s;

        // ---------------------------------------------------------
        // 3. PRIORITY CLOCK SYNC (Physical Pulse > USB MIDI)
        // ---------------------------------------------------------
        bool physicalClockConnected = Connected(Input::Pulse1);
        bool clockTriggered = false;

        MIDIMessage msg;
        while (queue_try_remove(&midi_queue, &msg)) {
            if (msg.status == 0xF8) { 
                midiClockActive = true;
                midiClockTimeout = 0;
                if (!physicalClockConnected) clockTriggered = true;
            } 
            else if (msg.status == 0xFA || msg.status == 0xFC) { 
                if (!physicalClockConnected) clockWasStopped = true;
            }
        }

        if (midiClockActive) {
            midiClockTimeout++;
            if (midiClockTimeout > 96000) midiClockActive = false;
        }

        if (physicalClockConnected) {
            if (PulseInRisingEdge(1)) {
                clockTriggered = true;
            }
        }

        bool isClocked = physicalClockConnected || midiClockActive;

        if (isClocked) {
            samplesSinceLastClock++;
            if (samplesSinceLastClock > 48000) clockWasStopped = true;

            if (clockTriggered) {
                if (clockWasStopped) {
                    currentStep = 0; currentStep2 = 0; sliceTimer = 0; writeInd = 0; 
                    smoothedPulseLength = samplesSinceLastClock; 
                    clockWasStopped = false;
                }

                uint32_t currentPulseLength = samplesSinceLastClock;
                smoothedPulseLength = (smoothedPulseLength * 3 + currentPulseLength) / 4;
                samplesSinceLastClock = 0;

                uint32_t beatLengthSamples = smoothedPulseLength * 24;
                uint32_t bufferSamplesPerBeat = beatLengthSamples / DOWNSAMPLE_FACTOR;
                uint32_t targetBufferSamples = latchedBarLength * bufferSamplesPerBeat;

                if (targetBufferSamples > 0 && targetBufferSamples <= bufSize) activeBufferLength = targetBufferSamples;
                else if (targetBufferSamples > bufSize && bufferSamplesPerBeat <= bufSize) {
                    activeBufferLength = (bufSize / bufferSamplesPerBeat) * bufferSamplesPerBeat;
                } else activeBufferLength = bufSize; 

                if (bufferSamplesPerBeat > 0 && activeBufferLength >= bufferSamplesPerBeat) {
                    uint32_t currentTotalBeats = activeBufferLength / bufferSamplesPerBeat;
                    if (currentTotalBeats < 1) currentTotalBeats = 1;
                    uint32_t ch2Beats = 1 + (((4096 - latchedPhaseWindow) * (currentTotalBeats - 1)) >> 12);
                    ch2ActiveBufferLength = ch2Beats * bufferSamplesPerBeat;
                } else ch2ActiveBufferLength = activeBufferLength;
            }
        } else {
            activeBufferLength = ch2ActiveBufferLength = bufSize; 
        }

        uint32_t triggerInterval = (activeBufferLength * DOWNSAMPLE_FACTOR) / totalSlices;
        if (triggerInterval == 0) triggerInterval = 1; 

        bool triggerNextSlice = false;
        sliceTimer++;
        if (sliceTimer >= triggerInterval) {
            sliceTimer = 0; triggerNextSlice = true;
        }

        // ---------------------------------------------------------
        // 4. THE TURING MACHINE & POLYMETRIC JUMPS
        // ---------------------------------------------------------
        if (currentMode == SLICE && triggerNextSlice) {
            int mainKnob = KnobVal(Knob::Main);
            currentSliceLength = activeBufferLength / totalSlices;
            
            if (mainKnob < 100) seq[currentStep] = currentStep % totalSlices; 
            else if (mainKnob <= 4000) {
                int prob = (mainKnob <= 2048) ? (mainKnob - 100) * 4095 / 1948 : (4000 - mainKnob) * 4095 / 1952;
                if (rnd12() < prob) seq[currentStep] = rnd12() % totalSlices; 
            }

            uint32_t ch2TotalSlices = totalSlices; 
            if (activeBufferLength > 0) ch2TotalSlices = (totalSlices * ch2ActiveBufferLength) / activeBufferLength;
            if (ch2TotalSlices < 1) ch2TotalSlices = 1;

            uint32_t targetSlice1 = seq[currentStep];
            uint32_t targetSlice2 = reverseCh2 ? seq[(ch2TotalSlices - 1) - currentStep2] : seq[currentStep2];

            // Restored CV 2 into Jitter calculation
            uint32_t jitterOffset = 0;
            int32_t activeJitter = latchedJitterAmt + CVIn2();
            if (activeJitter < 0) activeJitter = 0;
            if (activeJitter > 4095) activeJitter = 4095;

            if (activeJitter > 0) {
                uint32_t maxJitter = currentSliceLength / 2;
                // Cast to uint64_t prevents integer overflow before dividing
                jitterOffset = ((uint64_t)rnd12() * activeJitter * maxJitter) / (4095 * 4095);
            }

            newPlayPos1 = (frozenWriteInd + (targetSlice1 * currentSliceLength) + jitterOffset) % activeBufferLength;
            newPlayPos2 = (frozenWriteInd + (targetSlice2 * currentSliceLength) + jitterOffset) % activeBufferLength;
            
            xfadeInd1 = xfadeLen; xfadeInd2 = xfadeLen;
            samplesPlayedInSlice1 = 0; samplesPlayedInSlice2 = 0;
            ratchetSubTimer1 = 0; ratchetSubTimer2 = 0;
            
            currentStep++; if (currentStep >= totalSlices) currentStep = 0;
            currentStep2++; if (currentStep2 >= ch2TotalSlices) currentStep2 = 0;
            
            PulseOut1(true); PulseOut2(true);
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
            AudioOut1(audioIn); AudioOut2(audioIn);
            PulseOut1(false); PulseOut2(false);
        } 
        else if (currentMode == SLICE) {
            uint32_t allowedPlaySamples = (currentSliceLength * latchedGateInt) >> 12;
            int ratchets = latchedRatchets;
            uint32_t ratchetInterval = triggerInterval / ratchets;
            
            int pitchAdvance = 1;
            if (latchedPitchState == 2) {
                pitchAdvance = 2; // Double speed (+1 Octave)
            } else if (latchedPitchState == 0) {
                pitchAdvance = pitchSubTick1; // Toggles between 0 and 1 (-1 Octave)
            }


            // --- CH1 DSP ---
            int32_t out1 = 0; 
            if (++ratchetSubTimer1 >= ratchetInterval && ratchets > 1) { 
                ratchetSubTimer1 = 0; playPos1 = newPlayPos1; xfadeInd1 = xfadeLen; PulseOut1(true); 
            }
            if (samplesPlayedInSlice1 < allowedPlaySamples) {
                if (xfadeInd1 > 0) {
                    out1 = (buffer[playPos1] * xfadeInd1 + buffer[newPlayPos1] * (xfadeLen - xfadeInd1)) / xfadeLen;
                    if (dsTick == 0) { playPos1 = (playPos1 + pitchAdvance) % activeBufferLength; newPlayPos1 = (newPlayPos1 + pitchAdvance) % activeBufferLength; samplesPlayedInSlice1++; xfadeInd1--; if (xfadeInd1 == 0) playPos1 = newPlayPos1; }
                } else {
                    out1 = buffer[playPos1];
                    if (dsTick == 0) { playPos1 = (playPos1 + pitchAdvance) % activeBufferLength; samplesPlayedInSlice1++; }
                }
            } else PulseOut1(false);
            AudioOut1(out1);

            // --- CH2 DSP ---
            int32_t out2 = 0; 
            if (++ratchetSubTimer2 >= ratchetInterval && ratchets > 1) { 
                ratchetSubTimer2 = 0; playPos2 = newPlayPos2; xfadeInd2 = xfadeLen; PulseOut2(true); 
            }
            if (samplesPlayedInSlice2 < allowedPlaySamples) {
                if (xfadeInd2 > 0) {
                    out2 = (buffer[playPos2] * xfadeInd2 + buffer[newPlayPos2] * (xfadeLen - xfadeInd2)) / xfadeLen;
                    if (dsTick == 0) { playPos2 = (playPos2 + pitchAdvance) % activeBufferLength; newPlayPos2 = (newPlayPos2 + pitchAdvance) % activeBufferLength; samplesPlayedInSlice2++; xfadeInd2--; if (xfadeInd2 == 0) playPos2 = newPlayPos2; }
                } else {
                    out2 = buffer[playPos2];
                    if (dsTick == 0) { playPos2 = (playPos2 + pitchAdvance) % activeBufferLength; samplesPlayedInSlice2++; }
                }
            } else PulseOut2(false);
            AudioOut2(out2);

            // Toggle the sub-tick flag every downsampled frame so half-speed works
            if (dsTick == 0) {
                pitchSubTick1 ^= 1; 
            }
            
        }
        
        // ---------------------------------------------------------
        // 6. UI INDICATORS
        // ---------------------------------------------------------
        LedOn(0, reverseCh2); 
        LedOn(1, yKnobMode == 5 || yKnobMode == 4);
        LedOn(2, yKnobMode == 0 || yKnobMode == 4); 
        LedOn(3, yKnobMode == 1 || yKnobMode == 4); 
        LedOn(4, yKnobMode == 2 || yKnobMode == 4); 
        LedOn(5, yKnobMode == 3 || yKnobMode == 4); 

        CVOut1((currentStep * 4095) / totalSlices);
    }
};

// Global pointer for Core 1 to access the looper
GranularSlicer* looper_ptr = nullptr;

void core1_entry() {
    looper_ptr->Run(); 
}

int main()
{
    set_sys_clock_khz(160000, true);
    stdio_init_all();
    tusb_init();

    queue_init(&midi_queue, sizeof(MIDIMessage), 32);

    GranularSlicer slicer;
    looper_ptr = &slicer;
    slicer.EnableNormalisationProbe();

    multicore_launch_core1(core1_entry);

    while (1) {
        tud_task(); 
        
        if (tud_midi_available()) {
            uint8_t packet[4];
            while (tud_midi_packet_read(packet)) {
                MIDIMessage msg = { packet[1], packet[2], packet[3] };
                queue_try_add(&midi_queue, &msg);
            }
        }
    }
    return 0;
}