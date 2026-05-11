#include <cstdint>
#include <cstdlib> // For abs()
#include "ComputerCard.h"
#include "corpus_data.h"

constexpr int DOWNSAMPLE_FACTOR = 2; 
constexpr int NUM_VOICES = 4; 

// 48000 samples = exactly 1 second of delay time at 48kHz
constexpr uint32_t DELAY_BUFFER_SIZE = 48000; 
int16_t delay_buffer[DELAY_BUFFER_SIZE] = {0};

enum EnvState { OFF, ATTACK, HOLD, DECAY };

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

    // --- Core Parameters ---
    int32_t base_x = 2048; 
    int32_t base_y = 2048;
    int32_t spread = 0; 

    int32_t secondary_param_x = 0; 
    int32_t secondary_param_y = 0;
    int32_t secondary_param_main = 0; 
    
    int32_t secondary_param_ratchet = 2048; 

    // --- UI State Tracking (Soft Takeover) ---
    Switch last_switch_state = Switch::Middle;
    bool x_knob_active = false;
    bool y_knob_active = false;
    bool main_knob_active = false;
    const int32_t TAKEOVER_THRESHOLD = 100; 

    // --- Playback State ---
    int32_t held_x = 0;
    int32_t held_y = 0;
    int32_t last_played_target = -1;        
    uint32_t ratchet_counter = 48000;       
    uint32_t eos_timer = 0;

    // Helper for Soft Takeover
    void UpdateParam(int32_t& stored_param, Knob knob, bool& is_active) {
        int32_t physical_pos = KnobVal(knob);
        if (!is_active) {
            // Check if user has "picked up" the value by moving physical knob close to stored value
            if (std::abs(physical_pos - stored_param) < TAKEOVER_THRESHOLD) {
                is_active = true;
            }
        }
        if (is_active) {
            stored_param = physical_pos;
        }
    }

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
        // 1. Consistency: Read switch once
        Switch current_switch = SwitchVal();

        // 2. Soft Takeover: Reset activation on page change
        if (current_switch != last_switch_state) {
            x_knob_active = false;
            y_knob_active = false;
            main_knob_active = false;
            last_switch_state = current_switch;
        }

        // 3. UI Paging
        if (current_switch == Switch::Up) {
            UpdateParam(secondary_param_x, Knob::X, x_knob_active);
            UpdateParam(secondary_param_y, Knob::Y, y_knob_active);
            UpdateParam(secondary_param_main, Knob::Main, main_knob_active);
        } 
        else if (current_switch == Switch::Down) {
            UpdateParam(secondary_param_ratchet, Knob::Main, main_knob_active);
        }
        else { 
            UpdateParam(base_x, Knob::X, x_knob_active);
            UpdateParam(base_y, Knob::Y, y_knob_active);
            UpdateParam(spread, Knob::Main, main_knob_active);
        }
        
        // 4. Trigger Logic (with Deadzone and Safe Modulo)
        bool fire_voice = false;
        int target_to_play = -1;

        if (PulseIn1RisingEdge()) {
            // Hardware Deadzone for spread knob
            int32_t deadzoned_spread = (spread < 60) ? 0 : spread;

            int32_t cv_mod_x = (CVIn1() * deadzoned_spread) / 4096;
            int32_t cv_mod_y = (CVIn2() * deadzoned_spread) / 4096;
            
            held_x = base_x + cv_mod_x; 
            held_y = base_y + cv_mod_y;

            constexpr int32_t MAP_MAX = 4096;
            // O(1) Wrap Fix
            held_x = (held_x % MAP_MAX + MAP_MAX) % MAP_MAX;
            held_y = (held_y % MAP_MAX + MAP_MAX) % MAP_MAX;

            target_to_play = find_nearest_sample(held_x, held_y);
            if (target_to_play >= 0) {
                last_played_target = target_to_play; 
                fire_voice = true;
            }
        }

        // 5. Ratchet Logic (with Clamping)
        if (current_switch == Switch::Down) {
            if (last_played_target < 0) {
                held_x = base_x;
                held_y = base_y;
                target_to_play = find_nearest_sample(held_x, held_y);
                if (target_to_play >= 0) {
                    last_played_target = target_to_play; 
                    fire_voice = true;
                    ratchet_counter = 0; 
                }
            } 
            else {
                ratchet_counter++;
                int32_t safe_ratchet = (secondary_param_ratchet > 4095) ? 4095 : secondary_param_ratchet;
                uint32_t ratchet_interval = 1000 + ((4095 - safe_ratchet) * 5);
                if (ratchet_counter >= ratchet_interval) {
                    target_to_play = last_played_target;
                    fire_voice = true;
                    ratchet_counter = 0; 
                }
            }
        } else {
            ratchet_counter = 48000; 
        }

        // 6. Voice Allocation
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

        // 7. Audio Processing & Mixing
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
                            voices[i].env_state = HOLD; 
                        }
                    } 
                    else if (voices[i].env_state == HOLD) {
                        uint32_t decay_cycles_needed = 65535 / decay_step;
                        uint32_t cycles_remaining = (voices[i].length - flash_index) * DOWNSAMPLE_FACTOR;
                        if (cycles_remaining <= decay_cycles_needed) voices[i].env_state = DECAY;
                    }
                    else if (voices[i].env_state == DECAY) {
                        voices[i].env_level -= decay_step;
                        if (voices[i].env_level <= 0) {
                            voices[i].env_level = 0;
                            voices[i].is_playing = false; 
                            eos_timer = 480; 
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
                    eos_timer = 480; 
                }
            }
        }

        // 8. Outputs & Gain
        PulseOut1(anything_playing); 
        if (eos_timer > 0) {
            PulseOut2(true);
            eos_timer--;
        } else {
            PulseOut2(false);
        }

        int32_t dry_sample = 0;
        if (anything_playing) {
            mixed_sample = mixed_sample / 2; 
            if (mixed_sample > INT16_MAX) mixed_sample = INT16_MAX;
            if (mixed_sample < INT16_MIN) mixed_sample = INT16_MIN;
            dry_sample = mixed_sample;
        } else {
            LedOff(0);
        }

        // 9. Delay (with Clamp)
        int32_t safe_main = (secondary_param_main > 4095) ? 4095 : secondary_param_main;
        uint32_t current_delay_time = (safe_main * (DELAY_BUFFER_SIZE - 1)) / 4095;
        uint32_t read_ptr = (delay_ptr + DELAY_BUFFER_SIZE - current_delay_time) % DELAY_BUFFER_SIZE;
        
        int32_t wet_sample = delay_buffer[read_ptr];
        int32_t delay_input = dry_sample + ((wet_sample * 5) / 8);
        
        if (delay_input > INT16_MAX) delay_input = INT16_MAX;
        if (delay_input < INT16_MIN) delay_input = INT16_MIN;
        delay_buffer[delay_ptr] = (int16_t)delay_input;
        delay_ptr = (delay_ptr + 1) % DELAY_BUFFER_SIZE;

        AudioOut1((int16_t)dry_sample);
        AudioOut2((int16_t)wet_sample);
    }
};

int main() {
    CorpusExplorer moduleProgram;
    moduleProgram.Run(); 
    return 0;
}