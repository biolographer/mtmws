#include <cstdint>
#include "ComputerCard.h"
#include "corpus_data.h"

constexpr int DOWNSAMPLE_FACTOR = 2; 
constexpr int NUM_VOICES = 2; // Number of overlapping samples allowed

// 1. Define a struct to hold the state of a single playing sound
struct Voice {
    const int16_t* data = nullptr;
    uint32_t length = 0;
    uint32_t playhead = 0;
    bool is_playing = false;
};

class CorpusExplorer : public ComputerCard {
private:
    // 2. Create our pool of voices and a tracker for round-robin allocation
    Voice voices[NUM_VOICES];
    int next_voice = 0; 

    int32_t held_x = 0;
    int32_t held_y = 0;
    // Add these state variables to your class to "hold" the pot values
    int32_t base_x = 2048; 
    int32_t base_y = 2048;
    int32_t spread = 0; 

    // Your "Page 2" variables (for the UP position)
    int32_t secondary_param_x = 0; 
    int32_t secondary_param_y = 0;
    int32_t secondary_param_main = 0;

    inline int find_nearest_sample(int32_t target_x, int32_t target_y) {
        if (NUM_SAMPLES <= 0) return 0;
        int best_index = 0;
        int32_t min_dist_sq = INT32_MAX; 
        for (int i = 0; i < NUM_SAMPLES; i++) {
            int32_t dx = target_x - corpus[i].x;
            int32_t dy = target_y - corpus[i].y;
            int32_t dist_sq = (dx * dx) + (dy * dy);
            if (dist_sq < min_dist_sq) {
                min_dist_sq = dist_sq;
                best_index = i;
            }
        }
        return best_index;
    }

public:
    void ProcessSample() override {
        // --- 1. UI PAGING LOGIC ---
        if (SwitchUp()) {
            // "PAGE 2" - Knobs control your extra parameters
            secondary_param_x = PotX();
            secondary_param_y = PotY();
            secondary_param_main = PotMain();
        } 
        else if (!SwitchDown()) { // Assuming Middle position (Not Up, Not Down)
            // "PAGE 1" - Knobs control navigation and spread
            base_x = PotX();
            base_y = PotY();
            spread = PotMain(); // 0 to 4095
        }
        
        // --- TRIGGER LOGIC ---
        if (PulseIn1RisingEdge()) {
            // Calculate modulation magnitude based on the Spread (Main) knob.
            // Multiply CV by Spread, then divide by 4096 (using >> 12 for CPU speed).
            // If Spread is 0, cv_mod is 0. If Spread is max, cv_mod is 100% of CVIn.
            int32_t cv_mod_x = (CVIn1() * spread) >> 12;
            int32_t cv_mod_y = (CVIn2() * spread) >> 12;
            
            held_x = base_x + cv_mod_x; 
            held_y = base_y + cv_mod_y;

            // --- 3. BOUNDARY HANDLING (The "Pac-Man" Wrap) ---
            // Assuming your map coordinates go from 0 to 4095
            constexpr int32_t MAP_MAX = 4096;

            // Wrap X axis
            while (held_x >= MAP_MAX) held_x -= MAP_MAX;
            while (held_x < 0) held_x += MAP_MAX;

            // Wrap Y axis
            while (held_y >= MAP_MAX) held_y -= MAP_MAX;
            while (held_y < 0) held_y += MAP_MAX;

            // --- OR: BOUNDARY HANDLING (Clamping) ---
            /* Use this instead of the while loops if you want it to hit a wall:
            if (held_x > 4095) held_x = 4095;
            if (held_x < 0) held_x = 0;
            if (held_y > 4095) held_y = 4095;
            if (held_y < 0) held_y = 0;
            */

            int target = find_nearest_sample(held_x, held_y);

            // 3. VOICE ALLOCATION (Round-Robin Stealing)
            // We assign the new sample to 'next_voice'. 
            // If it was already playing an old sample, it gets instantly overwritten (stolen).
            voices[next_voice].data = corpus[target].data;
            voices[next_voice].length = corpus[target].length;
            voices[next_voice].playhead = 0;
            voices[next_voice].is_playing = true;

            // Move the pointer to the next voice, wrapping back to 0 if it hits the limit
            next_voice = (next_voice + 1) % NUM_VOICES;
            
            LedOn(0); 
        }

        // --- AUDIO PLAYBACK & MIXING ---
        // Use a 32-bit integer to accumulate the sum of all voices to prevent overflow
        int32_t mixed_sample = 0; 
        bool anything_playing = false;

        // Loop through all voices
        for (int i = 0; i < NUM_VOICES; i++) {
            if (voices[i].is_playing) {
                anything_playing = true;
                uint32_t flash_index = voices[i].playhead / DOWNSAMPLE_FACTOR;
                
                if (flash_index < voices[i].length) {
                    // Add this voice's current sample to the master mix
                    mixed_sample += voices[i].data[flash_index];
                    voices[i].playhead++; 
                } else {
                    // Voice finished its sample
                    voices[i].is_playing = false; 
                }
            }
        }

        if (anything_playing) {
            // 4. GAIN STAGING & CLAMPING
            // Because we are adding up to 4 samples together, the volume could be 4x too loud.
            // A simple approach is to attenuate the master mix slightly. 
            mixed_sample = mixed_sample >> 1;

            // Hard clip to ensure we absolutely never exceed 16-bit limits
            if (mixed_sample > INT16_MAX) mixed_sample = INT16_MAX;
            if (mixed_sample < INT16_MIN) mixed_sample = INT16_MIN;

            AudioOut1((int16_t)mixed_sample);
            AudioOut2((int16_t)mixed_sample);
        } else {
            AudioOut1(0);
            AudioOut2(0);
            LedOff(0);
        }
    }
};

int main() {
    CorpusExplorer moduleProgram;
    moduleProgram.Run(); 
    return 0;
}