#include "ComputerCard.h"
#include "corpus_data.h"

class CorpusExplorer : public ComputerCard {
private:
    const int16_t* current_sample_data = nullptr;
    uint32_t current_sample_length = 0;
    uint32_t playhead = 0;
    bool is_playing = false;

    // Fast Nearest Neighbor using integer math (Squared Euclidean Distance)
    int find_nearest_sample(int32_t cv_x, int32_t cv_y) {
        int best_index = 0;
        
        // Use a large starting number for our minimum distance
        int32_t min_dist_sq = 2147483647; 

        for (int i = 0; i < NUM_SAMPLES; i++) {
            // Calculate distance without floating point math
            int32_t dx = cv_x - corpus[i].x;
            int32_t dy = cv_y - corpus[i].y;
            
            // We don't need std::sqrt(), comparing squared distances works perfectly
            int32_t dist_sq = (dx * dx) + (dy * dy);
            
            if (dist_sq < min_dist_sq) {
                min_dist_sq = dist_sq;
                best_index = i;
            }
        }
        return best_index;
    }

public:
    // This is the core audio callback. 
    // It is called automatically by the library at 48kHz.
    void ProcessSample() override {
        
        // 1. TRIGGER LOGIC: Check if Pulse 1 just received a rising voltage edge
        if (PulseIn1RisingEdge()) {
            
            // Read current CV voltages (Returns values from -2048 to 2047)
            int32_t current_x = CVIn1(); 
            int32_t current_y = CVIn2();

            // Find the closest sample in the flash array
            int target = find_nearest_sample(current_x, current_y);

            // Prime the playhead
            current_sample_data = corpus[target].data;
            current_sample_length = corpus[target].length;
            playhead = 0;
            is_playing = true;
            
            LedOn(0); // Turn on LED 0 to indicate a trigger hit
        }

        // 2. AUDIO PLAYBACK LOGIC
        if (is_playing && playhead < current_sample_length) {
            
            // Grab the current frame of audio from flash memory via XIP
            int16_t sample_val = current_sample_data[playhead];
            
            // Send the audio to the output jacks (-2048 to 2047 limits)
            AudioOut1(sample_val);
            AudioOut2(sample_val);
            
            playhead++;
            
        } else {
            // If we reached the end of the sample (or haven't started), output silence
            AudioOut1(0);
            AudioOut2(0);
            
            if (is_playing) {
                is_playing = false;
                LedOff(0); // Turn off the trigger LED
            }
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