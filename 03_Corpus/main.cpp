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

    // ... [find_nearest_sample function remains exactly the same] ...

public:
    void ProcessSample() override {
        
        // 1. TRIGGER LOGIC
        if (PulseIn1RisingEdge()) {
            int32_t current_x = CVIn1(); 
            int32_t current_y = CVIn2();
            int target = find_nearest_sample(current_x, current_y);

            current_sample_data = corpus[target].data;
            current_sample_length = corpus[target].length;
            playhead = 0;
            is_playing = true;
            LedOn(0); 
        }

        // 2. AUDIO PLAYBACK LOGIC
        if (is_playing) {
            // Calculate the actual index in the flash array based on the factor
            uint32_t flash_index = playhead / DOWNSAMPLE_FACTOR;
            
            // Check if we haven't reached the end of the stored sample
            if (flash_index < current_sample_length) {
                
                int16_t sample_val = current_sample_data[flash_index];
                AudioOut1(sample_val);
                AudioOut2(sample_val);
                
                playhead++; // The playhead ALWAYS moves at 48kHz
                
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