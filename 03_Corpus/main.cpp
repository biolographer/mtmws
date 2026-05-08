#include <cstdint>
#include "ComputerCard.h"
#include "corpus_data.h"

constexpr int DOWNSAMPLE_FACTOR = 2; 
constexpr int NUM_VOICES = 4; // Bumped to 4 to drastically reduce "voice stealing" clicks

// --- 1. ENVELOPE STATES ---
enum EnvState { OFF, ATTACK, DECAY };

// 2. Define a struct to hold the state of a single playing sound
struct Voice {
    const int16_t* data = nullptr;
    uint32_t length = 0;
    uint32_t playhead = 0;
    bool is_playing = false;
    
    // VCA State Variables
    EnvState env_state = OFF;
    int32_t env_level = 0; // Ranges from 0 (silence) to 65535 (full volume)
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

    // "Page 2" variables (for the UP position)
    // x = Attack Time, y = Decay Time
    int32_t secondary_param_x = 0; 
    int32_t secondary_param_y = 0;
    int32_t secondary_param_main = 0;

    inline int find_nearest_sample(int32_t target_x, int32_t target_y) {
        if (NUM_SAMPLES <= 0) return -1;
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
            secondary_param_x = PotX(); // Attack
            secondary_param_y = PotY(); // Decay
            secondary_param_main = PotMain();
        } 
        else if (!SwitchDown()) { 
            base_x = PotX();
            base_y = PotY();
            spread = PotMain(); 
        }
        
        // --- TRIGGER LOGIC ---
        if (PulseIn1RisingEdge()) {
            int32_t cv_mod_x = (CVIn1() * spread) / 4096;
            int32_t cv_mod_y = (CVIn2() * spread) / 4096;
            
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

            if (target >= 0) {
                // 3. VOICE ALLOCATION (Round-Robin Stealing)
                // We assign the new sample to 'next_voice'. 
                // If it was already playing an old sample, it gets instantly overwritten (stolen).
                voices[next_voice].data = corpus[target].data;
                voices[next_voice].length = corpus[target].length;
                voices[next_voice].playhead = 0;
                voices[next_voice].is_playing = true;
                
                // Initialize the VCA to prevent start clicks
                voices[next_voice].env_state = ATTACK;
                voices[next_voice].env_level = 0; 

                next_voice = (next_voice + 1) % NUM_VOICES;
                LedOn(0); 
            }
        }

        // --- AUDIO PLAYBACK & MIXING ---
        int32_t mixed_sample = 0; 
        bool anything_playing = false;

        // Calculate Envelope speeds from Page 2 UI
        // Attack: Pot = 0 is fast (instant), Pot = 4095 is slow
        int32_t attack_step = 4096 - secondary_param_x; 
        if (attack_step < 1) attack_step = 1;
        
        // Decay: Pot = 0 is fast, Pot = 4095 is long (~2.7 seconds max)
        int32_t decay_step = (4096 - secondary_param_y) / 2; 
        if (decay_step < 1) decay_step = 1;

        // Loop through all voices
        for (int i = 0; i < NUM_VOICES; i++) {
            if (voices[i].is_playing) {
                anything_playing = true;
                uint32_t flash_index = voices[i].playhead / DOWNSAMPLE_FACTOR;
                
                if (flash_index < voices[i].length) {
                    
                    // --- 3. THE VCA ENVELOPE LOGIC ---
                    if (voices[i].env_state == ATTACK) {
                        voices[i].env_level += attack_step;
                        if (voices[i].env_level >= 65535) {
                            voices[i].env_level = 65535; // Cap at max volume
                            voices[i].env_state = DECAY; // Move to next phase
                        }
                    } 
                    else if (voices[i].env_state == DECAY) {
                        voices[i].env_level -= decay_step;
                        if (voices[i].env_level <= 0) {
                            voices[i].env_level = 0;
                            voices[i].is_playing = false; // Envelope finished, kill voice
                        }
                    }

                    // --- 4. END-OF-SAMPLE ANTI-CLICK ---
                    // If the user sets a long decay, but the audio file is short, it will click when it ends.
                    // This forces a rapid fade-out over the final 256 samples of the audio block.
                    uint32_t samples_left = voices[i].length - flash_index;
                    if (samples_left < 256) {
                        voices[i].env_level = (voices[i].env_level * samples_left) / 256;
                    }

                    // --- 5. APPLY VCA ---
                    int32_t raw_sample = voices[i].data[flash_index];
                    
                    // Multiply audio by envelope. (We bit-shift the envelope down by 1 first to strictly 
                    // prevent 32-bit math overflows, then shift by 15 to re-scale the volume).
                    int32_t enveloped_sample = (raw_sample * (voices[i].env_level >> 1)) >> 15;
                    
                    mixed_sample += enveloped_sample;
                    voices[i].playhead++; 
                } else {
                    voices[i].is_playing = false; 
                }
            }
        }

        if (anything_playing) {
            // Because we now have 4 voices, keep the standard divide by 2 headroom 
            // and let the hard clipper catch the absolute extremes.
            mixed_sample = mixed_sample / 2;

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