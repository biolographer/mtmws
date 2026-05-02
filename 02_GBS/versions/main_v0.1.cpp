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
    
    static constexpr int xfadeLen = 25; // 25-sample linear crossfade

    // Downsampling controls
    static constexpr int DOWNSAMPLE_FACTOR = 2; // 2 = 4 seconds, 3 = 6 seconds
    uint32_t dsTick = 0;
    
    // Clock & Striation Timers
    uint32_t samplesSinceLastClock = 0;
    uint32_t cycleLengthSamples = 48000;
    uint32_t sliceTimer = 0;
    
    // Turing Machine Sequencer
    uint8_t seq[64];
    uint8_t currentStep = 0;
    
    // UI & State
    enum Mode { RECORD, SLICE };
    Mode currentMode = RECORD;
    Switch lastSwitch = Switch::Down;

    // CH2 Reverse Long-Press State
    uint32_t switchHoldTimer = 0;
    bool switchHoldTriggered = false;
    bool reverseCh2 = false;

    GranularSlicer()
    {
        for (uint32_t i = 0; i < bufSize; i++) buffer[i] = 0;
        for (int i = 0; i < 64; i++) seq[i] = 0;
    }

    void __not_in_flash_func(ProcessSample)()
    {
        Switch s = SwitchVal();

        // Advance the downsample clock
        dsTick++;
        if (dsTick >= DOWNSAMPLE_FACTOR) {
            dsTick = 0;
        }

        // ---------------------------------------------------------
        // 1. Read Knobs 
        // ---------------------------------------------------------

        // X Knob: Continuous total slices (1 to 64)
        int totalSlices = (KnobVal(Knob::X) * 64) / 4096;
        if (totalSlices < 1) totalSlices = 1;

        // Y Knob: Stepped into 12 distinct zones (0 to 11)
        int yStep = (KnobVal(Knob::Y) * 12) / 4096; 
        
        // Calculate the Active Pool as a fraction: (yStep + 1) / 12
        int activePool = (totalSlices * (yStep + 1)) / 12;
        
        // Safety bounds
        if (activePool < 1) activePool = 1;
        if (activePool > totalSlices) activePool = totalSlices;


        // ---------------------------------------------------------
        // 2. STATE MANAGER & REVERSE HOLD LOGIC
        // ---------------------------------------------------------
        
        // 0.5 Second Hold Logic (48,000 samples/sec = 24,000 for 0.5s)
        if (s == Switch::Down) {
            switchHoldTimer++;
            if (switchHoldTimer >= 24000 && !switchHoldTriggered) {
                reverseCh2 = !reverseCh2; // Toggle CH2 Reverse Mode
                switchHoldTriggered = true;
            }
        } else {
            switchHoldTimer = 0;
            switchHoldTriggered = false;
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
        else if (s == Switch::Down && lastSwitch != Switch::Down) {
            // Momentary Tap: Reset grid and sequence instantly
            currentStep = 0;
            sliceTimer = 0;
        }
        lastSwitch = s;

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
            cycleLengthSamples = 96000; 
        }

        // The Grid Timing is strictly locked to X (totalSlices)
        uint32_t triggerInterval = cycleLengthSamples / totalSlices;
        if (triggerInterval == 0) triggerInterval = 1; 

        sliceTimer++;
        if (sliceTimer >= triggerInterval) {
            sliceTimer = 0;
            triggerNextSlice = true;
        }

        // ---------------------------------------------------------
        // 4. THE TURING MACHINE & POLYMETRIC PLAYHEAD JUMPS
        // ---------------------------------------------------------
        
        if (currentMode == SLICE && triggerNextSlice) {
            int mainKnob = KnobVal(Knob::Main);
            
            // TURING LOGIC (Wraps sequence around Y!)
            if (mainKnob < 100) {
                // Play sequentially, wrapped at Y. If X=8, Y=3: [0, 1, 2, 0, 1, 2, 0, 1]
                seq[currentStep] = currentStep % activePool; 
            } 
            else if (mainKnob > 4000) {
                // Loop locked, preserve array
            } 
            else {
                int prob;
                if (mainKnob <= 2048) {
                    prob = (mainKnob - 100) * 4095 / 1948; 
                } else {
                    prob = (4000 - mainKnob) * 4095 / 1952; 
                }
                
                if (rnd12() < prob) {
                    seq[currentStep] = rnd12() % activePool; 
                }
            }

            // CH 1 reads normally. CH 2 reads the sequence array backward if reversed!
            uint32_t targetSlice1 = seq[currentStep];
            uint32_t targetSlice2 = reverseCh2 ? seq[(totalSlices - 1) - currentStep] : targetSlice1;

            uint32_t sliceLength = bufSize / totalSlices;
            newPlayPos1 = (frozenWriteInd + (targetSlice1 * sliceLength)) % bufSize;
            newPlayPos2 = (frozenWriteInd + (targetSlice2 * sliceLength)) % bufSize;
            
            xfadeInd1 = xfadeLen; 
            xfadeInd2 = xfadeLen;
            
            // Grid loops perfectly at X
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
            // ONLY write to memory on the downsampled tick
            if (dsTick == 0) {
                buffer[writeInd] = audioIn;
                writeInd++;
                if (writeInd >= bufSize) writeInd = 0;
            }
            
            // Pass the high-res 48kHz audio straight to the speaker so monitoring sounds clean
            AudioOut1(audioIn);
            AudioOut2(audioIn);
            LedOn(0, true); 
        } 
        else if (currentMode == SLICE) {
            LedOn(0, false);
            
            // CH1 PLAYHEAD
            int32_t out1;
            if (xfadeInd1 > 0) {
                int32_t oldVal = buffer[playPos1];
                int32_t newVal = buffer[newPlayPos1];
                out1 = (oldVal * xfadeInd1 + newVal * (xfadeLen - xfadeInd1)) / xfadeLen;
                
                // ONLY advance the playhead on the downsampled tick
                if (dsTick == 0) {
                    playPos1 = (playPos1 + 1) % bufSize;
                    newPlayPos1 = (newPlayPos1 + 1) % bufSize;
                    xfadeInd1--;
                    if (xfadeInd1 == 0) playPos1 = newPlayPos1;
                }
            } else {
                out1 = buffer[playPos1];
                if (dsTick == 0) playPos1 = (playPos1 + 1) % bufSize;
            }
            AudioOut1(out1);

            // CH2 PLAYHEAD
            int32_t out2;
            if (xfadeInd2 > 0) {
                int32_t oldVal = buffer[playPos2];
                int32_t newVal = buffer[newPlayPos2];
                out2 = (oldVal * xfadeInd2 + newVal * (xfadeLen - xfadeInd2)) / xfadeLen;
                
                if (dsTick == 0) {
                    playPos2 = (playPos2 + 1) % bufSize;
                    newPlayPos2 = (newPlayPos2 + 1) % bufSize;
                    xfadeInd2--;
                    if (xfadeInd2 == 0) playPos2 = newPlayPos2;
                }
            } else {
                out2 = buffer[playPos2];
                if (dsTick == 0) playPos2 = (playPos2 + 1) % bufSize;
            }
            AudioOut2(out2);
        }
        
        // UI Indicators
        LedOn(1, reverseCh2); 
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