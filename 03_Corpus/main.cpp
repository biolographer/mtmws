#include <cstdint>
#include "ComputerCard.h"
#include "corpus_data.h"

constexpr int DOWNSAMPLE_FACTOR = 2; 
constexpr int NUM_VOICES = 4; 

// 48000 samples = exactly 1 second of delay time at 48kHz
constexpr uint32_t DELAY_BUFFER_SIZE = 48000; 
int16_t delay_buffer[DELAY_BUFFER_SIZE] = {0};

enum EnvState { OFF, ATTACK, DECAY };

struct Voice {
    const int16_t* data = nullptr;
    uint32_t length = 0;
    uint32_t playhead = 0;
    bool is_playing = false;
    
    EnvState env_state = OFF;
    int32_t env_level = 0; 
};

class CorpusExplorer : public ComputerCard {
private:
    Voice voices[NUM_VOICES];
    int next_voice = 0; 

    uint32_t delay_ptr = 0;

    int32_t held_x = 0;
    int32_t held_y = 0;
    int32_t base_x = 2048; 
    int32_t base_y = 2048;
    int32_t spread = 0; 

    int32_t secondary_param_x = 0; 
    int32_t secondary_param_y = 0;
    int32_t secondary_param_main = 0; 
    
    int32_t secondary_param_ratchet = 2048; 
    int32_t last_played_target = -1;        
    uint32_t ratchet_counter = 48000;       

    // --- NEW: End Of Sample (EOS) Timer ---
    uint32_t eos_timer = 0;

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
            secondary_param_x = PotX(); 
            secondary_param_y = PotY(); 
            secondary_param_main = PotMain(); 
        } 
        else if (SwitchDown()) {
            secondary_param_ratchet = PotMain(); 
        }
        else { 
            base_x = PotX();
            base_y = PotY();
            spread = PotMain(); 
        }
        
        // --- 2. TRIGGER LOGIC ---
        bool fire_voice = false;
        int target_to_play = -1;

        if (PulseIn1RisingEdge()) {
            int32_t cv_mod_x = (CVIn1() * spread) / 4096;
            int32_t cv_mod_y = (CVIn2() * spread) / 4096;
            
            held_x = base_x + cv_mod_x; 
            held_y = base_y + cv_mod_y;

            constexpr int32_t MAP_MAX = 4096;
            while (held_x >= MAP_MAX) held_x -= MAP_MAX;
            while (held_x < 0) held_x += MAP_MAX;
            while (held_y >= MAP_MAX) held_y -= MAP_MAX;
            while (held_y < 0) held_y += MAP_MAX;

            target_to_play = find_nearest_sample(held_x, held_y);

            if (target_to_play >= 0) {
                last_played_target = target_to_play; 
                fire_voice = true;
            }
        }

        // --- Condition B: The Ratchet & Auto-Start Switch ---
        if (SwitchDown()) {
            
            // THE AUTO-START: If the module just booted and nothing has played yet
            if (last_played_target < 0) {
                // Grab the current base knob positions to find our first sample
                held_x = base_x;
                held_y = base_y;
                target_to_play = find_nearest_sample(held_x, held_y);
                
                if (target_to_play >= 0) {
                    last_played_target = target_to_play; // Memorize it!
                    fire_voice = true;
                    ratchet_counter = 0; // Reset timer so ratchet can begin
                }
            } 
            // THE RATCHET: A sample has played before, do the normal stutter
            else {
                ratchet_counter++;
                uint32_t ratchet_interval = 1000 + ((4095 - secondary_param_ratchet) * 5);

                if (ratchet_counter >= ratchet_interval) {
                    target_to_play = last_played_target;
                    fire_voice = true;
                    ratchet_counter = 0; 
                }
            }
            
        } else {
            // Switch is not held down
            ratchet_counter = 48000; 
        }

        // --- 3. VOICE ALLOCATION ---
        if (fire_voice && target_to_play >= 0) {
            voices[next_voice].data = corpus[target_to_play].data;
            voices[next_voice].length = corpus[target_to_play].length;
            voices[next_voice].playhead = 0;
            voices[next_voice].is_playing = true;
            
            voices[next_voice].env_state = ATTACK;
            voices[next_voice].env_level = 0; 

            next_voice = (next_voice + 1) % NUM_VOICES;
            LedOn(0); 
        }

        // --- 4. DRY AUDIO PLAYBACK & MIXING ---
        int32_t mixed_sample = 0; 
        bool anything_playing = false;

        int32_t attack_step = 4096 - secondary_param_x; 
        if (attack_step < 1) attack_step = 1;
        
        int32_t decay_step = (4096 - secondary_param_y) / 2; 
        if (decay_step < 1) decay_step = 1;

        for (int i = 0; i < NUM_VOICES; i++) {
            if (voices[i].is_playing) {
                anything_playing = true;
                uint32_t flash_index = voices[i].playhead / DOWNSAMPLE_FACTOR;
                
                if (flash_index < voices[i].length) {
                    
                    if (voices[i].env_state == ATTACK) {
                        voices[i].env_level += attack_step;
                        if (voices[i].env_level >= 65535) {
                            voices[i].env_level = 65535; 
                            voices[i].env_state = DECAY; 
                        }
                    } 
                    else if (voices[i].env_state == DECAY) {
                        voices[i].env_level -= decay_step;
                        if (voices[i].env_level <= 0) {
                            voices[i].env_level = 0;
                            voices[i].is_playing = false; 
                            eos_timer = 480; // Trigger EOS! (Envelope naturally finished)
                        }
                    }

                    uint32_t samples_left = voices[i].length - flash_index;
                    if (samples_left < 256) {
                        voices[i].env_level = (voices[i].env_level * samples_left) / 256;
                    }

                    int32_t raw_sample = voices[i].data[flash_index];
                    int32_t enveloped_sample = (raw_sample * (voices[i].env_level >> 1)) >> 15;
                    
                    mixed_sample += enveloped_sample;
                    voices[i].playhead++; 
                } else {
                    voices[i].is_playing = false; 
                    eos_timer = 480; // Trigger EOS! (Ran out of sample data)
                }
            }
        }

        // --- 5. THE OUTPUTS (Gate & Triggers) ---
        
        // Gate Out: HIGH if any sound is currently being generated.
        PulseOut1(anything_playing); 

        // EOS Trigger Out: Stays HIGH for 480 samples (~10ms) when a voice ends.
        if (eos_timer > 0) {
            PulseOut2(true);
            eos_timer--;
        } else {
            PulseOut2(false);
        }

        // --- 6. AUDIO GAIN STAGING ---
        int32_t dry_sample = 0;
        if (anything_playing) {
            mixed_sample = mixed_sample / 2; // Headroom
            if (mixed_sample > INT16_MAX) mixed_sample = INT16_MAX;
            if (mixed_sample < INT16_MIN) mixed_sample = INT16_MIN;
            dry_sample = mixed_sample;
        } else {
            LedOff(0);
        }

        // --- 7. THE VARIABLE DELAY LINE ---
        uint32_t current_delay_time = (secondary_param_main * (DELAY_BUFFER_SIZE - 1)) / 4095;
        uint32_t read_ptr = (delay_ptr + DELAY_BUFFER_SIZE - current_delay_time) % DELAY_BUFFER_SIZE;
        int32_t wet_sample = delay_buffer[read_ptr];
        
        int32_t delay_input = dry_sample + ((wet_sample * 5) / 8);
        
        if (delay_input > INT16_MAX) delay_input = INT16_MAX;
        if (delay_input < INT16_MIN) delay_input = INT16_MIN;
        delay_buffer[delay_ptr] = (int16_t)delay_input;
        
        delay_ptr = (delay_ptr + 1) % DELAY_BUFFER_SIZE;

        // --- 8. AUDIO ROUTING ---
        AudioOut1((int16_t)dry_sample);
        AudioOut2((int16_t)wet_sample);
    }
};

int main() {
    CorpusExplorer moduleProgram;
    moduleProgram.Run(); 
    return 0;
}