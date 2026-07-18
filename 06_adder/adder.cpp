#include "ComputerCard.h"
#include <cmath>

// ========================================================
// 1. DATA STRUCTURES & CONFIGURATION
// ========================================================
constexpr int NUM_PARTIALS = 8;
constexpr int SCALE_STEPS = 12;
constexpr int NUM_PROFILES = 3; 

// We use 32-bit phase accumulators for the oscillators
constexpr uint32_t MAX_PHASE = 0xFFFFFFFF;
// Pre-computed Sine Table for fast oscillator lookup
constexpr int SINETABLE_SIZE = 1024;
int16_t sineTable[SINETABLE_SIZE];

struct InstrumentProfile {
    float partial_ratios[NUM_PARTIALS];
    float partial_amps[NUM_PARTIALS];
    float optimal_scale[SCALE_STEPS]; 
};

// You will copy/paste your Python-calculated data in here later!
constexpr InstrumentProfile profiles[NUM_PROFILES] = {
    { // Profile 0: Harmonic / String
        {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f},
        {1.0f, 0.8f, 0.6f, 0.4f, 0.3f, 0.2f, 0.1f, 0.05f},
        {1.0f, 1.059f, 1.122f, 1.189f, 1.260f, 1.335f, 1.414f, 1.498f, 1.587f, 1.682f, 1.782f, 1.888f} // Standard 12-TET for now
    },
    { // Profile 1: Semi-Inharmonic (Placeholder)
        {1.0f, 1.58f, 2.74f, 3.2f, 4.1f, 5.0f, 6.2f, 7.1f},
        {1.0f, 0.9f, 0.7f, 0.5f, 0.4f, 0.2f, 0.1f, 0.05f},
        {1.0f, 1.059f, 1.122f, 1.189f, 1.260f, 1.335f, 1.414f, 1.498f, 1.587f, 1.682f, 1.782f, 1.888f}
    },
    { // Profile 2: Pure Inharmonic / Bell (Placeholder)
        {1.0f, 2.3f, 3.5f, 4.7f, 5.9f, 7.2f, 8.5f, 9.7f},
        {1.0f, 0.9f, 0.8f, 0.6f, 0.5f, 0.4f, 0.2f, 0.1f},
        {1.0f, 1.059f, 1.122f, 1.189f, 1.260f, 1.335f, 1.414f, 1.498f, 1.587f, 1.682f, 1.782f, 1.888f} 
    }
};

// ========================================================
// 2. THE MAIN MODULE CLASS
// ========================================================
class PsychoAdditive : virtual public ComputerCard {
private:
    uint32_t phaseParams[2][NUM_PARTIALS]; // Phase accumulators for Voice 1 & 2
    float vactrolEnv[2] = {0.0f, 0.0f};    // LPG Envelopes
    
    // Live calculated, interpolated data
    float active_ratios[NUM_PARTIALS];
    float active_amps[NUM_PARTIALS];
    float active_scale[SCALE_STEPS];

    // Helper: Convert V/Oct to Frequency (Fast approximation or Math)
    float VOctToFreq(float voct) {
        // Base C2 = 65.4Hz at 0V
        return 65.406f * powf(2.0f, voct);
    }
    
    // Helper: Quantize to our active interpolated scale
    float Quantize(float v_octave) {
        float octave = floorf(v_octave);
        float remainder = v_octave - octave;
        
        // Convert the 0-1 voltage remainder to a frequency ratio (between 1.0 and 2.0)
        float ratio_to_quantize = powf(2.0f, remainder);
        
        // Find closest step in active_scale
        float min_dist = 9999.0f;
        float best_ratio = active_scale[0];
        
        for (int i = 0; i < SCALE_STEPS; i++) {
            float dist = fabsf(active_scale[i] - ratio_to_quantize);
            if (dist < min_dist) {
                min_dist = dist;
                best_ratio = active_scale[i];
            }
        }
        
        // Convert back to V/Oct
        return octave + log2f(best_ratio);
    }

public:
    PsychoAdditive() {
        // Pre-compute sine table
        for (int i = 0; i < SINETABLE_SIZE; i++) {
            sineTable[i] = (int16_t)(sinf((i / (float)SINETABLE_SIZE) * 2.0f * M_PI) * 32767.0f);
        }
        for(int v=0; v<2; v++) 
            for(int p=0; p<NUM_PARTIALS; p++) phaseParams[v][p] = 0;
            
        EnableNormalisationProbe();
    }

    void __not_in_flash_func(ProcessSample)() override {
        // 1. READ INPUTS
        // Knob::Y is global pitch offset. 2048 is center (0V).
        float basePitchVect = (KnobVal(Knob::Y) - 2048) / 409.6f; 
        
        // Read 1V/Oct inputs for both voices (CVIn is -2048 to 2047, scaled to typical V/Oct)
        float cv1_voct = CVIn1() / 409.6f;
        float cv2_voct = CVIn2() / 409.6f;
        
        // 2. TIMBRE MORPHING MATH
        // Read Morph value (Knob X + Audio 1 In) and map to 0.0 -> (NUM_PROFILES-1)
        float targetMorph = (KnobVal(Knob::X) + AudioIn1()) / 4095.0f;
        if(targetMorph < 0.0f) targetMorph = 0.0f;
        if(targetMorph > 1.0f) targetMorph = 1.0f;
        
        float morphIndex = targetMorph * (NUM_PROFILES - 1.0f);
        int indexA = (int)floorf(morphIndex);
        int indexB = indexA + 1;
        if (indexB >= NUM_PROFILES) indexB = NUM_PROFILES - 1;
        float blend = morphIndex - indexA;

        // Perform linear interpolation (LERP) for active overtone structures and scales
        for (int i = 0; i < NUM_PARTIALS; i++) {
            active_ratios[i] = profiles[indexA].partial_ratios[i] + blend * (profiles[indexB].partial_ratios[i] - profiles[indexA].partial_ratios[i]);
            active_amps[i]   = profiles[indexA].partial_amps[i] + blend * (profiles[indexB].partial_amps[i] - profiles[indexA].partial_amps[i]);
        }
        for (int i = 0; i < SCALE_STEPS; i++) {
             active_scale[i] = profiles[indexA].optimal_scale[i] + blend * (profiles[indexB].optimal_scale[i] - profiles[indexA].optimal_scale[i]);
        }

        // 3. OVERTONE AMPLITUDE LOGIC
        // Main Knob + AudioIn2 controls how loud the upper partials are. Deadzone at bottom.
        float overToneDepth = (KnobVal(Knob::Main) + AudioIn2()) / 4095.0f;
        if(overToneDepth < 0.05f) overToneDepth = 0.0f; // Deadzone

        // 4. QUANTIZATION & PITCH CALCULATION
        Switch sw = SwitchVal();
        float finalPitch1 = basePitchVect + cv1_voct;
        float finalPitch2 = basePitchVect + cv2_voct;
        
        if (sw != Switch::Up) { // If not free running...
            // If momentary Down, quantize to active (interpolated) dissonance scale
            // If Middle, we could load a static 12-TET scale, but for simplicity here we just call Quantize.
            // (You can add standard 12-TET logic here)
            finalPitch1 = Quantize(finalPitch1);
            finalPitch2 = Quantize(finalPitch2);
        }

        // Echo the quantized pitch to the CV outputs
        CVOut1Millivolts((int32_t)(finalPitch1 * 1000.0f));
        CVOut2Millivolts((int32_t)(finalPitch2 * 1000.0f));

        // 5. ENVELOPES (LPG)
        // Check triggers for Voice 1
        if (Disconnected(Input::Pulse1)) {
            vactrolEnv[0] = 1.0f; // Drone
        } else {
            if (PulseIn1RisingEdge()) vactrolEnv[0] = 1.0f; // Pluck
            // Pitch-tied decay: higher pitch = faster decay
            vactrolEnv[0] *= (0.9999f - (finalPitch1 * 0.00001f)); 
        }

        // Check triggers for Voice 2
        if (Disconnected(Input::Pulse2)) {
            vactrolEnv[1] = 1.0f; // Drone
        } else {
            if (PulseIn2RisingEdge()) vactrolEnv[1] = 1.0f;
            vactrolEnv[1] *= (0.9999f - (finalPitch2 * 0.00001f));
        }

        // 6. SYNTHESIS ENGINE (Phase Accumulators & Lookup)
        float freqV1 = VOctToFreq(finalPitch1);
        float freqV2 = VOctToFreq(finalPitch2);
        
        int32_t mixOut[2] = {0, 0};

        for (int v = 0; v < 2; v++) {
            float baseFreq = (v == 0) ? freqV1 : freqV2;
            
            for (int p = 0; p < NUM_PARTIALS; p++) {
                // Determine partial amplitude (Fundamental is always 1.0, others scaled by overToneDepth)
                float amp = (p == 0) ? 1.0f : active_amps[p] * overToneDepth;
                if (amp < 0.001f) continue; // Skip silent partials to save CPU
                
                // Calculate phase increment
                float partialFreq = baseFreq * active_ratios[p];
                // Formula: Increment = (Freq * MAX_PHASE) / SampleRate
                uint32_t increment = (uint32_t)((partialFreq * 4294967296.0f) / 48000.0f);
                
                phaseParams[v][p] += increment;
                
                // Extract top 10 bits (0-1023) for wavetable index
                uint32_t index = phaseParams[v][p] >> 22; 
                
                mixOut[v] += (int32_t)(sineTable[index] * amp);
            }
            
            // Apply LPG Envelope and scale down (division by 8 prevents clipping with 8 partials)
            mixOut[v] = (mixOut[v] >> 3) * vactrolEnv[v];
        }

        // 7. OUTPUT AUDIO
        AudioOut1(mixOut[0]);
        AudioOut2(mixOut[1]);
        
        // Bonus LED Feedback: brightness based on LPG envelope
        LedBrightness(0, vactrolEnv[0] * 4095);
        LedBrightness(1, vactrolEnv[1] * 4095);
    }
};

int main() {
    PsychoAdditive program;
    program.Run();
    return 0;
}