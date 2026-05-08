#include <cstdint>
#include "ComputerCard.h"
#include "corpus_data.h"

// Set this to match the `-d` flag you used in your Python script!
// 1 = 48kHz (Original)
// 2 = 24kHz (Lo-fi, double the storage)
// 4 = 12kHz (Very crunchy, quadruple storage)
constexpr int DOWNSAMPLE_FACTOR = 2; 

class CorpusExplorer : public ComputerCard {
private:
    const int16_t* current_sample_data = nullptr;
    uint32_t current_sample_length = 0;
    uint32_t playhead = 0;
    bool is_playing = false;

    // --- SAMPLE & HOLD STATE VARIABLES ---
    // We store these here so they stay locked until the next trigger.
    int32_t held_x = 0;
    int32_t held_y = 0;

    inline int find_nearest_sample(int32_t target_x, int32_t target_y) {
        if (NUM_SAMPLES <= 0) return 0;

        int best_index = 0;
        
        // Use the standard macro for the maximum possible 32-bit integer
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
        
        // 1. TRIGGER & SAMPLE-AND-HOLD LOGIC
        if (PulseIn1RisingEdge()) {
            
            // SAMPLE: Read the Pots AND the CV inputs at this exact moment.
            // (Assuming PotX/Y and CVIn return compatible ranges, e.g., 12-bit or 16-bit)
            held_x = PotX() + CVIn1(); 
            held_y = PotY() + CVIn2();
            
            // Find the sample based on our newly locked coordinates
            int target = find_nearest_sample(held_x, held_y);

            current_sample_data = corpus[target].data;
            current_sample_length = corpus[target].length;
            playhead = 0;
            is_playing = true;
            LedOn(0); 
        }

        // 2. AUDIO PLAYBACK LOGIC
        if (is_playing) {
            // Calculate the actual index in the flash array based on the factor
            // (e.g., if factor is 2, it holds each sample for two ticks creating a 24kHz rate)
            uint32_t flash_index = playhead / DOWNSAMPLE_FACTOR;
            
            // Check if we haven't reached the end of the stored sample
            if (flash_index < current_sample_length) {
                
                int16_t sample_val = current_sample_data[flash_index];
                AudioOut1(sample_val);
                AudioOut2(sample_val);
                
                playhead++; 
                
            } else {
                // End of sample
                AudioOut1(0);
                AudioOut2(0);
                is_playing = false;
                LedOff(0);
            }
        } else {
             // Not playing
             AudioOut1(0);
             AudioOut2(0);
        }
    }
};

// 3. THE BOOT SEQUENCE
int main() {
    CorpusExplorer moduleProgram;
    
    // Run() initializes the hardware and starts the infinite DMA loop.
    // The program will never execute past this line.
    moduleProgram.Run(); 
    return 0;
}